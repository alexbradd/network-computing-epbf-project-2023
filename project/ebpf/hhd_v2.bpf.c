#include <linux/bpf.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/icmp.h>
#include <linux/icmpv6.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <stddef.h>
#include <stdint.h>

#include "fasthash.h"
#include "hhd_v2_utils.bpf.h"
#include "jhash.h"

#define BLOOM_FILTER_ENTRIES 4096
#define FASTHASH_SEED 0xdeadbeef
#define JHASH_SEED 0x2d31e867

const volatile struct { 
    __u64 threshold; 
} hhd_v2_cfg = {};

/* TODO 6: Define a C struct for the 5-tuple
 * (source IP, destination IP, source port, destination port, protocol).
 */
struct flow_info {
    // Everything is saved in network order
    __u32 source_ip; 
    __u32 dest_ip;
    __u16 source_port;
    __u16 dest_port;
    __u8  protocol;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, __u32);
    __type(value, __u64);
    __uint(max_entries, BLOOM_FILTER_ENTRIES);
} bloom_filter_map SEC(".maps");

static __always_inline int parse_ethhdr(void *data, void *data_end, __u16 *nh_off,
                                        struct ethhdr **ethhdr) {
    struct ethhdr *eth = (struct ethhdr *)data;
    int hdr_size = sizeof(*eth);

    /* Byte-count bounds check; check if current pointer + size of header
     * is after data_end.
     */
    if ((void *)eth + hdr_size > data_end)
        return -1;

    *nh_off += hdr_size;
    *ethhdr = eth;

    return eth->h_proto; /* network-byte-order */
}

static __always_inline int parse_iphdr(void *data, void *data_end, __u16 *nh_off,
                                       struct iphdr **iphdr) {
    /* TODO 4: Implement the parse_iphdr header function */
    struct iphdr *ip = (struct iphdr *)data;
    int hdr_size;
    
    if ((void*)ip + sizeof(struct iphdr) > data_end)
        return -1;
    hdr_size = ip->ihl * 4;
    if (hdr_size < sizeof(struct iphdr))
        return -1;
    if ((void*)ip + hdr_size > data_end)
        return -1;

    *nh_off += hdr_size;
    *iphdr = ip;

    return ip->protocol;
}

static __always_inline int parse_tcphdr(void *data, void *data_end, __u16 *nh_off,
                                        struct tcphdr **tcphdr) {
    /* TODO 9: Implement the parse_tcphdr header function */
    struct tcphdr *tcp = (struct tcphdr *)data;
    int hdr_size;

    /* TODO 10: Make sure you check the actual size of the TCP header
     * The TCP header size is stored in the doff field, which is a 4-bit field
     * that stores the number of 32-bit words in the TCP header.
     * The minimum size of the TCP header is 5 words (20 bytes) and the maximum
     * is 15 words (60 bytes).
     */
    if ((void*)tcp + sizeof(struct tcphdr) > data_end)
        return -1;
    hdr_size = tcp->doff * 4;
    if (hdr_size < sizeof(struct tcphdr) || hdr_size > 60)
        return -1;
    if ((void*)tcp + hdr_size > data_end)
        return -1;

    *nh_off += hdr_size;
    *tcphdr = tcp;

    return hdr_size;
}

static __always_inline int parse_udphdr(void *data, void *data_end, __u16 *nh_off,
                                        struct udphdr **udphdr) {
    /* TODO 12: Implement the parse_udphdr header function */
    struct udphdr *udp = (struct udphdr *)data;
    int hdr_size = sizeof(struct udphdr);

    if ((void *)udp + hdr_size > data_end)
        return -1;
    if (bpf_ntohs(udp->len) < hdr_size)
        return -1;

    *nh_off += hdr_size;
    *udphdr = udp;

    return hdr_size;
}

static __always_inline __u64* lookup_and_increment_counter(__u32 *hash) {
    __u64* threshold = (__u64*)bpf_map_lookup_elem(&bloom_filter_map, hash);
    if (threshold)
        __sync_fetch_and_add(threshold, 1);
    return threshold;
}

SEC("xdp")
int xdp_hhd_v2(struct xdp_md *ctx) {
    __u16 nf_off = 0;
    struct ethhdr *eth;
    __u16 eth_type;
    struct ipv4_lookup_val *val;
    struct src_mac_val *src_mac_val;
    __u16 src_mac_key;
    int action = XDP_PASS;
    __u32 ipv4_lookup_map_key;

    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    struct iphdr *ip;
    struct tcphdr *tcp;
    struct udphdr *udp;
    int ip_type, transport_hdr_size;

    bpf_printk("Packet received from interface (ifindex) %d", ctx->ingress_ifindex);

    eth_type = parse_ethhdr(data, data_end, &nf_off, &eth);

    if (data + sizeof(struct ethhdr) > data_end) {
        bpf_printk("Packet is not a valid Ethernet packet");
        return XDP_DROP;
    }

    /* TODO 1: Check if the packet is ARP.
     * If it is, return XDP_PASS.
     */   
    if (eth_type == bpf_htons(ETH_P_ARP)) {
        bpf_printk("ARP packet detected, passing");
        return XDP_PASS;
    }

    /* TODO 2: Check if the packet is IPv4.
     * If it is, continue with the program.
     * If it is not, return XDP_DROP.
     */
    if (eth_type != bpf_htons(ETH_P_IP)) {
        bpf_printk("Non IPv4 (is %x) packet detected, dropping", bpf_ntohs(eth_type));
        return XDP_DROP;
    }

    /* TODO 3: Parse the IPv4 header.
     * If the packet is not a valid IPv4 packet, return XDP_DROP.
     */    
    bpf_printk("Parsing IP packet...");
    ip_type = parse_iphdr(data + nf_off, data_end, &nf_off, &ip);
    if (ip_type < 0) {
        bpf_printk("Packet is not a valid IPv4 packet, dropping");
        return XDP_DROP;
    }

    /* TODO 5: Define a C struct for the 5-tuple
     * (source IP, destination IP, source port, destination port, protocol).
     * Fill the struct with the values from the packet.
     */
    struct flow_info info;
    info.source_ip = ip->addrs.saddr;
    info.dest_ip   = ip->addrs.daddr;
    info.protocol  = ip_type;

    /* TODO 7: Check if the packet is TCP or UDP
     * If it is, fill the 5-tuple struct with the values from the packet.
     * If it is not, goto forward.
     */
    if (ip_type == IPPROTO_TCP) {
        /* TODO 8: If the packet is TCP, parse the TCP header */
        bpf_printk("Parsing TCP packet...");
        transport_hdr_size = parse_tcphdr(data + nf_off, data_end, &nf_off, &tcp);
        if (transport_hdr_size < 0) {
            bpf_printk("Packet is not a valid TCP packet, dropping");
            return XDP_DROP;
        }
        info.source_port = tcp->source;
        info.dest_port = tcp->dest;
    } else if (ip_type == IPPROTO_UDP) {
        /* TODO 11: If the packet is UDP, parse the UDP header */
        bpf_printk("Parsing UDP packet...");
        transport_hdr_size = parse_udphdr(data + nf_off, data_end, &nf_off, &udp);
        if (transport_hdr_size < 0) {
            bpf_printk("Packet is not a valid UDP packet, dropping");
            return XDP_DROP;
        }
        info.source_port = udp->source;
        info.dest_port = udp->dest;
    } else {
        bpf_printk("Not TCP/UDP packet (is %x), forwarding", ip_type);
        goto forward;
    }
    bpf_printk("Identified flow %pI4:%u -> %pI4:%u on %u, running HHD",
        info.source_ip,
        bpf_ntohs(info.source_port),
        info.dest_ip,
        bpf_ntohs(info.dest_port),
        info.protocol);

    /* TODO 13: Let's apply the heavy hitter detection algorithm
     * You can use two different hash functions for this.
     * You can use the jhash function and the fasthash function.
     * Both functions are already imported and ready to use.
     * The first parameter of both functions is the data to hash.
     * The second parameter is the size of the data to hash.
     * The third parameter is the seed to use for the hash function.
     * You can use the define values FASTHASH_SEED and JHASH_SEED for the seed.
     */
    __u32 h_fasthash = fasthash32(&info, sizeof(struct flow_info), FASTHASH_SEED) % BLOOM_FILTER_ENTRIES;
    __u32 h_jhash = jhash(&info, sizeof(struct flow_info), JHASH_SEED) % BLOOM_FILTER_ENTRIES;
    if (h_fasthash >= BLOOM_FILTER_ENTRIES || h_jhash >= BLOOM_FILTER_ENTRIES)
        return XDP_ABORTED;

    __u64 *counter_fasthash = lookup_and_increment_counter(&h_fasthash);
    if(counter_fasthash == NULL)
        return XDP_ABORTED;

    __u64 *counter_jhash = lookup_and_increment_counter(&h_jhash);
    if(counter_jhash == NULL)
        return XDP_ABORTED;

    /* TODO 14: Check if the values from the bloom filter are above the threshold
     * If they are, the packet is part of a DDoS attack, so drop it.
     * If they are not, the packet is not part of a DDoS attack, so let it pass
     * (goto forward). You can use the hhd_v2_cfg.threshold variable for the
     * threshold value.
     */
    if (
        *counter_fasthash > hhd_v2_cfg.threshold &&
        *counter_jhash > hhd_v2_cfg.threshold
    ) {
        bpf_printk("Possible DoS found, dropping packet");
        return XDP_DROP;
    }
    bpf_printk("Forwarding packet");

forward:
    /* TODO 15: Copy inside the ipv4_lookup_map_key variable the destination IP
     * address of the packet The value should be in network byte order. E.g.,
     * ipv4_lookup_map_key = flow.daddr;
     */
    ipv4_lookup_map_key = info.dest_ip;

    /* From here on, you don't need to modify anything
     * The following code will check if the destination IP is in the hash map.
     * If it is, it will forward the packet to the correct interface.
     * If it is not, it will drop the packet.
     */

    /* In this case the packet is allowed to pass, let's see if the hash map
     * contains the dst ip */
    val = bpf_map_lookup_elem(&ipv4_lookup_map, &ipv4_lookup_map_key);

    if (!val) {
        bpf_printk("Error looking up destination IP in map");
        action = XDP_ABORTED;
        goto out;
    }

    if (val->outPort < 1 || val->outPort > 4) {
        bpf_printk("Error looking up destination port in map");
        action = XDP_ABORTED;
        goto out;
    }

    src_mac_key = val->outPort;
    src_mac_val = bpf_map_lookup_elem(&src_mac_map, &src_mac_key);

    if (!src_mac_val) {
        bpf_printk("Error looking up source MAC in map with key: %d", src_mac_key);
        action = XDP_ABORTED;
        goto out;
    }

    __builtin_memcpy(eth->h_source, src_mac_val->srcMac, ETH_ALEN);
    __builtin_memcpy(eth->h_dest, val->dstMac, ETH_ALEN);

    bpf_printk("Packet forwarded to interface %d", val->outPort);

    action = bpf_redirect_map(&devmap, val->outPort, 0);

    if (action != XDP_REDIRECT) {
        bpf_printk("Error redirecting packet");
        action = XDP_ABORTED;
        goto out;
    }

out:
    return action;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
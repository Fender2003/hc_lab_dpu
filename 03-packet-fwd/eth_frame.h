#pragma once

#include <doca_types.h>
#include <endian.h>

struct udp_segment {
    doca_be16_t source_port;
    doca_be16_t dest_port;
    doca_be16_t length;
    doca_be16_t checksum;
    uint8_t data[];
} __attribute__((packed)) __attribute__((aligned(2)));

struct ipv4_packet {
    uint8_t version_ihl;
    uint8_t dscp_en;
    doca_be16_t total_length;
    doca_be16_t identification;
    doca_be16_t flags_fragment_offset;
    uint8_t ttl;
    uint8_t protocol;
    doca_be16_t header_checksum;
    doca_be32_t source_address;
    doca_be32_t dest_address;
    struct udp_segment segment;
} __attribute__((packed)) __attribute__((aligned(2)));

struct eth_frame {
    uint8_t dest_mac[6];
    uint8_t source_mac[6];
    doca_be16_t ethertype;
    struct ipv4_packet packet;
} __attribute__((packed)) __attribute__((aligned(2)));

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_PORT_UDP_H
#define KERVAN_PORT_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kervan/node.h"
#include "kervan/pal.h"
#include "kervan/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PC port: the "air" is UDP on 127.0.0.1. Each node binds its own port and transmits every
 * frame to an explicit list of peer ports, so the peer lists define who hears whom.
 * Links need not be symmetric: A may hear B while B does not hear A.
 */

#ifndef KV_UDP_MAX_PEERS
#define KV_UDP_MAX_PEERS 8u
#endif

/* Port state. Allocate it yourself; treat the fields as private. */
typedef struct {
    uintptr_t sock; /* SOCKET (Windows) or file descriptor; OS headers stay out of here */
    bool open;
    uint16_t port;
    uint16_t peers[KV_UDP_MAX_PEERS];
    size_t peer_count;
    bool tx_done_pending; /* reported to the node from the poll loop, never from radio_send */
    uint32_t rng;
    kv_pal_t pal;
    kv_node_t *node;
} kv_udp_t;

/*
 * Bind 127.0.0.1:port (0 = pick a free port, see kv_udp_port). mtu emulates the radio limit,
 * e.g. 51 for LoRa SF12. seed makes the PAL random numbers reproducible.
 */
kv_status_t kv_udp_open(kv_udp_t *udp, uint16_t port, size_t mtu, uint32_t seed);
void kv_udp_close(kv_udp_t *udp);

uint16_t kv_udp_port(const kv_udp_t *udp);
kv_status_t kv_udp_add_peer(kv_udp_t *udp, uint16_t port);

/* Initialize node on top of this port. The node is driven by kv_udp_poll from then on. */
kv_status_t kv_udp_node_init(kv_udp_t *udp, kv_node_t *node, const kv_node_config_t *cfg);

/*
 * One iteration of the main loop: deliver completions and received frames to the node and run
 * its timers. Blocks at most max_wait_ms, less if the node has an earlier deadline.
 */
kv_status_t kv_udp_poll(kv_udp_t *udp, uint32_t max_wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_PORT_UDP_H */

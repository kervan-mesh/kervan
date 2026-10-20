/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_NODE_H
#define KERVAN_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kervan/frame.h"
#include "kervan/pal.h"
#include "kervan/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time limits; override with -D to trade RAM for capacity. */
#ifndef KV_MTU_MAX
#define KV_MTU_MAX 255u /* largest frame of any supported radio */
#endif
#ifndef KV_DEDUP_SIZE
#define KV_DEDUP_SIZE 16u /* remembered (src, seq) pairs */
#endif
#ifndef KV_DEDUP_WINDOW_MS
#define KV_DEDUP_WINDOW_MS 30000u /* how long a pair counts as a duplicate */
#endif

/* A DATA frame addressed to this node (or broadcast) arrived. meta may be NULL. */
typedef void (*kv_data_cb_t)(void *user, const kv_frame_t *frame, const kv_rx_meta_t *meta);

/* Outcome of a send with ack_req: KV_OK when acknowledged, KV_ERR_TIMEOUT after all retries. */
typedef void (*kv_ack_cb_t)(void *user, uint8_t seq, kv_status_t result);

typedef struct {
    uint16_t addr;
    uint8_t net_id;
    uint8_t hop_limit;       /* for frames this node originates */
    uint32_t ack_timeout_ms; /* wait after a transmission ends before retrying */
    uint8_t max_retries;     /* retransmissions after the first attempt */
    kv_data_cb_t on_data;    /* optional */
    kv_ack_cb_t on_ack;      /* optional */
    void *user;
} kv_node_config_t;

typedef struct {
    bool used;
    uint8_t seq;
    uint8_t frag_key;
    uint16_t src;
    uint32_t time_ms;
} kv_dedup_entry_t;

/* Node state. Allocate it yourself (static or stack); treat the fields as private. */
typedef struct {
    kv_pal_t pal;
    kv_node_config_t cfg;
    uint8_t next_seq;
    bool tx_busy;
    uint8_t tx_buf[KV_MTU_MAX];

    /* the one outstanding message waiting for an ACK */
    bool ack_pending;
    bool ack_in_flight; /* its transmission has not finished yet */
    uint8_t ack_seq;
    uint8_t ack_retries_left;
    uint16_t ack_dst;
    uint32_t ack_deadline;
    size_t ack_len;
    uint8_t ack_buf[KV_MTU_MAX];

    kv_dedup_entry_t dedup[KV_DEDUP_SIZE];
    uint8_t dedup_next;
} kv_node_t;

/* Fill cfg with defaults (hop limit 3, 5 s ACK timeout, 3 retries). addr must still be set. */
void kv_node_config_default(kv_node_config_t *cfg);

kv_status_t kv_node_init(kv_node_t *node, const kv_pal_t *pal, const kv_node_config_t *cfg);

/*
 * Send a DATA frame. With ack_req, on_ack reports the outcome later; only one such message can be
 * outstanding. Returns KV_ERR_BUSY while the radio is transmitting. out_seq may be NULL.
 */
kv_status_t kv_node_send(kv_node_t *node, uint16_t dst, const uint8_t *data, size_t len,
                         bool ack_req, uint8_t *out_seq);

/* Port -> core events. Call from the main loop, never from an interrupt. */
void kv_node_on_rx(kv_node_t *node, const uint8_t *buf, size_t len, const kv_rx_meta_t *meta);
void kv_node_on_tx_done(kv_node_t *node);
void kv_node_tick(kv_node_t *node);

/*
 * When kv_node_tick() should be called next. Returns false if nothing is scheduled.
 * Re-query after every call into the node.
 */
bool kv_node_next_deadline(const kv_node_t *node, uint32_t *at_ms);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_NODE_H */

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/node.h"

#include <string.h>

#define FRAG_KEY_NONE 0xFFu

static bool time_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static void node_log(const kv_node_t *n, kv_log_level_t level, const char *msg)
{
    if (n->pal.log != NULL) {
        n->pal.log(n->pal.ctx, level, msg);
    }
}

static kv_status_t transmit(kv_node_t *n, const uint8_t *buf, size_t len)
{
    kv_status_t st = n->pal.radio_send(n->pal.ctx, buf, len);
    if (st == KV_OK) {
        n->tx_busy = true;
    }
    return st;
}

static void header_init(kv_node_t *n, kv_frame_header_t *hdr, uint8_t type, uint16_t dst)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->hop_limit = n->cfg.hop_limit;
    hdr->net_id = n->cfg.net_id;
    hdr->dst = dst;
    hdr->src = n->cfg.addr;
    hdr->seq = n->next_seq++;
}

/* Returns true if the frame was seen within the window; otherwise remembers it. */
static bool dedup_seen(kv_node_t *n, const kv_frame_header_t *hdr, uint32_t now)
{
    uint8_t key = hdr->frag ? hdr->frag_index : FRAG_KEY_NONE;
    kv_dedup_entry_t *e;
    size_t i;

    for (i = 0; i < KV_DEDUP_SIZE; i++) {
        e = &n->dedup[i];
        if (e->used && e->src == hdr->src && e->seq == hdr->seq && e->frag_key == key &&
            !time_reached(now, e->time_ms + KV_DEDUP_WINDOW_MS)) {
            return true;
        }
    }

    /* ring buffer: overwrite the oldest entry */
    e = &n->dedup[n->dedup_next];
    e->used = true;
    e->src = hdr->src;
    e->seq = hdr->seq;
    e->frag_key = key;
    e->time_ms = now;
    n->dedup_next = (uint8_t)((n->dedup_next + 1u) % KV_DEDUP_SIZE);
    return false;
}

static void send_ack(kv_node_t *n, uint16_t dst, uint8_t acked_seq)
{
    kv_frame_t f;
    size_t len;

    if (n->tx_busy) {
        node_log(n, KV_LOG_WARN, "radio busy, ACK dropped");
        return;
    }
    header_init(n, &f.hdr, KV_FRAME_ACK, dst);
    f.payload = &acked_seq;
    f.payload_len = 1;
    if (kv_frame_encode(&f, n->tx_buf, n->pal.mtu, &len) == KV_OK) {
        (void)transmit(n, n->tx_buf, len);
    }
}

static void handle_ack(kv_node_t *n, uint16_t src, uint8_t acked_seq)
{
    if (!n->ack_pending || src != n->ack_dst || acked_seq != n->ack_seq) {
        return;
    }
    n->ack_pending = false; /* before the callback, so it may send again */
    if (n->cfg.on_ack != NULL) {
        n->cfg.on_ack(n->cfg.user, acked_seq, KV_OK);
    }
}

/* Retransmit or give up on the outstanding ack_req message once its deadline passed. */
static void service_ack(kv_node_t *n, uint32_t now)
{
    if (!n->ack_pending || n->ack_in_flight || !time_reached(now, n->ack_deadline)) {
        return;
    }
    if (n->ack_retries_left == 0u) {
        n->ack_pending = false;
        if (n->cfg.on_ack != NULL) {
            n->cfg.on_ack(n->cfg.user, n->ack_seq, KV_ERR_TIMEOUT);
        }
        return;
    }
    if (n->tx_busy) {
        return; /* retried from kv_node_on_tx_done() */
    }
    n->ack_retries_left--;
    if (transmit(n, n->ack_buf, n->ack_len) == KV_OK) {
        n->ack_in_flight = true;
    } else {
        n->ack_deadline = now + n->cfg.ack_timeout_ms;
    }
}

void kv_node_config_default(kv_node_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->hop_limit = KV_HOP_LIMIT_DEFAULT;
    cfg->ack_timeout_ms = 5000u;
    cfg->max_retries = 3u;
}

kv_status_t kv_node_init(kv_node_t *node, const kv_pal_t *pal, const kv_node_config_t *cfg)
{
    if (node == NULL || pal == NULL || cfg == NULL || pal->now_ms == NULL ||
        pal->radio_send == NULL || pal->random == NULL) {
        return KV_ERR_INVALID_ARG;
    }
    if (pal->mtu <= KV_FRAME_HEADER_LEN || pal->mtu > KV_MTU_MAX ||
        cfg->addr == KV_ADDR_BROADCAST || cfg->hop_limit > KV_HOP_LIMIT_MAX) {
        return KV_ERR_INVALID_ARG;
    }

    memset(node, 0, sizeof(*node));
    node->pal = *pal;
    node->cfg = *cfg;
    /* random start: after a reboot, peers' dedup caches must not swallow our new messages */
    node->next_seq = (uint8_t)(pal->random(pal->ctx) & 0xFFu);
    return KV_OK;
}

kv_status_t kv_node_send(kv_node_t *node, uint16_t dst, const uint8_t *data, size_t len,
                         bool ack_req, uint8_t *out_seq)
{
    kv_frame_t f;
    uint8_t *buf;
    size_t frame_len;
    kv_status_t st;

    if (node == NULL || (data == NULL && len > 0u)) {
        return KV_ERR_INVALID_ARG;
    }
    if (ack_req && dst == KV_ADDR_BROADCAST) {
        return KV_ERR_MALFORMED;
    }
    if (node->tx_busy || (ack_req && node->ack_pending)) {
        return KV_ERR_BUSY;
    }
    if (len > node->pal.mtu - KV_FRAME_HEADER_LEN) {
        return KV_ERR_NO_SPACE; /* fragmentation comes in phase 3 */
    }

    header_init(node, &f.hdr, KV_FRAME_DATA, dst);
    f.hdr.ack_req = ack_req;
    f.payload = data;
    f.payload_len = len;

    /* ack_req frames keep their own buffer for retransmission */
    buf = ack_req ? node->ack_buf : node->tx_buf;
    st = kv_frame_encode(&f, buf, node->pal.mtu, &frame_len);
    if (st != KV_OK) {
        return st;
    }
    st = transmit(node, buf, frame_len);
    if (st != KV_OK) {
        return st;
    }

    if (ack_req) {
        node->ack_pending = true;
        node->ack_in_flight = true;
        node->ack_seq = f.hdr.seq;
        node->ack_dst = dst;
        node->ack_len = frame_len;
        node->ack_retries_left = node->cfg.max_retries;
    }
    if (out_seq != NULL) {
        *out_seq = f.hdr.seq;
    }
    return KV_OK;
}

void kv_node_on_rx(kv_node_t *node, const uint8_t *buf, size_t len, const kv_rx_meta_t *meta)
{
    kv_frame_t f;
    const kv_frame_header_t *hdr = &f.hdr;
    bool dup;

    if (node == NULL || kv_frame_decode(buf, len, &f) != KV_OK) {
        return;
    }
    if (hdr->net_id != node->cfg.net_id || hdr->src == node->cfg.addr) {
        return;
    }
    if (hdr->dst != node->cfg.addr && hdr->dst != KV_ADDR_BROADCAST) {
        return; /* relaying comes with the mesh (phase 4) */
    }
    if (hdr->frag) {
        node_log(node, KV_LOG_WARN, "fragments not supported yet");
        return;
    }

    dup = dedup_seen(node, hdr, node->pal.now_ms(node->pal.ctx));

    switch (hdr->type) {
    case KV_FRAME_DATA:
        /* ACK duplicates too: the earlier ACK may have been lost */
        if (hdr->ack_req) {
            send_ack(node, hdr->src, hdr->seq);
        }
        if (!dup && node->cfg.on_data != NULL) {
            node->cfg.on_data(node->cfg.user, &f, meta);
        }
        break;
    case KV_FRAME_ACK:
        if (!dup && f.payload_len >= 1u) {
            handle_ack(node, hdr->src, f.payload[0]);
        }
        break;
    default:
        break; /* unknown types are dropped at the destination */
    }
}

void kv_node_on_tx_done(kv_node_t *node)
{
    uint32_t now;

    if (node == NULL) {
        return;
    }
    now = node->pal.now_ms(node->pal.ctx);
    node->tx_busy = false;
    if (node->ack_in_flight) {
        /* the timeout starts when the frame has left the antenna */
        node->ack_in_flight = false;
        node->ack_deadline = now + node->cfg.ack_timeout_ms;
    }
    service_ack(node, now);
}

void kv_node_tick(kv_node_t *node)
{
    if (node != NULL) {
        service_ack(node, node->pal.now_ms(node->pal.ctx));
    }
}

bool kv_node_next_deadline(const kv_node_t *node, uint32_t *at_ms)
{
    /* while transmitting, kv_node_on_tx_done() drives the next step */
    if (node == NULL || at_ms == NULL || !node->ack_pending || node->ack_in_flight ||
        node->tx_busy) {
        return false;
    }
    *at_ms = node->ack_deadline;
    return true;
}

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/node.h"

#include <string.h>

#define FRAG_KEY_NONE 0xFFu
#define CREDIT_PER_MS 1000 /* credit units per ms of airtime */
#define CREDIT_MIN (-1000000000L)

/* ---- helpers ---- */

static bool time_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static uint32_t later(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0 ? a : b;
}

static uint32_t now_ms(const kv_node_t *n)
{
    return n->pal.now_ms(n->pal.ctx);
}

static uint32_t random32(const kv_node_t *n)
{
    return n->pal.random(n->pal.ctx);
}

static void node_log(const kv_node_t *n, kv_log_level_t level, const char *msg)
{
    if (n->pal.log != NULL) {
        n->pal.log(n->pal.ctx, level, msg);
    }
}

/* payload bytes per fragment */
static size_t chunk_size(const kv_node_t *n)
{
    return n->pal.mtu - KV_FRAME_HEADER_LEN - KV_FRAME_FRAG_HEADER_LEN;
}

static uint8_t frag_key(const kv_frame_header_t *hdr)
{
    return hdr->frag ? hdr->frag_index : FRAG_KEY_NONE;
}

/* ---- airtime budget (token bucket; docs/messaging.md) ---- */

static void budget_refill(kv_node_t *n, uint32_t now)
{
    int64_t credit;
    int64_t cap;

    if (n->cfg.duty_cycle_permille == 0u) {
        return;
    }
    credit = (int64_t)n->credit + (int64_t)(uint32_t)(now - n->credit_time) *
                                      (int64_t)n->cfg.duty_cycle_permille;
    cap = (int64_t)n->cfg.duty_burst_ms * CREDIT_PER_MS;
    if (cap > INT32_MAX) {
        cap = INT32_MAX;
    }
    n->credit = (int32_t)(credit > cap ? cap : credit);
    n->credit_time = now;
}

/*
 * A transmission ended: credit earned while it was on air minus its airtime. The off-time is then
 * airtime * (1000 - p) / p, the classic duty-cycle rule (99 x the airtime at 1 %).
 */
static void budget_charge(kv_node_t *n, uint32_t now, uint32_t airtime_ms)
{
    int64_t credit;

    if (n->cfg.duty_cycle_permille == 0u) {
        return;
    }
    credit = (int64_t)n->credit +
             (int64_t)(uint32_t)(now - n->credit_time) * (int64_t)n->cfg.duty_cycle_permille -
             (int64_t)airtime_ms * CREDIT_PER_MS;
    n->credit = (int32_t)(credit < CREDIT_MIN ? CREDIT_MIN : credit);
    n->credit_time = now;
    budget_refill(n, now); /* applies the cap */
}

static bool budget_ok(const kv_node_t *n)
{
    return n->cfg.duty_cycle_permille == 0u || n->credit >= 0;
}

/* ACKs prevent retransmissions that would cost far more airtime, so they may run into a
 * bounded debt; it is repaid as extra off-time. */
static bool ack_budget_ok(const kv_node_t *n)
{
    int64_t limit = -(int64_t)n->cfg.ack_debt_ms * CREDIT_PER_MS;
    return n->cfg.duty_cycle_permille == 0u || (int64_t)n->credit >= limit;
}

/* When the credit is back to zero (assuming no further transmissions). */
static uint32_t budget_ready_at(const kv_node_t *n)
{
    uint32_t p = n->cfg.duty_cycle_permille;
    if (budget_ok(n)) {
        return n->credit_time;
    }
    return n->credit_time + ((uint32_t)(-(int64_t)n->credit) + p - 1u) / p;
}

/* ---- duplicate detection ---- */

/* Returns true if the frame was seen within the window; otherwise remembers it. */
static bool dedup_seen(kv_node_t *n, const kv_frame_header_t *hdr, uint32_t now)
{
    uint8_t key = frag_key(hdr);
    kv_dedup_entry_t *e;
    size_t i;

    for (i = 0; i < KV_DEDUP_SIZE; i++) {
        e = &n->dedup[i];
        if (e->used && e->src == hdr->src && e->seq == hdr->seq && e->frag_key == key &&
            !time_reached(now, e->time_ms + KV_DEDUP_WINDOW_MS)) {
            return true;
        }
    }
    e = &n->dedup[n->dedup_next]; /* ring buffer: overwrite the oldest entry */
    e->used = true;
    e->src = hdr->src;
    e->seq = hdr->seq;
    e->frag_key = key;
    e->time_ms = now;
    n->dedup_next = (uint8_t)((n->dedup_next + 1u) % KV_DEDUP_SIZE);
    return false;
}

/* ---- transmission ---- */

static kv_status_t transmit(kv_node_t *n, const uint8_t *buf, size_t len, kv_tx_kind_t kind)
{
    kv_status_t st = n->pal.radio_send(n->pal.ctx, buf, len);
    if (st == KV_OK) {
        n->tx_busy = true;
        n->tx_kind = kind;
        n->tx_started = now_ms(n);
        n->stats.tx_frames++;
    }
    return st;
}

static void header_init(const kv_node_t *n, kv_frame_header_t *hdr, uint8_t type, uint16_t dst,
                        uint8_t seq)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->hop_limit = n->cfg.hop_limit;
    hdr->net_id = n->cfg.net_id;
    hdr->dst = dst;
    hdr->src = n->cfg.addr;
    hdr->seq = seq;
}

static kv_tx_entry_t *txq_head(kv_node_t *n)
{
    return n->txq_count > 0u ? &n->txq[n->txq_head] : NULL;
}

/* Remove the head message and report its outcome (QoS 1 only). */
static void finish_head(kv_node_t *n, kv_status_t result)
{
    kv_tx_entry_t *e = txq_head(n);
    uint8_t seq = e->seq;
    bool report = e->qos == KV_QOS1;

    n->txq_head = (uint8_t)((n->txq_head + 1u) % KV_TX_QUEUE);
    n->txq_count--;
    if (report && n->cfg.on_ack != NULL) {
        n->cfg.on_ack(n->cfg.user, seq, result);
    }
}

/* Wait for an ACK: the timeout doubles with every attempt, plus up to half of it as jitter. */
static uint32_t ack_wait_ms(const kv_node_t *n, uint8_t attempts)
{
    uint32_t base = n->cfg.ack_timeout_ms;
    unsigned shift = attempts > 1u ? attempts - 1u : 0u;

    if (shift > 5u) {
        shift = 5u;
    }
    if (base > (UINT32_MAX >> 7)) {
        base = UINT32_MAX >> 7;
    }
    return (base << shift) + random32(n) % (base / 2u + 1u);
}

static void send_fragment(kv_node_t *n, kv_tx_entry_t *e)
{
    kv_frame_t f;
    size_t len;
    kv_status_t st;

    header_init(n, &f.hdr, KV_FRAME_DATA, e->dst, e->seq);
    f.hdr.ack_req = e->qos == KV_QOS1;
    if (e->frag_count == 1u) {
        f.payload = e->data;
        f.payload_len = e->len;
    } else {
        size_t chunk = chunk_size(n);
        size_t off = (size_t)e->next_frag * chunk;
        f.hdr.frag = true;
        f.hdr.frag_index = e->next_frag;
        f.hdr.frag_last = (uint8_t)(e->frag_count - 1u);
        f.payload = &e->data[off];
        f.payload_len = e->len - off < chunk ? e->len - off : chunk;
    }
    st = kv_frame_encode(&f, n->tx_buf, n->pal.mtu, &len);
    if (st == KV_OK) {
        st = transmit(n, n->tx_buf, len, KV_TX_OWN);
    }
    if (st != KV_OK) {
        node_log(n, KV_LOG_ERROR, "cannot transmit, message dropped");
        finish_head(n, st);
        return;
    }
    if (e->next_frag == 0u && e->attempts > 0u) {
        n->stats.retransmissions++;
    }
    n->tx_own_seq = e->seq;
    e->next_frag++;
}

static void send_ack(kv_node_t *n, kv_ack_entry_t a)
{
    kv_frame_t f;
    size_t len;

    header_init(n, &f.hdr, KV_FRAME_ACK, a.dst, n->next_seq++);
    f.payload = &a.acked_seq;
    f.payload_len = 1;
    if (kv_frame_encode(&f, n->tx_buf, n->pal.mtu, &len) == KV_OK &&
        transmit(n, n->tx_buf, len, KV_TX_ACK) == KV_OK) {
        n->stats.acks_sent++;
    }
}

/*
 * In a flooded mesh the destination hears relayed copies of a message shortly after the first
 * one; only the sender's retransmission (after its ACK timeout) deserves another ACK.
 */
static uint32_t reack_gap_ms(const kv_node_t *n)
{
    uint32_t gap = 2u * n->cfg.relay_window_ms;
    return gap < 1000u ? 1000u : gap;
}

static kv_ack_record_t *ack_record(kv_node_t *n, uint16_t src, uint8_t seq)
{
    size_t i;
    for (i = 0; i < KV_ACK_RECORDS; i++) {
        kv_ack_record_t *r = &n->acked[i];
        if (r->used && r->src == src && r->seq == seq) {
            return r;
        }
    }
    return NULL;
}

static void queue_ack(kv_node_t *n, uint16_t dst, uint8_t seq)
{
    uint32_t now = now_ms(n);
    kv_ack_record_t *r = ack_record(n, dst, seq);

    if (r != NULL && !time_reached(now, r->time_ms + reack_gap_ms(n))) {
        return; /* a relayed copy of a message we just acknowledged */
    }
    if (r == NULL) {
        r = &n->acked[n->acked_next];
        n->acked_next = (uint8_t)((n->acked_next + 1u) % KV_ACK_RECORDS);
        r->used = true;
        r->src = dst;
        r->seq = seq;
    }
    r->time_ms = now;
    if (n->ackq_count >= KV_ACK_QUEUE) {
        node_log(n, KV_LOG_WARN, "ACK queue full, ACK dropped");
        return;
    }
    n->ackq[n->ackq_count].dst = dst;
    n->ackq[n->ackq_count].acked_seq = seq;
    n->ackq_count++;
}

static kv_relay_entry_t *earliest_due_relay(kv_node_t *n, uint32_t now)
{
    kv_relay_entry_t *best = NULL;
    size_t i;
    for (i = 0; i < KV_RELAY_QUEUE; i++) {
        kv_relay_entry_t *r = &n->relays[i];
        if (r->used && time_reached(now, r->due) &&
            (best == NULL || (int32_t)(r->due - best->due) < 0)) {
            best = r;
        }
    }
    return best;
}

/* Start the next transmission, if any: ACKs first, then due relays, then own messages. */
static void pump(kv_node_t *n)
{
    uint32_t now;
    kv_relay_entry_t *r;
    kv_tx_entry_t *e;

    if (n->tx_busy) {
        return;
    }
    n->depth++;
    now = now_ms(n);
    budget_refill(n, now);

    /* ACKs keep the sender from retrying; they may overdraw the budget (repaid later) */
    while (n->ackq_count > 0u && !n->tx_busy) {
        kv_ack_entry_t a = n->ackq[0];
        n->ackq_count--;
        memmove(&n->ackq[0], &n->ackq[1], n->ackq_count * sizeof(a));
        if (ack_budget_ok(n)) {
            send_ack(n, a);
        } else {
            node_log(n, KV_LOG_WARN, "ACK debt limit reached, ACK dropped");
        }
    }

    while (!n->tx_busy && (r = earliest_due_relay(n, now)) != NULL) {
        r->used = false;
        bool allowed = r->is_ack ? ack_budget_ok(n) : budget_ok(n);
        if (!allowed || transmit(n, r->frame, r->len, KV_TX_RELAY) != KV_OK) {
            n->stats.relay_dropped++; /* a late relay is useless; do not queue it */
        } else {
            n->stats.relayed++;
        }
    }

    while (!n->tx_busy && (e = txq_head(n)) != NULL) {
        if (e->awaiting_ack) {
            if (!time_reached(now, e->ack_deadline)) {
                break;
            }
            if (e->attempts > n->cfg.max_retries) {
                finish_head(n, KV_ERR_TIMEOUT);
                continue;
            }
            e->awaiting_ack = false; /* retransmit the whole message */
            e->next_frag = 0;
        }
        if (!budget_ok(n)) {
            if (!e->budget_wait_counted) {
                e->budget_wait_counted = true;
                n->stats.budget_waits++;
            }
            break;
        }
        e->budget_wait_counted = false;
        send_fragment(n, e);
    }
    n->depth--;
}

/* ---- reception ---- */

static bool subscribed(const kv_node_t *n, uint16_t topic)
{
    size_t i;
    for (i = 0; i < n->subs_count; i++) {
        if (n->subs[i] == topic) {
            return true;
        }
    }
    return false;
}

static void deliver(kv_node_t *n, const kv_frame_header_t *hdr, uint8_t hops, const uint8_t *data,
                    size_t len, const kv_rx_meta_t *meta)
{
    kv_message_t msg;

    if (len < 2u) {
        return; /* no topic id */
    }
    if (hdr->ack_req && hdr->dst == n->cfg.addr) {
        queue_ack(n, hdr->src, hdr->seq); /* before the callback, which may publish */
    }
    msg.topic = (uint16_t)(data[0] | (data[1] << 8));
    if (hdr->dst != n->cfg.addr && !subscribed(n, msg.topic)) {
        return;
    }
    msg.src = hdr->src;
    msg.dst = hdr->dst;
    msg.seq = hdr->seq;
    msg.hops = hops;
    msg.data = &data[2];
    msg.len = len - 2u;
    msg.meta = meta;
    n->stats.delivered++;
    if (n->cfg.on_message != NULL) {
        n->cfg.on_message(n->cfg.user, &msg);
    }
}

static kv_reassembly_t *reasm_find(kv_node_t *n, uint16_t src, uint8_t seq, uint32_t now)
{
    size_t i;
    for (i = 0; i < KV_REASSEMBLY_SLOTS; i++) {
        kv_reassembly_t *s = &n->reasm[i];
        if (s->used && s->src == src && s->seq == seq &&
            !time_reached(now, s->updated + KV_REASSEMBLY_TIMEOUT_MS)) {
            return s;
        }
    }
    return NULL;
}

static void reassemble(kv_node_t *n, const kv_frame_t *f, const kv_rx_meta_t *meta, uint32_t now)
{
    const kv_frame_header_t *hdr = &f->hdr;
    kv_reassembly_t *s = reasm_find(n, hdr->src, hdr->seq, now);
    size_t chunk = chunk_size(n);
    size_t off = (size_t)hdr->frag_index * chunk;
    uint32_t all;
    size_t i;

    if (s == NULL) {
        for (i = 0; i < KV_REASSEMBLY_SLOTS && s == NULL; i++) {
            kv_reassembly_t *c = &n->reasm[i];
            if (!c->used) {
                s = c;
            } else if (time_reached(now, c->updated + KV_REASSEMBLY_TIMEOUT_MS)) {
                n->stats.reassembly_dropped++; /* incomplete and expired */
                s = c;
            }
        }
        if (s == NULL) {
            n->stats.reassembly_dropped++; /* busy: never evict a live message */
            return;
        }
        memset(s, 0, sizeof(*s) - sizeof(s->data));
        s->used = true;
        s->src = hdr->src;
        s->seq = hdr->seq;
        s->dst = hdr->dst;
        s->ack_req = hdr->ack_req;
        s->last = hdr->frag_last;
        s->updated = now;
    }

    if (hdr->frag_last != s->last ||
        (hdr->frag_index < s->last && f->payload_len != chunk) ||
        off + f->payload_len > KV_MAX_MSG_SIZE) {
        s->used = false; /* inconsistent: senders must share the MTU */
        n->stats.reassembly_dropped++;
        return;
    }
    memcpy(&s->data[off], f->payload, f->payload_len);
    s->updated = now;
    s->have = (uint16_t)(s->have | (1u << hdr->frag_index));
    if (hdr->hop_count > s->hops) {
        s->hops = hdr->hop_count;
    }
    if (hdr->frag_index == s->last) {
        s->len = (uint16_t)(off + f->payload_len);
    }

    all = ((uint32_t)1 << (s->last + 1u)) - 1u;
    if (s->have == all) {
        kv_frame_header_t whole = *hdr;
        whole.ack_req = s->ack_req;
        whole.dst = s->dst;
        deliver(n, &whole, s->hops, s->data, s->len, meta);
        s->used = false;
    }
}

static void handle_ack(kv_node_t *n, uint16_t src, uint8_t acked_seq)
{
    kv_tx_entry_t *e = txq_head(n);
    if (e != NULL && e->qos == KV_QOS1 && e->dst == src && e->seq == acked_seq &&
        (e->awaiting_ack || e->attempts > 0u)) {
        finish_head(n, KV_OK);
    }
}

/* Delay before relaying: farther nodes (weaker signal) go first, plus jitter. */
static uint32_t relay_delay(const kv_node_t *n, const kv_rx_meta_t *meta)
{
    uint64_t window = n->cfg.relay_window_ms;
    int32_t weight = 500; /* per mille of the window; unknown SNR: the middle */

    if (meta != NULL) {
        weight = ((int32_t)meta->snr + 20) * 1000 / 30; /* -20 dB -> 0, +10 dB -> 1000 */
        weight = weight < 0 ? 0 : (weight > 1000 ? 1000 : weight);
    }
    return (uint32_t)(window * (uint64_t)weight / 1000u) +
           random32(n) % (uint32_t)(window / 4u + 1u);
}

static void relay_enqueue(kv_node_t *n, const kv_frame_t *f, const kv_rx_meta_t *meta,
                          uint32_t now)
{
    kv_relay_entry_t *slot = NULL;
    kv_frame_t r = *f;
    size_t len;
    size_t i;

    for (i = 0; i < KV_RELAY_QUEUE && slot == NULL; i++) {
        if (!n->relays[i].used) {
            slot = &n->relays[i];
        }
    }
    if (slot == NULL) {
        n->stats.relay_dropped++;
        return;
    }
    r.hdr.hop_limit--;
    r.hdr.hop_count++;
    if (kv_frame_encode(&r, slot->frame, sizeof(slot->frame), &len) != KV_OK) {
        return;
    }
    slot->used = true;
    slot->is_ack = r.hdr.type == KV_FRAME_ACK;
    slot->src = r.hdr.src;
    slot->seq = r.hdr.seq;
    slot->frag_key = frag_key(&r.hdr);
    slot->hop_count = r.hdr.hop_count;
    slot->len = (uint16_t)len;
    slot->due = now + relay_delay(n, meta);
}

/* Someone else relayed the frame we are about to relay: ours would only add a collision. */
static void relay_suppress(kv_node_t *n, const kv_frame_header_t *hdr)
{
    size_t i;
    for (i = 0; i < KV_RELAY_QUEUE; i++) {
        kv_relay_entry_t *r = &n->relays[i];
        if (r->used && r->src == hdr->src && r->seq == hdr->seq &&
            r->frag_key == frag_key(hdr) && hdr->hop_count >= r->hop_count) {
            r->used = false;
            n->stats.relay_cancelled++;
        }
    }
}

static void handle_frame(kv_node_t *n, const kv_frame_t *f, const kv_rx_meta_t *meta,
                         uint32_t now)
{
    const kv_frame_header_t *hdr = &f->hdr;
    const uint16_t me = n->cfg.addr;

    if (dedup_seen(n, hdr, now)) {
        n->stats.rx_duplicates++;
        relay_suppress(n, hdr);
        /* a retransmission: our ACK was lost, send it again (once per complete message) */
        if (hdr->dst == me && hdr->type == KV_FRAME_DATA && hdr->ack_req &&
            (!hdr->frag ||
             (hdr->frag_index == hdr->frag_last && reasm_find(n, hdr->src, hdr->seq, now) == NULL))) {
            queue_ack(n, hdr->src, hdr->seq);
        }
        return;
    }

    /* relays look at the header only, so they also carry types they do not understand */
    if (n->cfg.relay && hdr->dst != me && hdr->hop_limit > 0u) {
        relay_enqueue(n, f, meta, now);
    }
    if (hdr->dst != me && hdr->dst != KV_ADDR_BROADCAST) {
        return;
    }

    switch (hdr->type) {
    case KV_FRAME_DATA:
        if (hdr->frag) {
            reassemble(n, f, meta, now);
        } else {
            deliver(n, hdr, hdr->hop_count, f->payload, f->payload_len, meta);
        }
        break;
    case KV_FRAME_ACK:
        if (hdr->dst == me && f->payload_len >= 1u) {
            handle_ack(n, hdr->src, f->payload[0]);
        }
        break;
    default:
        break; /* unknown types are dropped at the destination */
    }
}

/* ---- public API ---- */

void kv_node_config_default(kv_node_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->hop_limit = KV_HOP_LIMIT_DEFAULT;
    cfg->ack_timeout_ms = 5000u;
    cfg->max_retries = 3u;
    cfg->duty_cycle_permille = 10u;
    cfg->ack_debt_ms = 500u;
    cfg->relay = true;
    cfg->relay_window_ms = 1000u;
}

kv_status_t kv_node_init(kv_node_t *node, const kv_pal_t *pal, const kv_node_config_t *cfg)
{
    if (node == NULL || pal == NULL || cfg == NULL || pal->now_ms == NULL ||
        pal->radio_send == NULL || pal->random == NULL) {
        return KV_ERR_INVALID_ARG;
    }
    if (pal->mtu < KV_FRAME_HEADER_LEN + KV_FRAME_FRAG_HEADER_LEN + 2u || pal->mtu > KV_MTU_MAX ||
        cfg->addr == KV_ADDR_BROADCAST || cfg->hop_limit > KV_HOP_LIMIT_MAX) {
        return KV_ERR_INVALID_ARG;
    }

    memset(node, 0, sizeof(*node));
    node->pal = *pal;
    node->cfg = *cfg;
    /* random start: after a reboot, peers' dedup caches must not swallow our new messages */
    node->next_seq = (uint8_t)(pal->random(pal->ctx) & 0xFFu);
    node->credit_time = pal->now_ms(pal->ctx);
    return KV_OK;
}

kv_status_t kv_node_subscribe(kv_node_t *node, uint16_t topic)
{
    if (node == NULL) {
        return KV_ERR_INVALID_ARG;
    }
    if (subscribed(node, topic)) {
        return KV_OK;
    }
    if (node->subs_count >= KV_MAX_SUBSCRIPTIONS) {
        return KV_ERR_NO_SPACE;
    }
    node->subs[node->subs_count++] = topic;
    return KV_OK;
}

kv_status_t kv_node_unsubscribe(kv_node_t *node, uint16_t topic)
{
    size_t i;
    if (node == NULL) {
        return KV_ERR_INVALID_ARG;
    }
    for (i = 0; i < node->subs_count; i++) {
        if (node->subs[i] == topic) {
            node->subs[i] = node->subs[--node->subs_count];
            return KV_OK;
        }
    }
    return KV_OK;
}

kv_status_t kv_node_publish(kv_node_t *node, uint16_t dst, uint16_t topic, const void *data,
                            size_t len, uint8_t qos, uint8_t *out_seq)
{
    kv_tx_entry_t *e;
    size_t total = len + 2u;
    size_t frags;

    if (node == NULL || (data == NULL && len > 0u) || qos > KV_QOS1 || dst == node->cfg.addr) {
        return KV_ERR_INVALID_ARG;
    }
    if (qos == KV_QOS1 && dst == KV_ADDR_BROADCAST) {
        return KV_ERR_MALFORMED; /* would trigger an ACK storm */
    }
    if (total > KV_MAX_MSG_SIZE) {
        return KV_ERR_NO_SPACE;
    }
    frags = total <= node->pal.mtu - KV_FRAME_HEADER_LEN
                ? 1u
                : (total + chunk_size(node) - 1u) / chunk_size(node);
    if (frags > KV_FRAG_MAX) {
        return KV_ERR_NO_SPACE;
    }
    if (node->txq_count >= KV_TX_QUEUE) {
        return KV_ERR_BUSY;
    }

    e = &node->txq[(node->txq_head + node->txq_count) % KV_TX_QUEUE];
    memset(e, 0, sizeof(*e) - sizeof(e->data));
    e->seq = node->next_seq++;
    e->qos = qos;
    e->frag_count = (uint8_t)frags;
    e->dst = dst;
    e->len = (uint16_t)total;
    e->data[0] = (uint8_t)(topic & 0xFFu);
    e->data[1] = (uint8_t)(topic >> 8);
    if (len > 0u) {
        memcpy(&e->data[2], data, len);
    }
    node->txq_count++;
    if (out_seq != NULL) {
        *out_seq = e->seq;
    }
    if (node->depth == 0u) {
        pump(node);
    }
    return KV_OK;
}

void kv_node_on_rx(kv_node_t *node, const uint8_t *buf, size_t len, const kv_rx_meta_t *meta)
{
    kv_frame_t f;

    if (node == NULL || kv_frame_decode(buf, len, &f) != KV_OK) {
        return;
    }
    node->stats.rx_frames++;
    if (f.hdr.net_id != node->cfg.net_id || f.hdr.src == node->cfg.addr) {
        return; /* other network, or our own frame relayed back */
    }
    node->depth++;
    handle_frame(node, &f, meta, now_ms(node));
    node->depth--;
    pump(node);
}

void kv_node_on_tx_done(kv_node_t *node)
{
    uint32_t now;
    uint32_t airtime;
    kv_tx_entry_t *e;

    if (node == NULL || !node->tx_busy) {
        return;
    }
    now = now_ms(node);
    airtime = now - node->tx_started;
    node->tx_busy = false;
    node->stats.tx_airtime_ms += airtime;
    budget_charge(node, now, airtime); /* the time-off starts when the frame has left */

    e = txq_head(node);
    if (node->tx_kind == KV_TX_OWN && e != NULL && e->seq == node->tx_own_seq &&
        e->next_frag >= e->frag_count) {
        e->attempts++;
        if (e->qos == KV_QOS1) {
            e->awaiting_ack = true;
            e->ack_deadline = now + ack_wait_ms(node, e->attempts);
        } else {
            node->depth++;
            finish_head(node, KV_OK);
            node->depth--;
        }
    }
    node->tx_kind = KV_TX_NONE;
    pump(node);
}

void kv_node_tick(kv_node_t *node)
{
    if (node != NULL) {
        pump(node);
    }
}

bool kv_node_next_deadline(const kv_node_t *node, uint32_t *at_ms)
{
    const kv_tx_entry_t *e;
    uint32_t now;
    uint32_t best = 0;
    bool have = false;
    size_t i;

    /* while transmitting, kv_node_on_tx_done() drives the next step */
    if (node == NULL || at_ms == NULL || node->tx_busy) {
        return false;
    }
    now = now_ms(node);
    for (i = 0; i < KV_RELAY_QUEUE; i++) {
        const kv_relay_entry_t *r = &node->relays[i];
        if (r->used && (!have || (int32_t)(r->due - best) < 0)) {
            best = r->due;
            have = true;
        }
    }
    if (node->ackq_count > 0u) {
        best = now;
        have = true;
    }
    e = node->txq_count > 0u ? &node->txq[node->txq_head] : NULL;
    if (e != NULL) {
        uint32_t t = e->awaiting_ack ? e->ack_deadline : later(now, budget_ready_at(node));
        if (!have || (int32_t)(t - best) < 0) {
            best = t;
            have = true;
        }
    }
    *at_ms = best;
    return have;
}

const kv_node_stats_t *kv_node_stats(const kv_node_t *node)
{
    return node != NULL ? &node->stats : NULL;
}

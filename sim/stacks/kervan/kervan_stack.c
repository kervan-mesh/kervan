/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Adapter: runs an unmodified kv_node on a simulated radio. Maps the simulator host to a kv_pal_t
 * and the node's next deadline to a simulator timer, adds a small traffic generator, and reports
 * relays, budget waits and per-node statistics to the trace.
 */

#include "ksim/stacks/kervan.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kervan/node.h"
#include "kervan/topic.h"
#include "ksim/frame_fields.h"

enum { TIMER_NODE = 1, TIMER_APP = 2 };

#define DEFAULT_TOPIC "kervan/data"

typedef struct {
    ksim_host_t host;
    kv_node_t node;
    uint16_t send_to;
    uint32_t send_every_ms; /* 0 = no traffic */
    uint32_t jitter_ms;     /* each period varies by up to +-jitter_ms, like a drifting clock */
    uint32_t len;
    uint8_t qos;
    uint16_t topic;
    uint16_t subscribe;
    uint32_t sent;
    kv_node_stats_t seen; /* counters already reported */
} kstack_t;

/* ---- PAL on top of the simulator host ---- */

static uint32_t pal_now_ms(void *ctx)
{
    kstack_t *s = (kstack_t *)ctx;
    return (uint32_t)(s->host.now_us(s->host.ctx) / 1000u);
}

static kv_status_t pal_radio_send(void *ctx, const uint8_t *buf, size_t len)
{
    kstack_t *s = (kstack_t *)ctx;
    int r = s->host.radio_send(s->host.ctx, buf, len);
    return r == KSIM_OK ? KV_OK : (r == KSIM_ERR_BUSY ? KV_ERR_BUSY : KV_ERR_INVALID_ARG);
}

static uint32_t pal_random(void *ctx)
{
    kstack_t *s = (kstack_t *)ctx;
    return s->host.random(s->host.ctx);
}

static void pal_log(void *ctx, kv_log_level_t level, const char *msg)
{
    kstack_t *s = (kstack_t *)ctx;
    (void)level;
    s->host.trace(s->host.ctx, "kv_log", msg);
}

/* ---- node callbacks -> simulator trace ---- */

static void on_message(void *user, const kv_message_t *msg)
{
    kstack_t *s = (kstack_t *)user;
    char detail[80];
    snprintf(detail, sizeof(detail), "from=%u seq=%u hops=%u len=%u topic=0x%04x",
             (unsigned)msg->src, (unsigned)msg->seq, (unsigned)msg->hops, (unsigned)msg->len,
             (unsigned)msg->topic);
    s->host.trace(s->host.ctx, "app_rx", detail);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    kstack_t *s = (kstack_t *)user;
    char detail[32];
    snprintf(detail, sizeof(detail), "seq=%u", (unsigned)seq);
    s->host.trace(s->host.ctx, result == KV_OK ? "app_ack" : "app_ack_timeout", detail);
}

/* Turn counter increments into trace events (the node has no callbacks for these). */
static void report_counters(kstack_t *s)
{
    const kv_node_stats_t *now = kv_node_stats(&s->node);
    uint32_t i;

    for (i = s->seen.relayed; i < now->relayed; i++) {
        s->host.trace(s->host.ctx, "kv_relay", "");
    }
    for (i = s->seen.relay_cancelled; i < now->relay_cancelled; i++) {
        s->host.trace(s->host.ctx, "kv_relay_cancel", "");
    }
    for (i = s->seen.relay_dropped; i < now->relay_dropped; i++) {
        s->host.trace(s->host.ctx, "kv_relay_drop", "");
    }
    for (i = s->seen.budget_waits; i < now->budget_waits; i++) {
        s->host.trace(s->host.ctx, "kv_budget_wait", "");
    }
    for (i = s->seen.retransmissions; i < now->retransmissions; i++) {
        s->host.trace(s->host.ctx, "kv_retx", "");
    }
    s->seen = *now;
}

/* Translate kv_node_next_deadline (ms) into the simulator's node timer (µs). */
static void sync(kstack_t *s)
{
    uint32_t at_ms;

    report_counters(s);
    if (kv_node_next_deadline(&s->node, &at_ms)) {
        uint64_t now_us = s->host.now_us(s->host.ctx);
        int32_t delta_ms = (int32_t)(at_ms - (uint32_t)(now_us / 1000u));
        uint64_t at_us = now_us + (delta_ms > 0 ? (uint64_t)delta_ms * 1000u : 0u);
        s->host.timer_set(s->host.ctx, TIMER_NODE, at_us);
    } else {
        s->host.timer_cancel(s->host.ctx, TIMER_NODE);
    }
}

static void schedule_app(kstack_t *s, uint64_t delay_us)
{
    s->host.timer_set(s->host.ctx, TIMER_APP, s->host.now_us(s->host.ctx) + delay_us);
}

static void app_send(kstack_t *s)
{
    uint8_t payload[KV_MAX_MSG_SIZE];
    char detail[64];
    uint8_t seq = 0;
    kv_status_t st;
    uint32_t i;
    uint32_t len = s->len;
    /* the scenario's payload if it sets one, else counter bytes of the configured length */
    int given = s->host.app_payload != NULL
                    ? s->host.app_payload(s->host.ctx, s->sent, payload, KV_MAX_MSG_SIZE - 2u)
                    : -1;

    if (given >= 0) {
        len = (uint32_t)given;
    } else {
        for (i = 0; i < len; i++) {
            payload[i] = (uint8_t)(s->sent + i);
        }
    }
    s->sent++;
    st = kv_node_publish(&s->node, s->send_to, s->topic, payload, len, s->qos, &seq);
    if (st == KV_OK) {
        snprintf(detail, sizeof(detail), "to=%u seq=%u len=%u qos=%u", (unsigned)s->send_to,
                 (unsigned)seq, (unsigned)len, (unsigned)s->qos);
        s->host.trace(s->host.ctx, "app_tx", detail);
    } else {
        snprintf(detail, sizeof(detail), "to=%u status=%d", (unsigned)s->send_to, (int)st);
        s->host.trace(s->host.ctx, "app_tx_busy", detail);
    }
}

/* ---- config: "key=value;key=value" ---- */

static bool apply_option(kstack_t *s, kv_node_config_t *cfg, const char *key, const char *text)
{
    char *end;
    unsigned long v = strtoul(text, &end, 10);
    bool number = text[0] != '\0' && *end == '\0';

    if (strcmp(key, "topic") == 0) {
        s->topic = kv_topic_id(text);
        return true;
    }
    if (strcmp(key, "subscribe") == 0) {
        s->subscribe = kv_topic_id(text);
        return true;
    }
    if (!number) {
        return false;
    }
    if (strcmp(key, "addr") == 0 && v < KV_ADDR_BROADCAST) {
        cfg->addr = (uint16_t)v;
    } else if (strcmp(key, "net") == 0 && v <= 0xFFu) {
        cfg->net_id = (uint8_t)v;
    } else if (strcmp(key, "hop_limit") == 0 && v <= KV_HOP_LIMIT_MAX) {
        cfg->hop_limit = (uint8_t)v;
    } else if (strcmp(key, "ack_timeout_ms") == 0 && v <= 0x7FFFFFFFul) {
        cfg->ack_timeout_ms = (uint32_t)v;
    } else if (strcmp(key, "retries") == 0 && v <= 0xFFu) {
        cfg->max_retries = (uint8_t)v;
    } else if (strcmp(key, "duty_permille") == 0 && v <= 1000u) {
        cfg->duty_cycle_permille = (uint16_t)v;
    } else if (strcmp(key, "duty_burst_ms") == 0 && v <= 0x7FFFFFFFul) {
        cfg->duty_burst_ms = (uint32_t)v;
    } else if (strcmp(key, "relay") == 0 && v <= 1u) {
        cfg->relay = v == 1u;
    } else if (strcmp(key, "relay_window_ms") == 0 && v <= 0x7FFFFFFFul) {
        cfg->relay_window_ms = (uint32_t)v;
    } else if (strcmp(key, "send_to") == 0 && v <= KV_ADDR_BROADCAST) {
        s->send_to = (uint16_t)v;
    } else if (strcmp(key, "send_every_ms") == 0 && v <= 0x7FFFFFFFul) {
        s->send_every_ms = (uint32_t)v;
    } else if (strcmp(key, "jitter_ms") == 0 && v <= 0x7FFFFFFFul) {
        s->jitter_ms = (uint32_t)v;
    } else if (strcmp(key, "len") == 0 && v <= KV_MAX_MSG_SIZE - 2u) {
        s->len = (uint32_t)v;
    } else if (strcmp(key, "qos") == 0 && v <= KV_QOS1) {
        s->qos = (uint8_t)v;
    } else {
        return false;
    }
    return true;
}

static bool parse_config(kstack_t *s, kv_node_config_t *cfg, const char *config)
{
    char buf[512];
    char *item;
    char *next;
    size_t n;

    if (config == NULL || config[0] == '\0') {
        return true;
    }
    n = strlen(config);
    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, config, n + 1u);
    for (item = buf; item != NULL; item = next) {
        char *eq;

        next = strchr(item, ';');
        if (next != NULL) {
            *next++ = '\0';
        }
        eq = strchr(item, '=');
        if (eq == NULL) {
            return false;
        }
        *eq = '\0';
        if (!apply_option(s, cfg, item, eq + 1)) {
            char detail[96];
            snprintf(detail, sizeof(detail), "invalid config option '%.40s'", item);
            s->host.trace(s->host.ctx, "kv_error", detail);
            return false;
        }
    }
    return true;
}

/* ---- ksim_stack_t ---- */

static void *ks_create(const ksim_host_t *host, const char *config)
{
    kstack_t *s = (kstack_t *)calloc(1, sizeof(kstack_t));
    kv_node_config_t cfg;
    kv_pal_t pal;
    int64_t phy = 0;

    if (s == NULL) {
        return NULL;
    }
    s->host = *host;
    /* Kervan frames go straight onto a LoRa radio; other radios have their own framing */
    if (host->radio_get(host->ctx, KSIM_RADIO_PHY, &phy) != KSIM_OK || phy != KSIM_PHY_LORA) {
        host->trace(host->ctx, "kv_error", "the kervan stack needs a lora radio");
        free(s);
        return NULL;
    }
    s->send_to = KV_ADDR_BROADCAST;
    s->len = 10;
    s->topic = kv_topic_id(DEFAULT_TOPIC);
    s->subscribe = s->topic;

    kv_node_config_default(&cfg);
    cfg.addr = (uint16_t)(host->node_id & 0xFFFFu);
    cfg.net_id = 1;
    cfg.on_message = on_message;
    cfg.on_ack = on_ack;
    cfg.user = s;

    memset(&pal, 0, sizeof(pal));
    pal.ctx = s;
    pal.mtu = host->mtu < KV_MTU_MAX ? host->mtu : KV_MTU_MAX;
    pal.now_ms = pal_now_ms;
    pal.radio_send = pal_radio_send;
    pal.random = pal_random;
    pal.log = pal_log;

    if (!parse_config(s, &cfg, config) || (s->qos == KV_QOS1 && s->send_to == KV_ADDR_BROADCAST) ||
        s->send_to == cfg.addr || kv_node_init(&s->node, &pal, &cfg) != KV_OK) {
        free(s);
        return NULL;
    }
    kv_node_subscribe(&s->node, s->subscribe);
    if (s->send_every_ms > 0u) {
        /* random first offset so nodes do not all start in the same instant */
        schedule_app(s, (uint64_t)(s->host.random(s->host.ctx) % s->send_every_ms) * 1000u);
    }
    return s;
}

static void ks_destroy(void *inst)
{
    kstack_t *s = (kstack_t *)inst;
    const kv_node_stats_t *st = kv_node_stats(&s->node);
    char detail[320];

    snprintf(detail, sizeof(detail),
             "tx=%lu airtime_ms=%lu delivered=%lu acks=%lu retx=%lu relayed=%lu "
             "relay_cancelled=%lu relay_dropped=%lu dup=%lu budget_waits=%lu reasm_dropped=%lu",
             (unsigned long)st->tx_frames, (unsigned long)st->tx_airtime_ms,
             (unsigned long)st->delivered, (unsigned long)st->acks_sent,
             (unsigned long)st->retransmissions, (unsigned long)st->relayed,
             (unsigned long)st->relay_cancelled, (unsigned long)st->relay_dropped,
             (unsigned long)st->rx_duplicates, (unsigned long)st->budget_waits,
             (unsigned long)st->reassembly_dropped);
    s->host.trace(s->host.ctx, "kv_stats", detail);
    free(s);
}

static void ks_on_rx(void *inst, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    kstack_t *s = (kstack_t *)inst;
    kv_rx_meta_t m;
    double snr = meta->snr_db < -128.0 ? -128.0 : (meta->snr_db > 127.0 ? 127.0 : meta->snr_db);
    m.rssi = (int16_t)meta->rssi_dbm;
    m.snr = (int8_t)snr;
    kv_node_on_rx(&s->node, buf, len, &m);
    sync(s);
}

static void ks_on_tx_done(void *inst)
{
    kstack_t *s = (kstack_t *)inst;
    kv_node_on_tx_done(&s->node);
    sync(s);
}

static void ks_on_timer(void *inst, uint32_t timer_id)
{
    kstack_t *s = (kstack_t *)inst;
    if (timer_id == TIMER_NODE) {
        kv_node_tick(&s->node);
    } else if (timer_id == TIMER_APP) {
        int64_t period_ms = (int64_t)s->send_every_ms;
        app_send(s);
        if (s->jitter_ms > 0u) {
            uint32_t span = 2u * s->jitter_ms + 1u;
            period_ms += (int64_t)(s->host.random(s->host.ctx) % span) - (int64_t)s->jitter_ms;
        }
        schedule_app(s, (uint64_t)(period_ms > 1 ? period_ms : 1) * 1000u);
    }
    sync(s);
}

/* ---- frame decoder for packet inspectors ---- */

/* Value of `key` in a "k=v;k=v" config string into buf; false if absent. */
static bool config_get(const char *config, const char *key, char *buf, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = config;

    while (p != NULL && *p != '\0') {
        const char *end = strchr(p, ';');
        size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
        if (n > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = n - klen - 1u;
            if (vlen >= cap) {
                vlen = cap - 1u;
            }
            memcpy(buf, p + klen + 1u, vlen);
            buf[vlen] = '\0';
            return true;
        }
        p = end != NULL ? end + 1 : NULL;
    }
    return false;
}

/* "0x1234 (name)" if the sender's config names a topic with this id. */
static void topic_text(const char *config, uint16_t id, char *out, size_t cap)
{
    static const char *const keys[] = {"topic", "subscribe"};
    char name[64];
    size_t k;

    snprintf(out, cap, "0x%04x", (unsigned)id);
    for (k = 0; k < 2u; k++) {
        if (!config_get(config != NULL ? config : "", keys[k], name, sizeof(name))) {
            snprintf(name, sizeof(name), "%s", DEFAULT_TOPIC);
        }
        if (kv_topic_id(name) == id) {
            snprintf(out, cap, "0x%04x (%.48s)", (unsigned)id, name);
            return;
        }
    }
}

static size_t ks_describe(const char *config, const uint8_t *buf, size_t len, char *out,
                          size_t cap)
{
    static const char *const types[16] = {"DATA", "ACK", "CTRL", "type 3", "type 4", "type 5",
                                          "type 6", "type 7", "type 8", "type 9", "type 10",
                                          "type 11", "type 12", "type 13", "type 14", "EXT"};
    kv_frame_t f;
    ksim_fields_t w;
    char v[80];
    int hl;
    int rest;

    if (kv_frame_decode(buf, len, &f) != KV_OK) {
        return 0;
    }
    hl = (int)kv_frame_header_len(&f.hdr);
    rest = (int)f.payload_len;
    ksim_fields_begin(&w, out, cap);
    ksim_field(&w, "version", "0", 0, 1);
    snprintf(v, sizeof(v), "%s (%u)", types[f.hdr.type & 0xFu], (unsigned)f.hdr.type);
    ksim_field(&w, "type", v, 0, 1);
    ksim_field(&w, "ack_req", f.hdr.ack_req ? "yes" : "no", 0, 1);
    ksim_field(&w, "fragmented", f.hdr.frag ? "yes" : "no", 0, 1);
    snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.hop_limit);
    ksim_field(&w, "hop_limit", v, 1, 1);
    snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.hop_count);
    ksim_field(&w, "hop_count", v, 1, 1);
    ksim_field(&w, "secured", f.hdr.sec ? "yes" : "no", 1, 1);
    snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.net_id);
    ksim_field(&w, "net_id", v, 2, 1);
    if (f.hdr.dst == KV_ADDR_BROADCAST) {
        snprintf(v, sizeof(v), "%u (broadcast)", (unsigned)f.hdr.dst);
    } else {
        snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.dst);
    }
    ksim_field(&w, "dst", v, 3, 2);
    snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.src);
    ksim_field(&w, "src", v, 5, 2);
    snprintf(v, sizeof(v), "%u", (unsigned)f.hdr.seq);
    ksim_field(&w, "seq", v, 7, 1);
    if (f.hdr.frag) {
        snprintf(v, sizeof(v), "%u of %u", (unsigned)f.hdr.frag_index + 1u,
                 (unsigned)f.hdr.frag_last + 1u);
        ksim_field(&w, "fragment", v, 8, 1);
    }

    if (f.hdr.type == KV_FRAME_DATA && (!f.hdr.frag || f.hdr.frag_index == 0u) && rest >= 2) {
        topic_text(config, (uint16_t)(f.payload[0] | (f.payload[1] << 8)), v, sizeof(v));
        ksim_field(&w, "topic", v, hl, 2);
        snprintf(v, sizeof(v), "%d bytes", rest - 2);
        ksim_field(&w, "data", v, hl + 2, rest - 2);
    } else if (f.hdr.type == KV_FRAME_ACK && rest >= 1) {
        snprintf(v, sizeof(v), "%u", (unsigned)f.payload[0]);
        ksim_field(&w, "acked_seq", v, hl, 1);
    } else if (rest > 0) {
        snprintf(v, sizeof(v), "%d bytes", rest);
        ksim_field(&w, f.hdr.type == KV_FRAME_DATA ? "data (continued)" : "payload", v, hl, rest);
    }
    return ksim_fields_end(&w);
}

/* Options accepted by ks_create, for tools such as the web UI (docs/simulator.md). Keep the
 * defaults in sync with kv_node_config_default and ks_create; a test checks them. */
static const char config_schema[] =
    "{\"phy\":[\"lora\"],\"options\":["
    "{\"key\":\"addr\",\"type\":\"int\",\"min\":0,\"max\":65534,\"group\":\"node\","
    "\"help\":\"Kervan address (default: the node id)\"},"
    "{\"key\":\"net\",\"type\":\"int\",\"min\":0,\"max\":255,\"default\":1,\"group\":\"node\","
    "\"help\":\"Network id; nodes ignore other networks\"},"
    "{\"key\":\"hop_limit\",\"type\":\"int\",\"min\":0,\"max\":7,\"default\":3,\"group\":\"node\","
    "\"help\":\"Relays a message of this node may pass\"},"
    "{\"key\":\"relay\",\"type\":\"bool\",\"default\":true,\"group\":\"mesh\","
    "\"help\":\"Relay frames for other nodes (managed flooding)\"},"
    "{\"key\":\"relay_window_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":1000,"
    "\"group\":\"mesh\",\"help\":\"Relays wait 0..window (weaker signal first) plus jitter\"},"
    "{\"key\":\"duty_permille\",\"type\":\"int\",\"min\":0,\"max\":1000,\"default\":10,"
    "\"group\":\"budget\",\"help\":\"Airtime budget in 1/1000: 10 = 1 % (EU868); 0 = unlimited\"},"
    "{\"key\":\"duty_burst_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":0,"
    "\"group\":\"budget\",\"help\":\"Airtime credit that may build up while idle\"},"
    "{\"key\":\"ack_timeout_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":5000,"
    "\"group\":\"qos\",\"help\":\"First wait for an ACK; doubles with every retry\"},"
    "{\"key\":\"retries\",\"type\":\"int\",\"min\":0,\"max\":255,\"default\":3,\"group\":\"qos\","
    "\"help\":\"Retransmissions after the first attempt (QoS 1)\"},"
    "{\"key\":\"send_to\",\"type\":\"int\",\"min\":0,\"max\":65535,\"default\":65535,"
    "\"group\":\"traffic\",\"help\":\"Destination address; 65535 = broadcast\"},"
    "{\"key\":\"send_every_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":0,"
    "\"group\":\"traffic\",\"help\":\"Publish period; 0 = no traffic\"},"
    "{\"key\":\"jitter_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":0,"
    "\"group\":\"traffic\",\"help\":\"Each period varies by up to +-jitter\"},"
    "{\"key\":\"len\",\"type\":\"int\",\"min\":0,\"max\":254,\"default\":10,\"group\":\"traffic\","
    "\"help\":\"Payload bytes; larger than one frame is fragmented\"},"
    "{\"key\":\"qos\",\"type\":\"enum\",\"values\":[0,1],\"default\":0,\"group\":\"traffic\","
    "\"help\":\"0 = at most once, 1 = acknowledged (unicast only)\"},"
    "{\"key\":\"topic\",\"type\":\"string\",\"default\":\"kervan/data\",\"group\":\"traffic\","
    "\"help\":\"Topic name of published messages\"},"
    "{\"key\":\"subscribe\",\"type\":\"string\",\"default\":\"kervan/data\",\"group\":\"traffic\","
    "\"help\":\"Broadcast topic this node receives\"}"
    "]}";

const ksim_stack_t *ksim_stack_kervan(void)
{
    static const ksim_stack_t stack = {KSIM_ABI_VERSION, "kervan",      "2",        ks_create,
                                       ks_destroy,       ks_on_rx,      ks_on_tx_done, ks_on_timer,
                                       config_schema, ks_describe};
    return &stack;
}

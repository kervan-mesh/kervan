/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Adapter: runs an unmodified kv_node on a simulated radio. Maps the simulator host to a kv_pal_t
 * and the node's next deadline to a simulator timer, and adds a small traffic generator.
 */

#include "ksim/stacks/kervan.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kervan/node.h"

enum { TIMER_NODE = 1, TIMER_APP = 2 };

typedef struct {
    ksim_host_t host;
    kv_node_t node;
    uint16_t send_to;
    uint32_t send_every_ms; /* 0 = no traffic */
    uint32_t jitter_ms;     /* each period varies by up to +-jitter_ms, like a drifting clock */
    uint32_t len;
    bool ack;
    uint32_t sent;
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

static void on_data(void *user, const kv_frame_t *frame, const kv_rx_meta_t *meta)
{
    kstack_t *s = (kstack_t *)user;
    char detail[64];
    (void)meta;
    snprintf(detail, sizeof(detail), "from=%u seq=%u len=%u", (unsigned)frame->hdr.src,
             (unsigned)frame->hdr.seq, (unsigned)frame->payload_len);
    s->host.trace(s->host.ctx, "app_rx", detail);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    kstack_t *s = (kstack_t *)user;
    char detail[32];
    snprintf(detail, sizeof(detail), "seq=%u", (unsigned)seq);
    s->host.trace(s->host.ctx, result == KV_OK ? "app_ack" : "app_ack_timeout", detail);
}

/* Translate kv_node_next_deadline (ms) into the simulator's node timer (µs). */
static void sync_deadline(kstack_t *s)
{
    uint32_t at_ms;

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
    uint8_t payload[KV_MTU_MAX];
    char detail[48];
    uint8_t seq = 0;
    kv_status_t st;
    uint32_t i;

    for (i = 0; i < s->len; i++) {
        payload[i] = (uint8_t)(s->sent + i);
    }
    s->sent++;
    st = kv_node_send(&s->node, s->send_to, payload, s->len, s->ack, &seq);
    if (st == KV_OK) {
        snprintf(detail, sizeof(detail), "to=%u seq=%u", (unsigned)s->send_to, (unsigned)seq);
        s->host.trace(s->host.ctx, "app_tx", detail);
    } else {
        snprintf(detail, sizeof(detail), "to=%u status=%d", (unsigned)s->send_to, (int)st);
        s->host.trace(s->host.ctx, "app_tx_busy", detail);
    }
}

/* ---- config: "key=value;key=value" ---- */

static bool apply_option(kstack_t *s, kv_node_config_t *cfg, const char *key, unsigned long v)
{
    if (strcmp(key, "addr") == 0 && v < KV_ADDR_BROADCAST) {
        cfg->addr = (uint16_t)v;
    } else if (strcmp(key, "net") == 0 && v <= 0xFFu) {
        cfg->net_id = (uint8_t)v;
    } else if (strcmp(key, "hop_limit") == 0 && v <= KV_HOP_LIMIT_MAX) {
        cfg->hop_limit = (uint8_t)v;
    } else if (strcmp(key, "ack_timeout_ms") == 0 && v <= 0xFFFFFFFFul) {
        cfg->ack_timeout_ms = (uint32_t)v;
    } else if (strcmp(key, "retries") == 0 && v <= 0xFFu) {
        cfg->max_retries = (uint8_t)v;
    } else if (strcmp(key, "send_to") == 0 && v <= KV_ADDR_BROADCAST) {
        s->send_to = (uint16_t)v;
    } else if (strcmp(key, "send_every_ms") == 0 && v <= 0xFFFFFFFFul) {
        s->send_every_ms = (uint32_t)v;
    } else if (strcmp(key, "jitter_ms") == 0 && v <= 0x7FFFFFFFul) {
        s->jitter_ms = (uint32_t)v;
    } else if (strcmp(key, "len") == 0 && v <= KV_MTU_MAX - KV_FRAME_HEADER_LEN) {
        s->len = (uint32_t)v;
    } else if (strcmp(key, "ack") == 0 && v <= 1u) {
        s->ack = v == 1u;
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

    if (config == NULL || config[0] == '\0') {
        return true;
    }
    if (strlen(config) >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, config, strlen(config) + 1u);
    for (item = buf; item != NULL; item = next) {
        char *eq;
        char *end;
        unsigned long v;

        next = strchr(item, ';');
        if (next != NULL) {
            *next++ = '\0';
        }
        eq = strchr(item, '=');
        if (eq == NULL) {
            return false;
        }
        *eq = '\0';
        v = strtoul(eq + 1, &end, 10);
        if (eq[1] == '\0' || *end != '\0' || !apply_option(s, cfg, item, v)) {
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

    kv_node_config_default(&cfg);
    cfg.addr = (uint16_t)(host->node_id & 0xFFFFu);
    cfg.net_id = 1;
    cfg.on_data = on_data;
    cfg.on_ack = on_ack;
    cfg.user = s;

    memset(&pal, 0, sizeof(pal));
    pal.ctx = s;
    pal.mtu = host->mtu < KV_MTU_MAX ? host->mtu : KV_MTU_MAX;
    pal.now_ms = pal_now_ms;
    pal.radio_send = pal_radio_send;
    pal.random = pal_random;
    pal.log = pal_log;

    if (!parse_config(s, &cfg, config) || s->len + KV_FRAME_HEADER_LEN > pal.mtu ||
        (s->ack && s->send_to == KV_ADDR_BROADCAST) ||
        kv_node_init(&s->node, &pal, &cfg) != KV_OK) {
        free(s);
        return NULL;
    }
    if (s->send_every_ms > 0u) {
        /* random first offset so nodes do not all start in the same instant */
        schedule_app(s, (uint64_t)(s->host.random(s->host.ctx) % s->send_every_ms) * 1000u);
    }
    return s;
}

static void ks_destroy(void *inst)
{
    free(inst);
}

static void ks_on_rx(void *inst, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    kstack_t *s = (kstack_t *)inst;
    kv_rx_meta_t m;
    m.rssi = (int16_t)meta->rssi_dbm;
    m.snr = (int8_t)meta->snr_db;
    kv_node_on_rx(&s->node, buf, len, &m);
    sync_deadline(s);
}

static void ks_on_tx_done(void *inst)
{
    kstack_t *s = (kstack_t *)inst;
    kv_node_on_tx_done(&s->node);
    sync_deadline(s);
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
    sync_deadline(s);
}

const ksim_stack_t *ksim_stack_kervan(void)
{
    static const ksim_stack_t stack = {KSIM_ABI_VERSION, "kervan",      "1",        ks_create,
                                       ks_destroy,       ks_on_rx,      ks_on_tx_done, ks_on_timer};
    return &stack;
}

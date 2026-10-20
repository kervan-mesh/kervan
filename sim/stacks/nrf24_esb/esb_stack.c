/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Clean-room model of the nRF24L01+ Enhanced ShockBurst link layer (docs/nrf24-esb.md):
 * PID, auto-acknowledge, ARD/ARC retransmission, ACK payload, dynamic payload, pipes 1-5,
 * 130 us settling. Frames: address | PCF (len<<2 | PID, NO_ACK) | payload.
 */

#include "ksim/stacks/nrf24_esb.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTLE_US 130u
#define MAX_PAYLOAD 32u
#define FRAME_MAX (5u + 2u + MAX_PAYLOAD)
#define MAX_PIPES 5u

enum { T_TX_START = 1, T_ARD = 2, T_RX_ON = 3, T_APP = 4, T_ACK_START = 5 };

typedef enum {
    ST_IDLE,           /* PRX (listening) or standby */
    ST_PTX_SETTLE,     /* going to TX */
    ST_PTX_TX,         /* sending a payload */
    ST_PTX_WAIT_ACK,   /* listening for the ACK until ARD expires */
    ST_PRX_ACK_SETTLE, /* going to TX to acknowledge */
    ST_PRX_ACK_TX      /* sending the ACK */
} esb_state_t;

typedef struct {
    bool valid;
    uint8_t pid;
    uint8_t len;
    uint8_t payload[MAX_PAYLOAD];
} pipe_state_t;

typedef struct {
    ksim_host_t host;

    /* config */
    uint64_t addr;
    unsigned aw;
    unsigned pipes;
    bool listen;
    uint64_t send_to;
    bool has_dst;
    uint32_t send_every_ms;
    uint32_t jitter_ms;
    unsigned len;
    bool no_ack;
    uint32_t ard_us;
    unsigned arc;
    unsigned ack_payload_len;

    /* state */
    esb_state_t state;
    uint8_t frame[FRAME_MAX]; /* PTX payload frame, kept for retransmission */
    size_t frame_len;
    uint8_t pid;
    unsigned retries_left;
    uint8_t ack[FRAME_MAX];
    size_t ack_len;
    pipe_state_t pipe[MAX_PIPES];
    uint32_t counter;
} esb_t;

/* ---- helpers ---- */

static uint64_t now_us(esb_t *s)
{
    return s->host.now_us(s->host.ctx);
}

static void event(esb_t *s, const char *name, const char *fmt, ...)
{
    char detail[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    s->host.trace(s->host.ctx, name, detail);
}

static uint64_t addr_mask(unsigned aw)
{
    return ((uint64_t)1 << (8u * aw)) - 1u;
}

/* Pipes 2-5 share the upper bytes of pipe 1 and differ in the least significant byte. */
static uint64_t pipe_addr(const esb_t *s, unsigned k)
{
    uint64_t base = s->addr & addr_mask(s->aw);
    return (base & ~(uint64_t)0xFFu) | ((base + k) & 0xFFu);
}

static size_t build(uint8_t *out, uint64_t addr, unsigned aw, uint8_t pid, bool no_ack,
                    const uint8_t *payload, unsigned len)
{
    unsigned i;
    for (i = 0; i < aw; i++) {
        out[i] = (uint8_t)(addr >> (8u * i));
    }
    out[aw] = (uint8_t)((len << 2) | (pid & 3u));
    out[aw + 1u] = no_ack ? 1u : 0u;
    if (len > 0u) {
        memcpy(&out[aw + 2u], payload, len);
    }
    return aw + 2u + len;
}

static void rx_enable(esb_t *s, bool on)
{
    s->host.radio_set(s->host.ctx, KSIM_RADIO_RX_ENABLED, on ? 1 : 0);
}

static void at(esb_t *s, uint32_t timer, uint64_t delay_us)
{
    s->host.timer_set(s->host.ctx, timer, now_us(s) + delay_us);
}

/* Back to PRX (after settling) or standby. */
static void go_idle(esb_t *s)
{
    s->state = ST_IDLE;
    s->host.timer_cancel(s->host.ctx, T_ARD);
    rx_enable(s, false);
    if (s->listen) {
        at(s, T_RX_ON, SETTLE_US);
    }
}

/* ---- PTX ---- */

static void app_send(esb_t *s)
{
    uint8_t payload[MAX_PAYLOAD];
    unsigned i;

    if (s->state != ST_IDLE) {
        event(s, "esb_tx_busy", "state=%d", (int)s->state);
        return;
    }
    /* first byte: sender, so equal readings from two senders on one pipe are not duplicates */
    for (i = 0; i < s->len; i++) {
        payload[i] = i == 0u ? (uint8_t)s->host.node_id : (uint8_t)(s->counter + i);
    }
    s->counter++;
    s->pid = (uint8_t)((s->pid + 1u) & 3u);
    s->frame_len = build(s->frame, s->send_to, s->aw, s->pid, s->no_ack, payload, s->len);
    s->retries_left = s->arc;
    s->host.timer_cancel(s->host.ctx, T_RX_ON);
    rx_enable(s, false);
    s->state = ST_PTX_SETTLE;
    at(s, T_TX_START, SETTLE_US);
    event(s, "esb_tx", "to=0x%llx pid=%u len=%u", (unsigned long long)s->send_to,
          (unsigned)s->pid, s->len);
}

static void transmit(esb_t *s, const uint8_t *buf, size_t len, esb_state_t next)
{
    if (s->host.radio_send(s->host.ctx, buf, len) == KSIM_OK) {
        s->state = next;
    } else {
        event(s, "esb_error", "radio_send failed");
        go_idle(s);
    }
}

static void ard_expired(esb_t *s)
{
    rx_enable(s, false);
    if (s->retries_left == 0u) {
        event(s, "esb_max_rt", "pid=%u", (unsigned)s->pid);
        go_idle(s);
        return;
    }
    s->retries_left--;
    event(s, "esb_retx", "pid=%u left=%u", (unsigned)s->pid, s->retries_left);
    transmit(s, s->frame, s->frame_len, ST_PTX_TX); /* starts exactly ARD after the last end */
}

/* ---- PRX ---- */

static void prx_receive(esb_t *s, uint64_t addr, uint8_t pid, bool no_ack, const uint8_t *payload,
                        unsigned len)
{
    unsigned k;
    pipe_state_t *p = NULL;
    bool dup;

    for (k = 0; k < s->pipes; k++) {
        if (addr == pipe_addr(s, k)) {
            p = &s->pipe[k];
            break;
        }
    }
    if (p == NULL) {
        return; /* not for us: the chip's address filter drops it */
    }

    if (!no_ack) {
        uint8_t ack_payload[MAX_PAYLOAD];
        unsigned i;
        for (i = 0; i < s->ack_payload_len; i++) {
            ack_payload[i] = (uint8_t)(0xA0u + i);
        }
        s->ack_len = build(s->ack, addr, s->aw, pid, true, ack_payload, s->ack_payload_len);
        rx_enable(s, false);
        s->state = ST_PRX_ACK_SETTLE;
        at(s, T_ACK_START, SETTLE_US);
    }

    dup = p->valid && p->pid == pid && p->len == len && memcmp(p->payload, payload, len) == 0;
    if (dup) {
        event(s, "esb_rx_dup", "pipe=%u pid=%u", k + 1u, (unsigned)pid);
        return;
    }
    p->valid = true;
    p->pid = pid;
    p->len = (uint8_t)len;
    memcpy(p->payload, payload, len);
    event(s, "esb_rx", "pipe=%u pid=%u len=%u", k + 1u, (unsigned)pid, len);
}

/* ---- config: "key=value;key=value" ---- */

static bool apply_option(esb_t *s, const char *key, unsigned long long v)
{
    if (strcmp(key, "addr") == 0) {
        s->addr = (uint64_t)v;
    } else if (strcmp(key, "aw") == 0 && v >= 3u && v <= 5u) {
        s->aw = (unsigned)v;
    } else if (strcmp(key, "pipes") == 0 && v >= 1u && v <= MAX_PIPES) {
        s->pipes = (unsigned)v;
    } else if (strcmp(key, "listen") == 0 && v <= 1u) {
        s->listen = v == 1u;
    } else if (strcmp(key, "send_to") == 0) {
        s->send_to = (uint64_t)v;
        s->has_dst = true;
    } else if (strcmp(key, "send_every_ms") == 0 && v <= 0xFFFFFFFFull) {
        s->send_every_ms = (uint32_t)v;
    } else if (strcmp(key, "jitter_ms") == 0 && v <= 0x7FFFFFFFull) {
        s->jitter_ms = (uint32_t)v;
    } else if (strcmp(key, "len") == 0 && v <= MAX_PAYLOAD) {
        s->len = (unsigned)v;
    } else if (strcmp(key, "no_ack") == 0 && v <= 1u) {
        s->no_ack = v == 1u;
    } else if (strcmp(key, "ard_us") == 0 && v >= 250u && v <= 4000u && v % 250u == 0u) {
        s->ard_us = (uint32_t)v;
    } else if (strcmp(key, "arc") == 0 && v <= 15u) {
        s->arc = (unsigned)v;
    } else if (strcmp(key, "ack_payload_len") == 0 && v <= MAX_PAYLOAD) {
        s->ack_payload_len = (unsigned)v;
    } else {
        return false;
    }
    return true;
}

static bool parse_config(esb_t *s, const char *config)
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
        char *end;
        unsigned long long v;

        next = strchr(item, ';');
        if (next != NULL) {
            *next++ = '\0';
        }
        eq = strchr(item, '=');
        if (eq == NULL) {
            return false;
        }
        *eq = '\0';
        v = strtoull(eq + 1, &end, 0); /* accepts 0x... for addresses */
        if (eq[1] == '\0' || *end != '\0' || !apply_option(s, item, v)) {
            event(s, "esb_error", "invalid config option '%.40s'", item);
            return false;
        }
    }
    return true;
}

/* ---- ksim_stack_t ---- */

static void *es_create(const ksim_host_t *host, const char *config)
{
    esb_t *s = (esb_t *)calloc(1, sizeof(esb_t));
    int64_t phy = 0;

    if (s == NULL) {
        return NULL;
    }
    s->host = *host;
    if (host->radio_get(host->ctx, KSIM_RADIO_PHY, &phy) != KSIM_OK || phy != KSIM_PHY_NRF24) {
        event(s, "esb_error", "the nrf24-esb stack needs an nrf24 radio");
        free(s);
        return NULL;
    }
    s->addr = 0xE7E7E7E700ull | (host->node_id & 0xFFu);
    s->aw = 5;
    s->pipes = 1;
    s->listen = true;
    s->len = 8;
    s->ard_us = 500;
    s->arc = 3;

    if (!parse_config(s, config) || (s->send_every_ms > 0u && !s->has_dst) ||
        s->aw + 2u + s->len > host->mtu || s->aw + 2u + s->ack_payload_len > host->mtu) {
        free(s);
        return NULL;
    }
    s->state = ST_IDLE;
    rx_enable(s, s->listen); /* powered up and settled at time 0 */
    if (s->send_every_ms > 0u) {
        at(s, T_APP, (uint64_t)(s->host.random(s->host.ctx) % s->send_every_ms) * 1000u);
    }
    return s;
}

static void es_destroy(void *inst)
{
    free(inst);
}

static void es_on_rx(void *inst, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    esb_t *s = (esb_t *)inst;
    uint64_t addr;
    unsigned plen;
    uint8_t pid;
    bool no_ack;
    unsigned i;

    (void)meta;
    if (len < s->aw + 2u) {
        return;
    }
    addr = 0;
    for (i = 0; i < s->aw; i++) {
        addr |= (uint64_t)buf[i] << (8u * i);
    }
    plen = (unsigned)(buf[s->aw] >> 2);
    pid = (uint8_t)(buf[s->aw] & 3u);
    no_ack = (buf[s->aw + 1u] & 1u) != 0u;
    if (plen > MAX_PAYLOAD || s->aw + 2u + plen != len) {
        return; /* other address width or garbage: the CRC would not match */
    }

    if (s->state == ST_PTX_WAIT_ACK) {
        /* any frame on the destination address counts as the ACK */
        if (addr == (s->send_to & addr_mask(s->aw))) {
            event(s, "esb_tx_ds", "pid=%u", (unsigned)s->pid);
            if (plen > 0u) {
                event(s, "esb_ack_payload", "len=%u", plen);
            }
            go_idle(s);
        }
        return;
    }
    if (s->state == ST_IDLE && s->listen) {
        prx_receive(s, addr, pid, no_ack, &buf[s->aw + 2u], plen);
    }
}

static void es_on_tx_done(void *inst)
{
    esb_t *s = (esb_t *)inst;

    if (s->state == ST_PTX_TX) {
        if (s->no_ack) {
            event(s, "esb_tx_ds", "pid=%u no_ack", (unsigned)s->pid);
            go_idle(s);
            return;
        }
        s->state = ST_PTX_WAIT_ACK;
        at(s, T_RX_ON, SETTLE_US);
        at(s, T_ARD, s->ard_us);
    } else if (s->state == ST_PRX_ACK_TX) {
        go_idle(s);
    }
}

static void es_on_timer(void *inst, uint32_t timer_id)
{
    esb_t *s = (esb_t *)inst;

    switch (timer_id) {
    case T_TX_START:
        if (s->state == ST_PTX_SETTLE) {
            transmit(s, s->frame, s->frame_len, ST_PTX_TX);
        }
        break;
    case T_ACK_START:
        if (s->state == ST_PRX_ACK_SETTLE) {
            transmit(s, s->ack, s->ack_len, ST_PRX_ACK_TX);
        }
        break;
    case T_RX_ON:
        if (s->state == ST_PTX_WAIT_ACK || (s->state == ST_IDLE && s->listen)) {
            rx_enable(s, true);
        }
        break;
    case T_ARD:
        if (s->state == ST_PTX_WAIT_ACK) {
            ard_expired(s);
        }
        break;
    case T_APP: {
        int64_t period_ms = (int64_t)s->send_every_ms;
        app_send(s);
        if (s->jitter_ms > 0u) {
            uint32_t span = 2u * s->jitter_ms + 1u;
            period_ms += (int64_t)(s->host.random(s->host.ctx) % span) - (int64_t)s->jitter_ms;
        }
        at(s, T_APP, (uint64_t)(period_ms > 1 ? period_ms : 1) * 1000u);
        break;
    }
    default:
        break;
    }
}

/* Options accepted by es_create, for tools such as the web UI (docs/simulator.md). Keep the
 * defaults in sync with es_create; a test checks them. */
static const char config_schema[] =
    "{\"phy\":[\"nrf24\"],\"options\":["
    "{\"key\":\"addr\",\"type\":\"int\",\"min\":0,\"max\":1099511627775,\"group\":\"radio\","
    "\"help\":\"Own address, up to 5 bytes (default: 0xE7E7E7E7 + node id)\"},"
    "{\"key\":\"aw\",\"type\":\"int\",\"min\":3,\"max\":5,\"default\":5,\"group\":\"radio\","
    "\"help\":\"Address width in bytes\"},"
    "{\"key\":\"pipes\",\"type\":\"int\",\"min\":1,\"max\":5,\"default\":1,\"group\":\"radio\","
    "\"help\":\"Receive pipes: addr, addr+1, ...\"},"
    "{\"key\":\"listen\",\"type\":\"bool\",\"default\":true,\"group\":\"radio\","
    "\"help\":\"Receiver on when not transmitting (PRX)\"},"
    "{\"key\":\"no_ack\",\"type\":\"bool\",\"default\":false,\"group\":\"esb\","
    "\"help\":\"Send without asking for an ACK\"},"
    "{\"key\":\"ard_us\",\"type\":\"int\",\"min\":250,\"max\":4000,\"step\":250,\"default\":500,"
    "\"group\":\"esb\",\"help\":\"Auto retransmit delay\"},"
    "{\"key\":\"arc\",\"type\":\"int\",\"min\":0,\"max\":15,\"default\":3,\"group\":\"esb\","
    "\"help\":\"Auto retransmit count\"},"
    "{\"key\":\"ack_payload_len\",\"type\":\"int\",\"min\":0,\"max\":32,\"default\":0,"
    "\"group\":\"esb\",\"help\":\"Payload carried back in every ACK\"},"
    "{\"key\":\"send_to\",\"type\":\"int\",\"min\":0,\"max\":1099511627775,\"group\":\"traffic\","
    "\"help\":\"Destination address (needed when sending)\"},"
    "{\"key\":\"send_every_ms\",\"type\":\"int\",\"min\":0,\"max\":4294967295,\"default\":0,"
    "\"group\":\"traffic\",\"help\":\"Send period; 0 = no traffic\"},"
    "{\"key\":\"jitter_ms\",\"type\":\"int\",\"min\":0,\"max\":2147483647,\"default\":0,"
    "\"group\":\"traffic\",\"help\":\"Each period varies by up to +-jitter\"},"
    "{\"key\":\"len\",\"type\":\"int\",\"min\":0,\"max\":32,\"default\":8,\"group\":\"traffic\","
    "\"help\":\"Payload bytes (byte 0 carries the node id)\"}"
    "]}";

const ksim_stack_t *ksim_stack_nrf24_esb(void)
{
    static const ksim_stack_t stack = {KSIM_ABI_VERSION, "nrf24-esb",   "1",          es_create,
                                       es_destroy,       es_on_rx,      es_on_tx_done, es_on_timer,
                                       config_schema};
    return &stack;
}

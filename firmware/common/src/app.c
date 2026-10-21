/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kfw/app.h"

#include <stdio.h>
#include <string.h>

#include "kfw/json.h"

#define OUT_MAX 320u
#define RX_PER_POLL 4u

/* ---- output ---- */

static void emit(kfw_app_t *a, kfw_json_out_t *o)
{
    if (kfw_json_end(o)) {
        a->host.write_line(a->host.ctx, o->buf);
    }
}

static void error(kfw_app_t *a, const char *msg)
{
    char buf[OUT_MAX];
    kfw_json_out_t o;
    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "error");
    kfw_json_str(&o, "msg", msg);
    emit(a, &o);
}

static void ok(kfw_app_t *a, const char *cmd)
{
    char buf[OUT_MAX];
    kfw_json_out_t o;
    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "ok");
    kfw_json_str(&o, "cmd", cmd);
    emit(a, &o);
}

static void hello(kfw_app_t *a)
{
    char buf[OUT_MAX];
    kfw_json_out_t o;
    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "hello");
    kfw_json_str(&o, "fw", KFW_FIRMWARE);
    kfw_json_int(&o, "version", KFW_VERSION);
    kfw_json_int(&o, "proto", KFW_PROTOCOL);
    kfw_json_str(&o, "board", a->host.board);
    kfw_json_str(&o, "id", a->host.board_id);
    kfw_json_str(&o, "radio", a->has_radio ? "nrf24l01p" : "none");
    emit(a, &o);
}

static void activity(kfw_app_t *a)
{
    if (a->host.activity != NULL) {
        a->host.activity(a->host.ctx);
    }
}

/* ---- commands ---- */

typedef struct {
    kfw_app_config_t cfg;
    char cmd[12];
    bool options; /* members other than "cmd" */
    char error[64];
} parse_t;

static bool fail(parse_t *p, const char *what, const char *key, size_t key_len)
{
    snprintf(p->error, sizeof(p->error), "%s '%.*s'", what, (int)(key_len > 24u ? 24u : key_len),
             key);
    return false;
}

static bool int_in(const kfw_json_value_t *v, int64_t lo, int64_t hi)
{
    return v->type == KFW_JSON_INT && v->i >= lo && v->i <= hi;
}

static bool member(void *ctx, const char *key, size_t n, const kfw_json_value_t *v)
{
    parse_t *p = (parse_t *)ctx;
    kfw_app_config_t *c = &p->cfg;
    const int64_t addr_max = ((int64_t)1 << 40) - 1;

    if (kfw_json_key_is(key, n, "cmd")) {
        if (v->type != KFW_JSON_STRING || v->len >= sizeof(p->cmd)) {
            return fail(p, "bad value for", key, n);
        }
        memcpy(p->cmd, v->s, v->len);
        p->cmd[v->len] = '\0';
        return true;
    }
    p->options = true;
    if (kfw_json_key_is(key, n, "listen") || kfw_json_key_is(key, n, "no_ack")) {
        if (v->type != KFW_JSON_BOOL) {
            return fail(p, "needs true or false:", key, n);
        }
        if (key[0] == 'l') {
            c->listen = v->b;
        } else {
            c->no_ack = v->b;
        }
        return true;
    }
    if (kfw_json_key_is(key, n, "payload")) {
        size_t len = 0;
        if (v->type != KFW_JSON_STRING ||
            !kfw_hex_decode(v->s, v->len, c->payload, sizeof(c->payload), &len)) {
            return fail(p, "needs up to 32 bytes as hex:", key, n);
        }
        c->fixed = true;
        c->payload_len = (uint8_t)len;
        return true;
    }

#define OPTION(name, lo, hi, field, type)                                                          \
    if (kfw_json_key_is(key, n, name)) {                                                           \
        if (!int_in(v, lo, hi)) {                                                                  \
            return fail(p, "out of range:", key, n);                                               \
        }                                                                                          \
        field = (type)v->i;                                                                        \
        return true;                                                                               \
    }
    OPTION("node", 0, 0xFFFFFFFF, c->node, uint32_t)
    OPTION("channel", 0, 125, c->radio.channel, uint8_t)
    OPTION("rate", 250000, 2000000, c->radio.data_rate, uint32_t)
    OPTION("crc", 1, 2, c->radio.crc_bytes, uint8_t)
    OPTION("pa", 0, 3, c->radio.pa_level, uint8_t)
    OPTION("aw", 3, 5, c->radio.addr_width, uint8_t)
    OPTION("addr", 0, addr_max, c->radio.addr, uint64_t)
    OPTION("pipes", 1, 5, c->radio.pipes, uint8_t)
    OPTION("ard_us", 250, 4000, c->radio.ard_us, uint16_t)
    OPTION("arc", 0, 15, c->radio.arc, uint8_t)
    OPTION("every_ms", 0, 0xFFFFFFFF, c->every_ms, uint32_t)
    OPTION("jitter_ms", 0, 0x7FFFFFFF, c->jitter_ms, uint32_t)
    OPTION("len", 0, 32, c->len, uint8_t)
    OPTION("ack_payload_len", 0, 32, c->ack_payload_len, uint8_t)
#undef OPTION
    if (kfw_json_key_is(key, n, "send_to")) {
        if (!int_in(v, 0, addr_max)) {
            return fail(p, "out of range:", key, n);
        }
        c->send_to = (uint64_t)v->i;
        c->has_dst = true;
        return true;
    }
    return fail(p, "unknown key", key, n);
}

/* The defaults of the nrf24-esb stack and of the chip. */
static void defaults(kfw_app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->radio.channel = 76;
    c->radio.data_rate = 1000000;
    c->radio.crc_bytes = 2;
    c->radio.pa_level = 3;
    c->radio.addr_width = 5;
    c->radio.pipes = 1;
    c->radio.ard_us = 500;
    c->radio.arc = 3;
    c->listen = true;
    c->len = 8;
}

static void preload_ack_payloads(kfw_app_t *a)
{
    uint8_t data[KFW_NRF24_MAX_PAYLOAD];
    uint8_t i;
    uint8_t pipe;

    if (a->cfg.ack_payload_len == 0u) {
        return;
    }
    for (i = 0; i < a->cfg.ack_payload_len; i++) {
        data[i] = (uint8_t)(0xA0u + i); /* like the nrf24-esb stack */
    }
    for (pipe = 1; pipe <= a->cfg.radio.pipes && pipe <= 3u; pipe++) { /* the FIFO holds 3 */
        kfw_nrf24_ack_payload(&a->radio, pipe, data, a->cfg.ack_payload_len);
    }
}

static void idle_radio(kfw_app_t *a)
{
    if (a->cfg.listen) {
        kfw_nrf24_listen(&a->radio);
        preload_ack_payloads(a);
    } else {
        kfw_nrf24_standby(&a->radio);
    }
}

static uint32_t next_random(kfw_app_t *a)
{
    uint32_t x = a->rng; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rng = x;
    return x;
}

static void configure(kfw_app_t *a, const kfw_app_config_t *c)
{
    if (!a->has_radio) {
        error(a, "no nRF24 radio found: check the wiring");
        return;
    }
    if (c->every_ms > 0u && !c->has_dst) {
        error(a, "sending (every_ms) needs send_to");
        return;
    }
    a->running = false;
    a->in_flight = false;
    a->cfg = *c;
    if (!kfw_nrf24_configure(&a->radio, &a->cfg.radio)) {
        a->configured = false;
        error(a, "radio settings out of range");
        return;
    }
    a->configured = true;
    ok(a, "config");
}

static void start(kfw_app_t *a, uint64_t now)
{
    if (!a->configured) {
        error(a, "start needs a config first");
        return;
    }
    a->epoch_us = now;
    a->seq = 0;
    a->rng = (a->cfg.node * 2654435761u) ^ 0x9E3779B9u;
    if (a->rng == 0u) {
        a->rng = 1;
    }
    a->in_flight = false;
    a->running = true;
    if (a->cfg.every_ms > 0u) {
        a->next_send_us = now + (uint64_t)(next_random(a) % a->cfg.every_ms) * 1000u;
    }
    idle_radio(a);
    ok(a, "start");
}

static void stop(kfw_app_t *a)
{
    a->running = false;
    a->in_flight = false;
    if (a->has_radio) {
        kfw_nrf24_standby(&a->radio);
    }
}

void kfw_app_init(kfw_app_t *a, const kfw_app_host_t *host, const kfw_nrf24_io_t *io)
{
    memset(a, 0, sizeof(*a));
    a->host = *host;
    defaults(&a->cfg);
    a->has_radio = io != NULL && kfw_nrf24_probe(&a->radio, io);
}

void kfw_app_line(kfw_app_t *a, const char *line, size_t len, uint64_t now_us)
{
    parse_t p;

    memset(&p, 0, sizeof(p));
    defaults(&p.cfg);
    if (!kfw_json_parse(line, len, member, &p)) {
        error(a, p.error[0] != '\0' ? p.error : "not a JSON object");
        return;
    }
    if (strcmp(p.cmd, "config") == 0) {
        configure(a, &p.cfg);
        return;
    }
    if (p.options) {
        error(a, "only config takes options");
    } else if (strcmp(p.cmd, "hello") == 0) {
        hello(a);
    } else if (strcmp(p.cmd, "start") == 0) {
        start(a, now_us);
    } else if (strcmp(p.cmd, "stop") == 0) {
        stop(a);
        ok(a, "stop");
    } else if (strcmp(p.cmd, "ping") == 0) {
        char buf[OUT_MAX];
        kfw_json_out_t o;
        kfw_json_begin(&o, buf, sizeof(buf));
        kfw_json_str(&o, "ev", "pong");
        kfw_json_uint(&o, "t", a->running ? now_us - a->epoch_us : 0u);
        emit(a, &o);
    } else {
        error(a, "unknown cmd");
    }
}

void kfw_app_disconnected(kfw_app_t *a)
{
    stop(a);
}

/* ---- running ---- */

static void send(kfw_app_t *a, uint64_t now)
{
    uint8_t i;

    if (a->cfg.fixed) {
        memcpy(a->tx_data, a->cfg.payload, a->cfg.payload_len);
        a->tx_len = a->cfg.payload_len;
    } else {
        /* the nrf24-esb stack's test data: byte 0 is the sender */
        for (i = 0; i < a->cfg.len; i++) {
            a->tx_data[i] = i == 0u ? (uint8_t)a->cfg.node : (uint8_t)(a->seq + i);
        }
        a->tx_len = a->cfg.len;
    }
    a->tx_start_us = now;
    a->in_flight = true;
    kfw_nrf24_send(&a->radio, a->cfg.send_to, a->tx_data, a->tx_len, a->cfg.no_ack);
    activity(a);
}

static void finish_tx(kfw_app_t *a, kfw_nrf24_tx_t result, uint8_t retries, const uint8_t *ack,
                      uint8_t ack_len)
{
    char buf[OUT_MAX];
    kfw_json_out_t o;

    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "tx");
    kfw_json_uint(&o, "t", a->tx_start_us - a->epoch_us);
    kfw_json_uint(&o, "seq", a->seq);
    kfw_json_hex(&o, "data", a->tx_data, a->tx_len);
    kfw_json_bool(&o, "ack", result == KFW_NRF24_TX_ACKED && !a->cfg.no_ack);
    if (a->cfg.no_ack) {
        kfw_json_bool(&o, "no_ack", true);
    }
    kfw_json_uint(&o, "retries", retries);
    if (ack_len > 0u) {
        kfw_json_hex(&o, "ack_data", ack, ack_len);
    }
    emit(a, &o);
    a->seq++;
    a->in_flight = false;
    idle_radio(a);
}

static void poll_tx(kfw_app_t *a, uint64_t now)
{
    uint8_t retries = 0;
    uint8_t ack[KFW_NRF24_MAX_PAYLOAD];
    uint8_t ack_len = 0;
    const kfw_nrf24_tx_t r = kfw_nrf24_tx_poll(&a->radio, &retries, ack, &ack_len);

    if (r != KFW_NRF24_TX_PENDING) {
        finish_tx(a, r, retries, ack, ack_len);
    } else if (now - a->tx_start_us > KFW_TX_TIMEOUT_US) {
        error(a, "the radio did not finish sending: check the wiring and the supply");
        (void)kfw_nrf24_configure(&a->radio, &a->cfg.radio);
        a->seq++;
        a->in_flight = false;
        idle_radio(a);
    }
}

static void poll_rx(kfw_app_t *a, uint64_t now)
{
    unsigned n;
    for (n = 0; n < RX_PER_POLL; n++) {
        uint8_t pipe = 0;
        uint8_t data[KFW_NRF24_MAX_PAYLOAD];
        uint8_t len = 0;
        bool rpd = false;
        char buf[OUT_MAX];
        kfw_json_out_t o;

        if (!kfw_nrf24_rx_poll(&a->radio, &pipe, data, &len, &rpd)) {
            return;
        }
        kfw_json_begin(&o, buf, sizeof(buf));
        kfw_json_str(&o, "ev", "rx");
        kfw_json_uint(&o, "t", now - a->epoch_us);
        kfw_json_uint(&o, "pipe", pipe);
        kfw_json_hex(&o, "data", data, len);
        kfw_json_bool(&o, "rpd", rpd);
        emit(a, &o);
        activity(a);
        if (a->cfg.ack_payload_len > 0u) {
            uint8_t ackp[KFW_NRF24_MAX_PAYLOAD];
            uint8_t i;
            for (i = 0; i < a->cfg.ack_payload_len; i++) {
                ackp[i] = (uint8_t)(0xA0u + i);
            }
            kfw_nrf24_ack_payload(&a->radio, pipe, ackp, a->cfg.ack_payload_len);
        }
    }
}

void kfw_app_poll(kfw_app_t *a, uint64_t now)
{
    if (!a->running) {
        return;
    }
    if (a->in_flight) {
        poll_tx(a, now);
    } else if (a->cfg.listen) {
        poll_rx(a, now);
    }

    if (a->cfg.every_ms > 0u && now >= a->next_send_us) {
        int64_t period_ms = (int64_t)a->cfg.every_ms;
        if (a->in_flight) {
            char buf[OUT_MAX];
            kfw_json_out_t o;
            kfw_json_begin(&o, buf, sizeof(buf));
            kfw_json_str(&o, "ev", "tx_busy");
            kfw_json_uint(&o, "t", now - a->epoch_us);
            emit(a, &o);
        } else {
            send(a, now);
        }
        if (a->cfg.jitter_ms > 0u) {
            const uint32_t span = 2u * a->cfg.jitter_ms + 1u;
            period_ms += (int64_t)(next_random(a) % span) - (int64_t)a->cfg.jitter_ms;
        }
        a->next_send_us += (uint64_t)(period_ms > 1 ? period_ms : 1) * 1000u;
        if (a->next_send_us <= now) {
            a->next_send_us = now + (uint64_t)a->cfg.every_ms * 1000u; /* fell behind: no burst */
        }
    }
}

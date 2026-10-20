/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * C client of the ksim remote protocol. Implements ksim_host_t on top of the connection, so any
 * ksim_stack_t runs out of process without changes.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* with _POSIX_C_SOURCE, macOS hides INADDR_LOOPBACK and SO_NOSIGPIPE */
#endif

#include "ksim/remote.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksim/remote_protocol.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef int io_len_t;
typedef int addr_len_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
typedef size_t io_len_t;
typedef socklen_t addr_len_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define BUF_SIZE (KSIM_REMOTE_MAX_MESSAGE + 4u)

typedef struct {
    uint8_t data[BUF_SIZE];
    size_t len; /* write: bytes used; read: body length */
    size_t pos; /* read position */
} msg_t;

typedef struct {
    sock_t sock;
    bool failed;
    uint64_t now;
    ksim_host_t host;
    msg_t out;
    msg_t in;
    char config[KSIM_REMOTE_MAX_MESSAGE];
} client_t;

/* ---- encoding ---- */

static void w_begin(msg_t *m, unsigned type)
{
    m->len = 4;
    m->data[m->len++] = (uint8_t)type;
}

static void w_le(msg_t *m, uint64_t v, unsigned n)
{
    unsigned i;
    if (m->len + n > BUF_SIZE) {
        return;
    }
    for (i = 0; i < n; i++) {
        m->data[m->len++] = (uint8_t)(v >> (8u * i));
    }
}

static void w_bytes(msg_t *m, const void *data, size_t len)
{
    if (len > 0xFFFFu || m->len + 2u + len > BUF_SIZE) {
        len = 0;
    }
    w_le(m, len, 2);
    if (len > 0u) {
        memcpy(&m->data[m->len], data, len);
        m->len += len;
    }
}

static void w_str(msg_t *m, const char *s)
{
    w_bytes(m, s, s != NULL ? strlen(s) : 0u);
}

static uint64_t r_le(client_t *c, unsigned n)
{
    msg_t *m = &c->in;
    uint64_t v = 0;
    unsigned i;
    if (m->len - m->pos < n) {
        c->failed = true;
        return 0;
    }
    for (i = 0; i < n; i++) {
        v |= (uint64_t)m->data[m->pos++] << (8u * i);
    }
    return v;
}

static double r_f64(client_t *c)
{
    uint64_t bits = r_le(c, 8);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* Returns a pointer into the message and its length. */
static const uint8_t *r_bytes(client_t *c, size_t *len)
{
    msg_t *m = &c->in;
    size_t n = (size_t)r_le(c, 2);
    const uint8_t *p = &m->data[m->pos];
    if (m->len - m->pos < n) {
        c->failed = true;
        *len = 0;
        return m->data;
    }
    m->pos += n;
    *len = n;
    return p;
}

/* ---- transport ---- */

static bool send_msg(client_t *c, msg_t *m)
{
    size_t body = m->len - 4u;
    size_t sent = 0;
    unsigned i;
    if (c->failed) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        m->data[i] = (uint8_t)(body >> (8u * i));
    }
    while (sent < m->len) {
        long n = (long)send(c->sock, (const char *)&m->data[sent], (io_len_t)(m->len - sent),
                            SEND_FLAGS);
        if (n <= 0) {
            c->failed = true;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(client_t *c, uint8_t *p, size_t len)
{
    while (len > 0u) {
        long n = (long)recv(c->sock, (char *)p, (io_len_t)len, 0);
        if (n <= 0) {
            c->failed = true;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* Read one message into c->in; returns its type or 0 on failure. */
static unsigned recv_msg(client_t *c)
{
    uint8_t len_bytes[4];
    size_t len;
    if (c->failed || !recv_all(c, len_bytes, 4)) {
        return 0;
    }
    len = (size_t)len_bytes[0] | (size_t)len_bytes[1] << 8 | (size_t)len_bytes[2] << 16 |
          (size_t)len_bytes[3] << 24;
    if (len == 0u || len > KSIM_REMOTE_MAX_MESSAGE || !recv_all(c, c->in.data, len)) {
        c->failed = true;
        return 0;
    }
    c->in.len = len;
    c->in.pos = 1;
    return c->in.data[0];
}

/* Send the request in c->out and wait for its RESULT. */
static int request(client_t *c, int64_t *value)
{
    int32_t status;
    if (!send_msg(c, &c->out) || recv_msg(c) != KSIM_MSG_RESULT) {
        c->failed = true;
        return KSIM_ERR_UNSUPPORTED;
    }
    status = (int32_t)(uint32_t)r_le(c, 4);
    if (value != NULL) {
        *value = (int64_t)r_le(c, 8);
    }
    return status;
}

/* ---- ksim_host_t over the connection ---- */

static uint64_t h_now(void *ctx)
{
    return ((client_t *)ctx)->now;
}

static int h_radio_send(void *ctx, const uint8_t *buf, size_t len)
{
    client_t *c = (client_t *)ctx;
    w_begin(&c->out, KSIM_REQ_RADIO_SEND);
    w_bytes(&c->out, buf, len);
    return request(c, NULL);
}

static void h_timer_set(void *ctx, uint32_t timer_id, uint64_t at_us)
{
    client_t *c = (client_t *)ctx;
    w_begin(&c->out, KSIM_REQ_TIMER_SET);
    w_le(&c->out, timer_id, 4);
    w_le(&c->out, at_us, 8);
    send_msg(c, &c->out);
}

static void h_timer_cancel(void *ctx, uint32_t timer_id)
{
    client_t *c = (client_t *)ctx;
    w_begin(&c->out, KSIM_REQ_TIMER_CANCEL);
    w_le(&c->out, timer_id, 4);
    send_msg(c, &c->out);
}

static uint32_t h_random(void *ctx)
{
    client_t *c = (client_t *)ctx;
    int64_t value = 0;
    w_begin(&c->out, KSIM_REQ_RANDOM);
    request(c, &value);
    return (uint32_t)value;
}

static void h_trace(void *ctx, const char *event, const char *detail)
{
    client_t *c = (client_t *)ctx;
    w_begin(&c->out, KSIM_REQ_TRACE);
    w_str(&c->out, event);
    w_str(&c->out, detail);
    send_msg(c, &c->out);
}

static int h_radio_get(void *ctx, uint32_t param, int64_t *value)
{
    client_t *c = (client_t *)ctx;
    int64_t v = 0;
    int status;
    w_begin(&c->out, KSIM_REQ_RADIO_GET);
    w_le(&c->out, param, 4);
    status = request(c, &v);
    if (value != NULL) {
        *value = v;
    }
    return status;
}

/* Payloads come only from in-process scenarios for now (ksim refuses "payload" on remote nodes): the
 * stack uses its own. */
static int h_app_payload(void *ctx, uint32_t seq, uint8_t *buf, size_t cap)
{
    (void)ctx;
    (void)seq;
    (void)buf;
    (void)cap;
    return -1;
}

static int h_radio_set(void *ctx, uint32_t param, int64_t value)
{
    client_t *c = (client_t *)ctx;
    w_begin(&c->out, KSIM_REQ_RADIO_SET);
    w_le(&c->out, param, 4);
    w_le(&c->out, (uint64_t)value, 8);
    return request(c, NULL);
}

/* ---- main loop ---- */

static bool done(client_t *c, bool ok)
{
    w_begin(&c->out, KSIM_MSG_DONE);
    c->out.data[c->out.len++] = ok ? 1u : 0u;
    return send_msg(c, &c->out);
}

static sock_t connect_loopback(uint16_t port)
{
    struct sockaddr_in addr;
    sock_t s;
    int one = 1;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) {
        return BAD_SOCK;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(s, (const struct sockaddr *)&addr, (addr_len_t)sizeof(addr)) != 0) {
        close_sock(s);
        return BAD_SOCK;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (addr_len_t)sizeof(one));
#if defined(__APPLE__)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, (addr_len_t)sizeof(one));
#endif
    return s;
}

static int serve(client_t *c, const ksim_stack_t *stack, uint32_t node_id)
{
    void *inst = NULL;

    w_begin(&c->out, KSIM_MSG_HELLO);
    w_le(&c->out, KSIM_REMOTE_PROTOCOL_VERSION, 2);
    w_le(&c->out, KSIM_ABI_VERSION, 2);
    w_le(&c->out, node_id, 4);
    w_le(&c->out, KSIM_REMOTE_MODE_LOCKSTEP, 1);
    w_str(&c->out, stack->name);
    w_str(&c->out, stack->version);
    if (!send_msg(c, &c->out)) {
        fprintf(stderr, "ksim-remote: cannot send HELLO\n");
        return 1;
    }

    for (;;) {
        unsigned type = recv_msg(c);
        bool ok = true;

        if (type == KSIM_MSG_ERROR) {
            size_t n;
            const uint8_t *msg = r_bytes(c, &n);
            fprintf(stderr, "ksim-remote: ksim refused node %u: %.*s\n", (unsigned)node_id,
                    (int)n, (const char *)msg);
            return 1;
        }
        if (type == 0u || c->failed) {
            fprintf(stderr, "ksim-remote: connection to ksim lost\n");
            return 1;
        }
        c->now = r_le(c, 8);

        switch (type) {
        case KSIM_EV_CREATE: {
            size_t n;
            const uint8_t *cfg;
            c->host.node_id = (uint32_t)r_le(c, 4);
            c->host.mtu = (size_t)r_le(c, 4);
            cfg = r_bytes(c, &n);
            memcpy(c->config, cfg, n);
            c->config[n] = '\0';
            inst = stack->create(&c->host, c->config);
            ok = inst != NULL;
            break;
        }
        case KSIM_EV_RX: {
            ksim_rx_meta_t meta;
            size_t n;
            const uint8_t *frame;
            meta.rssi_dbm = r_f64(c);
            meta.snr_db = r_f64(c);
            frame = r_bytes(c, &n);
            stack->on_rx(inst, frame, n, &meta);
            break;
        }
        case KSIM_EV_TX_DONE:
            stack->on_tx_done(inst);
            break;
        case KSIM_EV_TIMER:
            stack->on_timer(inst, (uint32_t)r_le(c, 4));
            break;
        case KSIM_EV_DESTROY:
            if (inst != NULL) {
                stack->destroy(inst);
            }
            done(c, true);
            return 0;
        default:
            fprintf(stderr, "ksim-remote: unexpected message type %u\n", type);
            return 1;
        }
        if (!done(c, ok)) {
            fprintf(stderr, "ksim-remote: connection to ksim lost\n");
            return 1;
        }
    }
}

int ksim_remote_serve(const ksim_stack_t *stack, uint16_t port, uint32_t node_id)
{
    client_t *c;
    int result;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
#endif
    c = (client_t *)calloc(1, sizeof(client_t));
    if (c == NULL || stack == NULL) {
        free(c);
        return 1;
    }
    c->sock = connect_loopback(port);
    if (c->sock == BAD_SOCK) {
        fprintf(stderr, "ksim-remote: cannot connect to 127.0.0.1:%u\n", (unsigned)port);
        free(c);
        return 1;
    }
    c->host.ctx = c;
    c->host.node_id = node_id;
    c->host.now_us = h_now;
    c->host.radio_send = h_radio_send;
    c->host.timer_set = h_timer_set;
    c->host.timer_cancel = h_timer_cancel;
    c->host.random = h_random;
    c->host.trace = h_trace;
    c->host.radio_get = h_radio_get;
    c->host.radio_set = h_radio_set;
    c->host.app_payload = h_app_payload;

    result = serve(c, stack, node_id);
    close_sock(c->sock);
    free(c);
#if defined(_WIN32)
    WSACleanup();
#endif
    return result;
}

int ksim_remote_serve_env(const ksim_stack_t *stack)
{
    const char *port = getenv(KSIM_ENV_PORT);
    const char *node = getenv(KSIM_ENV_NODE_ID);
    if (port == NULL || node == NULL) {
        fprintf(stderr, "ksim-remote: %s and %s must be set (ksim sets them when it starts the "
                        "process)\n",
                KSIM_ENV_PORT, KSIM_ENV_NODE_ID);
        return 2;
    }
    return ksim_remote_serve(stack, (uint16_t)strtoul(port, NULL, 10),
                             (uint32_t)strtoul(node, NULL, 10));
}

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* with _POSIX_C_SOURCE, macOS hides INADDR_LOOPBACK and SO_NOSIGPIPE */
#endif

#include "kervan/port/udp.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef int sock_len_t;
#define poll WSAPoll
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
typedef size_t sock_len_t;
#endif

static sock_t get_sock(const kv_udp_t *udp)
{
    return (sock_t)udp->sock;
}

static void close_sock(sock_t s)
{
#if defined(_WIN32)
    closesocket(s);
    WSACleanup();
#else
    close(s);
#endif
}

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

/* ---- PAL implementation ---- */

static uint32_t udp_now_ms(void *ctx)
{
    (void)ctx;
#if defined(_WIN32)
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

static kv_status_t udp_radio_send(void *ctx, const uint8_t *buf, size_t len)
{
    kv_udp_t *udp = (kv_udp_t *)ctx;
    size_t i;

    if (len > udp->pal.mtu) {
        return KV_ERR_INVALID_ARG;
    }
    for (i = 0; i < udp->peer_count; i++) {
        struct sockaddr_in to = loopback(udp->peers[i]);
        /* a lost datagram is just a lost frame, like on a real radio */
        (void)sendto(get_sock(udp), (const char *)buf, (sock_len_t)len, 0,
                     (const struct sockaddr *)&to, (socklen_t)sizeof(to));
    }
    udp->tx_done_pending = true;
    return KV_OK;
}

static uint32_t udp_random(void *ctx)
{
    kv_udp_t *udp = (kv_udp_t *)ctx;
    uint32_t x = udp->rng; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    udp->rng = x;
    return x;
}

static void udp_log(void *ctx, kv_log_level_t level, const char *msg)
{
    static const char *const names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    const kv_udp_t *udp = (const kv_udp_t *)ctx;
    fprintf(stderr, "[udp:%u] %s: %s\n", (unsigned)udp->port, names[level], msg);
}

/* ---- public API ---- */

kv_status_t kv_udp_open(kv_udp_t *udp, uint16_t port, size_t mtu, uint32_t seed)
{
    struct sockaddr_in addr = loopback(port);
    socklen_t addr_len = (socklen_t)sizeof(addr);
    sock_t s;

    if (udp == NULL || mtu > KV_MTU_MAX) {
        return KV_ERR_INVALID_ARG;
    }
    memset(udp, 0, sizeof(*udp));

#if defined(_WIN32)
    {
        WSADATA wsa;
        BOOL no_reset = FALSE;
        DWORD ret_len = 0;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return KV_ERR_UNSUPPORTED;
        }
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            WSACleanup();
            return KV_ERR_UNSUPPORTED;
        }
        /* otherwise sending to a port nobody listens on breaks the next recvfrom */
        WSAIoctl(s, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), NULL, 0, &ret_len, NULL,
                 NULL);
    }
#else
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return KV_ERR_UNSUPPORTED;
    }
#endif

    if (bind(s, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0 ||
        getsockname(s, (struct sockaddr *)&addr, &addr_len) != 0) {
        close_sock(s);
        return KV_ERR_BUSY; /* port in use */
    }

    udp->sock = (uintptr_t)s;
    udp->open = true;
    udp->port = ntohs(addr.sin_port);
    udp->rng = seed != 0u ? seed : 0x6B657276u; /* xorshift must not start at 0 */
    udp->pal.ctx = udp;
    udp->pal.mtu = mtu;
    udp->pal.now_ms = udp_now_ms;
    udp->pal.radio_send = udp_radio_send;
    udp->pal.random = udp_random;
    udp->pal.log = udp_log;
    return KV_OK;
}

void kv_udp_close(kv_udp_t *udp)
{
    if (udp != NULL && udp->open) {
        close_sock(get_sock(udp));
        udp->open = false;
    }
}

uint16_t kv_udp_port(const kv_udp_t *udp)
{
    return udp != NULL ? udp->port : 0u;
}

kv_status_t kv_udp_add_peer(kv_udp_t *udp, uint16_t port)
{
    if (udp == NULL || port == 0u) {
        return KV_ERR_INVALID_ARG;
    }
    if (udp->peer_count >= KV_UDP_MAX_PEERS) {
        return KV_ERR_NO_SPACE;
    }
    udp->peers[udp->peer_count++] = port;
    return KV_OK;
}

kv_status_t kv_udp_node_init(kv_udp_t *udp, kv_node_t *node, const kv_node_config_t *cfg)
{
    kv_status_t st;

    if (udp == NULL || !udp->open) {
        return KV_ERR_INVALID_ARG;
    }
    st = kv_node_init(node, &udp->pal, cfg);
    if (st == KV_OK) {
        udp->node = node;
    }
    return st;
}

/* Wait up to timeout_ms for a datagram. */
static bool wait_readable(const kv_udp_t *udp, uint32_t timeout_ms)
{
    struct pollfd p;
    p.fd = get_sock(udp);
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, (int)timeout_ms) > 0 && (p.revents & POLLIN) != 0;
}

static void flush_tx_done(kv_udp_t *udp)
{
    /* UDP transmits instantly; loop because a completion can start a retransmission */
    while (udp->tx_done_pending) {
        udp->tx_done_pending = false;
        kv_node_on_tx_done(udp->node);
    }
}

kv_status_t kv_udp_poll(kv_udp_t *udp, uint32_t max_wait_ms)
{
    uint8_t buf[KV_MTU_MAX + 1u]; /* one extra byte to detect oversized datagrams */
    uint32_t wait = max_wait_ms;
    uint32_t at;

    if (udp == NULL || !udp->open || udp->node == NULL) {
        return KV_ERR_INVALID_ARG;
    }

    flush_tx_done(udp);
    if (kv_node_next_deadline(udp->node, &at)) {
        int32_t until = (int32_t)(at - udp_now_ms(udp));
        if (until <= 0) {
            wait = 0;
        } else if ((uint32_t)until < wait) {
            wait = (uint32_t)until;
        }
    }

    while (wait_readable(udp, wait)) {
        long n = (long)recvfrom(get_sock(udp), (char *)buf, (sock_len_t)sizeof(buf), 0, NULL,
                                NULL);
        if (n < 0) {
            break;
        }
        if ((size_t)n <= udp->pal.mtu) {
            kv_node_on_rx(udp->node, buf, (size_t)n, NULL);
            flush_tx_done(udp);
        }
        wait = 0; /* drain what is queued, then return */
    }

    kv_node_tick(udp->node);
    flush_tx_done(udp);
    return KV_OK;
}

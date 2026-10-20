/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two (or more) Kervan nodes talking over UDP on one PC.
 *
 *   terminal 1:  kervan_udp_demo --addr 1 --port 40001 --peer 40002 --dst 2
 *   terminal 2:  kervan_udp_demo --addr 2 --port 40002 --peer 40001 --dst 1
 *
 * Each node sends a numbered message with ACK request every --interval ms and prints
 * what it receives and whether its messages were acknowledged.
 */

#include "kervan/kervan.h"
#include "kervan/port/udp.h"
#include "kervan/topic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_ID 1u
#define LORA_MTU 51u
#define TOPIC_NAME "demo/hello"

typedef struct {
    uint16_t addr;
    uint16_t port;
    uint16_t peers[KV_UDP_MAX_PEERS];
    size_t peer_count;
    uint16_t dst;
    uint32_t interval_ms;
} options_t;

static void usage(void)
{
    fprintf(stderr, "usage: kervan_udp_demo --addr N --port P --peer P [--peer P ...]\n"
                    "                       [--dst N|broadcast] [--interval MS]\n");
    exit(2);
}

static uint16_t parse_u16(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (*s == '\0' || *end != '\0' || v > 0xFFFFul) {
        usage();
    }
    return (uint16_t)v;
}

static options_t parse_args(int argc, char **argv)
{
    options_t o;
    int i;

    memset(&o, 0, sizeof(o));
    o.dst = KV_ADDR_BROADCAST;
    o.interval_ms = 3000;
    for (i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i];
        const char *v = argv[i + 1];
        if (strcmp(k, "--addr") == 0) {
            o.addr = parse_u16(v);
        } else if (strcmp(k, "--port") == 0) {
            o.port = parse_u16(v);
        } else if (strcmp(k, "--peer") == 0 && o.peer_count < KV_UDP_MAX_PEERS) {
            o.peers[o.peer_count++] = parse_u16(v);
        } else if (strcmp(k, "--dst") == 0) {
            o.dst = strcmp(v, "broadcast") == 0 ? KV_ADDR_BROADCAST : parse_u16(v);
        } else if (strcmp(k, "--interval") == 0) {
            o.interval_ms = parse_u16(v);
        } else {
            usage();
        }
    }
    if (i != argc || o.addr == 0u || o.port == 0u || o.peer_count == 0u) {
        usage();
    }
    return o;
}

static void on_message(void *user, const kv_message_t *msg)
{
    (void)user;
    printf("<- from %u (seq %u, hops %u): %.*s\n", (unsigned)msg->src, (unsigned)msg->seq,
           (unsigned)msg->hops, (int)msg->len, (const char *)msg->data);
    fflush(stdout);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    (void)user;
    printf("   seq %u %s\n", (unsigned)seq, result == KV_OK ? "acknowledged" : "NOT acknowledged");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    options_t o = parse_args(argc, argv);
    kv_udp_t udp;
    kv_node_t node;
    kv_node_config_t cfg;
    uint32_t next_send;
    unsigned counter = 0;
    size_t i;

    if (kv_udp_open(&udp, o.port, LORA_MTU, (uint32_t)o.addr * 2654435761u) != KV_OK) {
        fprintf(stderr, "cannot bind 127.0.0.1:%u\n", (unsigned)o.port);
        return 1;
    }
    for (i = 0; i < o.peer_count; i++) {
        kv_udp_add_peer(&udp, o.peers[i]);
    }

    kv_node_config_default(&cfg);
    cfg.addr = o.addr;
    cfg.net_id = NET_ID;
    cfg.ack_timeout_ms = 1000;
    cfg.duty_cycle_permille = 0; /* no airtime regulation on UDP */
    cfg.on_message = on_message;
    cfg.on_ack = on_ack;
    if (kv_udp_node_init(&udp, &node, &cfg) != KV_OK) {
        fprintf(stderr, "invalid node configuration\n");
        return 1;
    }
    kv_node_subscribe(&node, kv_topic_id(TOPIC_NAME)); /* hear broadcasts too */

    printf("Kervan %s: node %u on 127.0.0.1:%u, MTU %u\n", kv_version_string(), (unsigned)o.addr,
           (unsigned)o.port, (unsigned)LORA_MTU);
    fflush(stdout);

    next_send = udp.pal.now_ms(udp.pal.ctx) + o.interval_ms;
    for (;;) {
        uint32_t now = udp.pal.now_ms(udp.pal.ctx);

        if ((int32_t)(now - next_send) >= 0) {
            char text[32];
            bool ack = o.dst != KV_ADDR_BROADCAST;
            uint8_t seq = 0;
            int n = snprintf(text, sizeof(text), "hello #%u from %u", ++counter,
                             (unsigned)o.addr);
            kv_status_t st = kv_node_publish(&node, o.dst, kv_topic_id(TOPIC_NAME), text, (size_t)n,
                                             ack ? KV_QOS1 : KV_QOS0, &seq);
            if (st == KV_OK) {
                printf("-> seq %u: %s\n", (unsigned)seq, text);
            } else {
                printf("-> send skipped (status %d)\n", (int)st);
            }
            fflush(stdout);
            next_send = now + o.interval_ms;
        }
        kv_udp_poll(&udp, 50);
    }
}

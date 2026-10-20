/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* Integration test: real nodes over real UDP sockets on 127.0.0.1. */

#include "kervan/port/udp.h"
#include "unity.h"

#include <string.h>

#define NET 3u
#define LORA_MTU 51u
#define LOOP_LIMIT_MS 3000u /* fail instead of hanging if something is wrong */

typedef struct {
    size_t data_count;
    uint8_t data[KV_MTU_MAX];
    size_t data_len;
    size_t ack_count;
    kv_status_t ack_result;
} events_t;

static kv_udp_t udp_a, udp_b, udp_c;
static kv_node_t node_a, node_b, node_c;
static events_t ev_a, ev_b, ev_c;

static void on_data(void *user, const kv_frame_t *frame, const kv_rx_meta_t *meta)
{
    events_t *ev = (events_t *)user;
    (void)meta;
    ev->data_count++;
    ev->data_len = frame->payload_len;
    memcpy(ev->data, frame->payload, frame->payload_len);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    events_t *ev = (events_t *)user;
    (void)seq;
    ev->ack_count++;
    ev->ack_result = result;
}

static void open_node(kv_udp_t *udp, kv_node_t *node, events_t *ev, uint16_t addr, uint32_t seed)
{
    kv_node_config_t cfg;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_udp_open(udp, 0, LORA_MTU, seed));
    TEST_ASSERT_NOT_EQUAL(0, kv_udp_port(udp));
    kv_node_config_default(&cfg);
    cfg.addr = addr;
    cfg.net_id = NET;
    cfg.ack_timeout_ms = 100;
    cfg.max_retries = 1;
    cfg.on_data = on_data;
    cfg.on_ack = on_ack;
    cfg.user = ev;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_udp_node_init(udp, node, &cfg));
}

static void make_link(kv_udp_t *from, kv_udp_t *to)
{
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_udp_add_peer(from, kv_udp_port(to)));
}

/* Run all three main loops until the condition holds or the time limit hits. */
#define RUN_UNTIL(cond)                                                                            \
    do {                                                                                           \
        uint32_t start_ = udp_a.pal.now_ms(udp_a.pal.ctx);                                         \
        while (!(cond) && udp_a.pal.now_ms(udp_a.pal.ctx) - start_ < LOOP_LIMIT_MS) {              \
            kv_udp_poll(&udp_a, 5);                                                                \
            kv_udp_poll(&udp_b, 5);                                                                \
            kv_udp_poll(&udp_c, 5);                                                                \
        }                                                                                          \
    } while (0)

void setUp(void)
{
    memset(&ev_a, 0, sizeof(ev_a));
    memset(&ev_b, 0, sizeof(ev_b));
    memset(&ev_c, 0, sizeof(ev_c));
    open_node(&udp_a, &node_a, &ev_a, 0x0001, 1);
    open_node(&udp_b, &node_b, &ev_b, 0x0002, 2);
    open_node(&udp_c, &node_c, &ev_c, 0x0003, 3);
}

void tearDown(void)
{
    kv_udp_close(&udp_a);
    kv_udp_close(&udp_b);
    kv_udp_close(&udp_c);
}

static const uint8_t msg[] = "merhaba";

static void test_unicast_with_ack(void)
{
    make_link(&udp_a, &udp_b);
    make_link(&udp_b, &udp_a);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, 0x0002, msg, sizeof(msg), true, NULL));
    RUN_UNTIL(ev_a.ack_count > 0);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_MEMORY(msg, ev_b.data, sizeof(msg));
    TEST_ASSERT_EQUAL_size_t(1, ev_a.ack_count);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev_a.ack_result);
}

static void test_broadcast_reaches_all_peers(void)
{
    make_link(&udp_a, &udp_b);
    make_link(&udp_a, &udp_c);

    TEST_ASSERT_EQUAL_INT(KV_OK,
                          kv_node_send(&node_a, KV_ADDR_BROADCAST, msg, sizeof(msg), false, NULL));
    RUN_UNTIL(ev_b.data_count > 0 && ev_c.data_count > 0);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_size_t(1, ev_c.data_count);
}

static void test_one_way_link_times_out(void)
{
    /* A hears nobody: B gets the data but its ACKs go nowhere */
    make_link(&udp_a, &udp_b);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, 0x0002, msg, sizeof(msg), true, NULL));
    RUN_UNTIL(ev_a.ack_count > 0);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count); /* retry was deduplicated */
    TEST_ASSERT_EQUAL_size_t(1, ev_a.ack_count);
    TEST_ASSERT_EQUAL_INT(KV_ERR_TIMEOUT, ev_a.ack_result);
}

static void test_unlinked_node_hears_nothing(void)
{
    make_link(&udp_a, &udp_b);

    TEST_ASSERT_EQUAL_INT(KV_OK,
                          kv_node_send(&node_a, KV_ADDR_BROADCAST, msg, sizeof(msg), false, NULL));
    RUN_UNTIL(ev_b.data_count > 0);
    kv_udp_poll(&udp_c, 50);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_size_t(0, ev_c.data_count);
}

static void test_mtu_is_enforced(void)
{
    uint8_t big[LORA_MTU];

    memset(big, 0x55, sizeof(big));
    make_link(&udp_a, &udp_b);
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE,
                          kv_node_send(&node_a, 0x0002, big, LORA_MTU - KV_FRAME_HEADER_LEN + 1u,
                                       false, NULL));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, 0x0002, big,
                                              LORA_MTU - KV_FRAME_HEADER_LEN, false, NULL));
    RUN_UNTIL(ev_b.data_count > 0);
    TEST_ASSERT_EQUAL_size_t(LORA_MTU - KV_FRAME_HEADER_LEN, ev_b.data_len);
}

static void test_port_argument_checks(void)
{
    kv_udp_t u;
    uint16_t taken = kv_udp_port(&udp_a);
    size_t i;

    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY, kv_udp_open(&u, taken, LORA_MTU, 1)); /* port in use */
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_udp_open(&u, 0, KV_MTU_MAX + 1u, 1));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_udp_add_peer(&udp_a, 0));
    for (i = 0; i < KV_UDP_MAX_PEERS; i++) {
        TEST_ASSERT_EQUAL_INT(KV_OK, kv_udp_add_peer(&udp_a, taken));
    }
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_udp_add_peer(&udp_a, taken));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unicast_with_ack);
    RUN_TEST(test_broadcast_reaches_all_peers);
    RUN_TEST(test_one_way_link_times_out);
    RUN_TEST(test_unlinked_node_hears_nothing);
    RUN_TEST(test_mtu_is_enforced);
    RUN_TEST(test_port_argument_checks);
    return UNITY_END();
}

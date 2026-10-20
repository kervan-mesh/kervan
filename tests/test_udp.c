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
#define TOPIC 0x0042u
#define LOOP_LIMIT_MS 3000u /* fail instead of hanging if something is wrong */

typedef struct {
    size_t data_count;
    uint8_t data[KV_MAX_MSG_SIZE];
    uint8_t hops;
    size_t data_len;
    size_t ack_count;
    kv_status_t ack_result;
} events_t;

static kv_udp_t udp_a, udp_b, udp_c;
static kv_node_t node_a, node_b, node_c;
static events_t ev_a, ev_b, ev_c;

static void on_message(void *user, const kv_message_t *msg)
{
    events_t *ev = (events_t *)user;
    ev->data_count++;
    ev->data_len = msg->len;
    ev->hops = msg->hops;
    memcpy(ev->data, msg->data, msg->len);
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
    cfg.duty_cycle_permille = 0; // no regulation on UDP
    cfg.relay = false;
    cfg.on_message = on_message;
    cfg.on_ack = on_ack;
    cfg.user = ev;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_udp_node_init(udp, node, &cfg));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_subscribe(node, TOPIC));
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

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node_a, 0x0002, TOPIC, msg, sizeof(msg), KV_QOS1, NULL));
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
                          kv_node_publish(&node_a, KV_ADDR_BROADCAST, TOPIC, msg, sizeof(msg), KV_QOS0, NULL));
    RUN_UNTIL(ev_b.data_count > 0 && ev_c.data_count > 0);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_size_t(1, ev_c.data_count);
}

static void test_one_way_link_times_out(void)
{
    /* A hears nobody: B gets the data but its ACKs go nowhere */
    make_link(&udp_a, &udp_b);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node_a, 0x0002, TOPIC, msg, sizeof(msg), KV_QOS1, NULL));
    RUN_UNTIL(ev_a.ack_count > 0);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count); /* retry was deduplicated */
    TEST_ASSERT_EQUAL_size_t(1, ev_a.ack_count);
    TEST_ASSERT_EQUAL_INT(KV_ERR_TIMEOUT, ev_a.ack_result);
}

static void test_unlinked_node_hears_nothing(void)
{
    make_link(&udp_a, &udp_b);

    TEST_ASSERT_EQUAL_INT(KV_OK,
                          kv_node_publish(&node_a, KV_ADDR_BROADCAST, TOPIC, msg, sizeof(msg), KV_QOS0, NULL));
    RUN_UNTIL(ev_b.data_count > 0);
    kv_udp_poll(&udp_c, 50);

    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_size_t(0, ev_c.data_count);
}

static void test_large_message_is_fragmented(void)
{
    uint8_t big[KV_MAX_MSG_SIZE];
    size_t i;

    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)i;
    }
    make_link(&udp_a, &udp_b);
    make_link(&udp_b, &udp_a);
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_node_publish(&node_a, 0x0002, TOPIC, big,
                                                           KV_MAX_MSG_SIZE - 1u, KV_QOS0, NULL));
    /* 200 bytes over a 51-byte MTU: 5 fragments, acknowledged once */
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node_a, 0x0002, TOPIC, big, 200, KV_QOS1, NULL));
    RUN_UNTIL(ev_a.ack_count > 0);
    TEST_ASSERT_EQUAL_size_t(1, ev_b.data_count);
    TEST_ASSERT_EQUAL_size_t(200, ev_b.data_len);
    TEST_ASSERT_EQUAL_MEMORY(big, ev_b.data, 200);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev_a.ack_result);
}

/* A - B - C over real sockets: C is out of A's range, B relays. */
static void test_two_hops_over_udp(void)
{
    node_b.cfg.relay = true;
    node_b.cfg.relay_window_ms = 50;
    node_a.cfg.ack_timeout_ms = 2000; /* two relays (data, then ACK) must fit */
    make_link(&udp_a, &udp_b);
    make_link(&udp_b, &udp_a);
    make_link(&udp_b, &udp_c);
    make_link(&udp_c, &udp_b);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node_a, 0x0003, TOPIC, msg, sizeof(msg), KV_QOS1, NULL));
    RUN_UNTIL(ev_a.ack_count > 0);
    TEST_ASSERT_EQUAL_size_t(1, ev_c.data_count);
    TEST_ASSERT_EQUAL_UINT8(1, ev_c.hops);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev_a.ack_result);
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
    RUN_TEST(test_large_message_is_fragmented);
    RUN_TEST(test_two_hops_over_udp);
    RUN_TEST(test_port_argument_checks);
    return UNITY_END();
}

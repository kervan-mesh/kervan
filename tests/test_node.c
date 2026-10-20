/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/node.h"
#include "unity.h"

#include <string.h>

/* ---- mock PAL: records transmissions, time is driven by the test ---- */

#define MOCK_MAX_FRAMES 8

typedef struct {
    uint32_t random_value;
    bool fail_send; /* radio_send reports an error */
    size_t sent_count;
    uint8_t sent[MOCK_MAX_FRAMES][KV_MTU_MAX];
    size_t sent_len[MOCK_MAX_FRAMES];
    size_t log_count;
    kv_log_level_t last_log_level;
} mock_radio_t;

static uint32_t clock_ms;

static void mock_log(void *ctx, kv_log_level_t level, const char *msg)
{
    mock_radio_t *r = (mock_radio_t *)ctx;
    (void)msg;
    r->log_count++;
    r->last_log_level = level;
}

static uint32_t mock_now(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static kv_status_t mock_send(void *ctx, const uint8_t *buf, size_t len)
{
    mock_radio_t *r = (mock_radio_t *)ctx;
    if (r->fail_send) {
        return KV_ERR_BUSY;
    }
    TEST_ASSERT_TRUE(r->sent_count < MOCK_MAX_FRAMES);
    memcpy(r->sent[r->sent_count], buf, len);
    r->sent_len[r->sent_count] = len;
    r->sent_count++;
    return KV_OK;
}

static uint32_t mock_random(void *ctx)
{
    return ((mock_radio_t *)ctx)->random_value;
}

/* ---- recorded callbacks ---- */

static size_t data_count;
static uint16_t data_src;
static uint8_t data_payload[KV_MTU_MAX];
static size_t data_len;
static size_t ack_count;
static uint8_t ack_seq;
static kv_status_t ack_result;

static void on_data(void *user, const kv_frame_t *frame, const kv_rx_meta_t *meta)
{
    (void)user;
    (void)meta;
    data_count++;
    data_src = frame->hdr.src;
    data_len = frame->payload_len;
    memcpy(data_payload, frame->payload, frame->payload_len);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    (void)user;
    ack_count++;
    ack_seq = seq;
    ack_result = result;
}

/* ---- two nodes, A (0x0001) and B (0x0002), net 7, LoRa-sized MTU ---- */

#define ADDR_A 0x0001u
#define ADDR_B 0x0002u
#define NET 7u
#define MTU 51u
#define TIMEOUT 1000u

static mock_radio_t radio_a, radio_b;
static kv_node_t node_a, node_b;

static kv_pal_t make_pal(mock_radio_t *r)
{
    kv_pal_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.ctx = r;
    pal.mtu = MTU;
    pal.now_ms = mock_now;
    pal.radio_send = mock_send;
    pal.random = mock_random;
    pal.log = mock_log;
    return pal;
}

static void init_node(kv_node_t *n, mock_radio_t *r, uint16_t addr)
{
    kv_pal_t pal = make_pal(r);
    kv_node_config_t cfg;

    kv_node_config_default(&cfg);
    cfg.addr = addr;
    cfg.net_id = NET;
    cfg.ack_timeout_ms = TIMEOUT;
    cfg.max_retries = 2;
    cfg.on_data = on_data;
    cfg.on_ack = on_ack;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_init(n, &pal, &cfg));
}

/* Hand frame i sent by `from` to node `to`, then finish the sender's transmission. */
static void air(mock_radio_t *from_radio, kv_node_t *from, size_t i, kv_node_t *to)
{
    kv_node_on_tx_done(from);
    kv_node_on_rx(to, from_radio->sent[i], from_radio->sent_len[i], NULL);
}

void setUp(void)
{
    memset(&radio_a, 0, sizeof(radio_a));
    memset(&radio_b, 0, sizeof(radio_b));
    radio_a.random_value = 0x12345678u;
    radio_b.random_value = 0x9Au;
    clock_ms = 1000;
    data_count = 0;
    data_len = 0;
    ack_count = 0;
    init_node(&node_a, &radio_a, ADDR_A);
    init_node(&node_b, &radio_b, ADDR_B);
}

void tearDown(void) {}

static const uint8_t hello[] = {'h', 'i'};

/* ---- tests ---- */

static void test_init_rejects_bad_config(void)
{
    kv_pal_t pal = make_pal(&radio_a);
    kv_node_config_t cfg;
    kv_node_t n;

    kv_node_config_default(&cfg);
    cfg.addr = ADDR_A;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_init(&n, &pal, &cfg));

    cfg.addr = KV_ADDR_BROADCAST;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    cfg.addr = ADDR_A;

    pal.mtu = KV_FRAME_HEADER_LEN;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    pal.mtu = KV_MTU_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    pal.mtu = MTU;

    pal.radio_send = NULL;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
}

static void test_send_builds_frame_with_random_start_seq(void)
{
    kv_frame_t f;
    uint8_t seq = 0;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, &seq));
    TEST_ASSERT_EQUAL_UINT8(0x78, seq);
    TEST_ASSERT_EQUAL_size_t(1, radio_a.sent_count);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(radio_a.sent[0], radio_a.sent_len[0], &f));
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_DATA, f.hdr.type);
    TEST_ASSERT_EQUAL_HEX8(NET, f.hdr.net_id);
    TEST_ASSERT_EQUAL_HEX16(ADDR_A, f.hdr.src);
    TEST_ASSERT_EQUAL_HEX16(ADDR_B, f.hdr.dst);
    TEST_ASSERT_EQUAL_UINT8(KV_HOP_LIMIT_DEFAULT, f.hdr.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(0x78, f.hdr.seq);
    TEST_ASSERT_EQUAL_MEMORY(hello, f.payload, sizeof(hello));

    kv_node_on_tx_done(&node_a);
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, &seq));
    TEST_ASSERT_EQUAL_UINT8(0x79, seq);
}

static void test_data_is_delivered(void)
{
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL);
    air(&radio_a, &node_a, 0, &node_b);

    TEST_ASSERT_EQUAL_size_t(1, data_count);
    TEST_ASSERT_EQUAL_HEX16(ADDR_A, data_src);
    TEST_ASSERT_EQUAL_size_t(sizeof(hello), data_len);
    TEST_ASSERT_EQUAL_MEMORY(hello, data_payload, sizeof(hello));
    TEST_ASSERT_EQUAL_size_t(0, radio_b.sent_count); /* no ACK requested */
}

static void test_broadcast_is_delivered(void)
{
    kv_node_send(&node_a, KV_ADDR_BROADCAST, hello, sizeof(hello), false, NULL);
    air(&radio_a, &node_a, 0, &node_b);
    TEST_ASSERT_EQUAL_size_t(1, data_count);
}

static void test_foreign_frames_are_dropped(void)
{
    mock_radio_t radio_c;
    kv_node_t node_c;

    /* other network */
    memset(&radio_c, 0, sizeof(radio_c));
    init_node(&node_c, &radio_c, 0x0003u);
    node_c.cfg.net_id = NET + 1u;
    kv_node_send(&node_c, ADDR_B, hello, sizeof(hello), false, NULL);
    air(&radio_c, &node_c, 0, &node_b);

    /* addressed to someone else */
    kv_node_send(&node_a, 0x0003u, hello, sizeof(hello), false, NULL);
    air(&radio_a, &node_a, 0, &node_b);

    /* our own frame echoed back */
    kv_node_on_rx(&node_a, radio_a.sent[0], radio_a.sent_len[0], NULL);

    TEST_ASSERT_EQUAL_size_t(0, data_count);
}

static void test_duplicate_is_delivered_once(void)
{
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL);
    air(&radio_a, &node_a, 0, &node_b);
    kv_node_on_rx(&node_b, radio_a.sent[0], radio_a.sent_len[0], NULL);
    TEST_ASSERT_EQUAL_size_t(1, data_count);
}

static void test_duplicate_window_expires(void)
{
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL);
    air(&radio_a, &node_a, 0, &node_b);
    clock_ms += KV_DEDUP_WINDOW_MS;
    kv_node_on_rx(&node_b, radio_a.sent[0], radio_a.sent_len[0], NULL);
    TEST_ASSERT_EQUAL_size_t(2, data_count);
}

static void test_ack_round_trip(void)
{
    uint8_t seq = 0;
    uint32_t at;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, &seq));
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, &at)); /* still transmitting */

    air(&radio_a, &node_a, 0, &node_b);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
    TEST_ASSERT_EQUAL_UINT32(clock_ms + TIMEOUT, at);
    TEST_ASSERT_EQUAL_size_t(1, data_count);
    TEST_ASSERT_EQUAL_size_t(1, radio_b.sent_count);

    air(&radio_b, &node_b, 0, &node_a);
    TEST_ASSERT_EQUAL_size_t(1, ack_count);
    TEST_ASSERT_EQUAL_UINT8(seq, ack_seq);
    TEST_ASSERT_EQUAL_INT(KV_OK, ack_result);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, &at));
}

static void test_lost_ack_triggers_retransmit_and_reack(void)
{
    uint32_t at;

    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL);
    air(&radio_a, &node_a, 0, &node_b);
    kv_node_on_tx_done(&node_b); /* B's ACK is lost */

    TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
    clock_ms = at - 1u;
    kv_node_tick(&node_a);
    TEST_ASSERT_EQUAL_size_t(1, radio_a.sent_count); /* not yet */
    clock_ms = at;
    kv_node_tick(&node_a);
    TEST_ASSERT_EQUAL_size_t(2, radio_a.sent_count);
    TEST_ASSERT_EQUAL_MEMORY(radio_a.sent[0], radio_a.sent[1], radio_a.sent_len[0]);

    air(&radio_a, &node_a, 1, &node_b);
    TEST_ASSERT_EQUAL_size_t(1, data_count);         /* not delivered twice */
    TEST_ASSERT_EQUAL_size_t(2, radio_b.sent_count); /* but ACKed again */

    air(&radio_b, &node_b, 1, &node_a);
    TEST_ASSERT_EQUAL_size_t(1, ack_count);
    TEST_ASSERT_EQUAL_INT(KV_OK, ack_result);
}

static void test_ack_times_out_after_retries(void)
{
    uint8_t seq = 0;
    uint32_t at;
    int i;

    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, &seq);
    for (i = 0; i < 3; i++) {
        kv_node_on_tx_done(&node_a);
        TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
        clock_ms = at;
        kv_node_tick(&node_a);
    }
    TEST_ASSERT_EQUAL_size_t(3, radio_a.sent_count); /* first attempt + 2 retries */
    TEST_ASSERT_EQUAL_size_t(1, ack_count);
    TEST_ASSERT_EQUAL_UINT8(seq, ack_seq);
    TEST_ASSERT_EQUAL_INT(KV_ERR_TIMEOUT, ack_result);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, &at));
}

/* Deliver a hand-made ACK frame to node A. */
static void inject_ack(uint16_t src, uint8_t acked_seq)
{
    kv_frame_t f;
    uint8_t buf[16];
    size_t len;

    memset(&f, 0, sizeof(f));
    f.hdr.type = KV_FRAME_ACK;
    f.hdr.net_id = NET;
    f.hdr.dst = ADDR_A;
    f.hdr.src = src;
    f.hdr.seq = 0x50;
    f.payload = &acked_seq;
    f.payload_len = 1;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&f, buf, sizeof(buf), &len));
    kv_node_on_rx(&node_a, buf, len, NULL);
}

static void test_ack_must_match_peer_and_seq(void)
{
    uint8_t seq = 0;

    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, &seq);
    kv_node_on_tx_done(&node_a);

    inject_ack(0x0003u, seq); /* wrong peer */
    clock_ms += KV_DEDUP_WINDOW_MS; /* let the reused frame seq pass dedup */
    inject_ack(ADDR_B, (uint8_t)(seq + 1u)); /* wrong seq */
    TEST_ASSERT_EQUAL_size_t(0, ack_count);
}

static void test_busy_while_transmitting(void)
{
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY,
                          kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL));
    kv_node_on_tx_done(&node_a);
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL));
}

static void test_only_one_ack_outstanding(void)
{
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL);
    kv_node_on_tx_done(&node_a);
    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY,
                          kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL));
    /* without ack_req it is fine */
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), false, NULL));
}

static void test_send_rejects_bad_requests(void)
{
    uint8_t big[MTU];

    memset(big, 0, sizeof(big));
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE,
                          kv_node_send(&node_a, ADDR_B, big, MTU - KV_FRAME_HEADER_LEN + 1u,
                                       false, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED,
                          kv_node_send(&node_a, KV_ADDR_BROADCAST, hello, sizeof(hello), true,
                                       NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_send(&node_a, ADDR_B, NULL, 1, false, NULL));
    TEST_ASSERT_EQUAL_size_t(0, radio_a.sent_count);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_send(&node_a, ADDR_B, big, MTU - KV_FRAME_HEADER_LEN,
                                              false, NULL));
    TEST_ASSERT_EQUAL_size_t(MTU, radio_a.sent_len[0]);
}

static void test_ack_dropped_while_radio_busy(void)
{
    uint32_t at;

    kv_node_send(&node_b, KV_ADDR_BROADCAST, hello, sizeof(hello), false, NULL); /* B busy */
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL);
    air(&radio_a, &node_a, 0, &node_b);

    TEST_ASSERT_EQUAL_size_t(1, data_count);
    TEST_ASSERT_EQUAL_size_t(1, radio_b.sent_count); /* only the broadcast, no ACK */
    TEST_ASSERT_EQUAL_size_t(1, radio_b.log_count);
    TEST_ASSERT_EQUAL_INT(KV_LOG_WARN, radio_b.last_log_level);

    /* A's retry recovers it */
    kv_node_on_tx_done(&node_b);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
    clock_ms = at;
    kv_node_tick(&node_a);
    air(&radio_a, &node_a, 1, &node_b);
    air(&radio_b, &node_b, 1, &node_a);
    TEST_ASSERT_EQUAL_INT(KV_OK, ack_result);
    TEST_ASSERT_EQUAL_size_t(1, data_count);
}

static void test_retry_waits_for_busy_radio(void)
{
    uint32_t at;

    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL);
    kv_node_on_tx_done(&node_a);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
    kv_node_send(&node_a, KV_ADDR_BROADCAST, hello, sizeof(hello), false, NULL); /* busy */
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, &at)); /* tx_done will drive it */

    clock_ms = at + 10u;
    kv_node_tick(&node_a);
    TEST_ASSERT_EQUAL_size_t(2, radio_a.sent_count); /* deferred */
    kv_node_on_tx_done(&node_a);
    TEST_ASSERT_EQUAL_size_t(3, radio_a.sent_count); /* retried right away */
    TEST_ASSERT_EQUAL_MEMORY(radio_a.sent[0], radio_a.sent[2], radio_a.sent_len[0]);
}

static void test_radio_send_failure(void)
{
    uint32_t at;

    radio_a.fail_send = true;
    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY,
                          kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL));
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, &at)); /* nothing pending */

    /* failure during a retry counts as an attempt and waits another timeout */
    radio_a.fail_send = false;
    kv_node_send(&node_a, ADDR_B, hello, sizeof(hello), true, NULL);
    kv_node_on_tx_done(&node_a);
    kv_node_next_deadline(&node_a, &at);
    clock_ms = at;
    radio_a.fail_send = true;
    kv_node_tick(&node_a);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node_a, &at));
    TEST_ASSERT_EQUAL_UINT32(clock_ms + TIMEOUT, at);
}

static void test_unsupported_frames_are_dropped(void)
{
    kv_frame_t f;
    uint8_t buf[16];
    size_t len;

    memset(&f, 0, sizeof(f));
    f.hdr.net_id = NET;
    f.hdr.dst = ADDR_B;
    f.hdr.src = ADDR_A;
    f.payload = hello;
    f.payload_len = sizeof(hello);

    f.hdr.type = KV_FRAME_DATA; /* fragmented: phase 3 */
    f.hdr.frag = true;
    f.hdr.frag_last = 1;
    kv_frame_encode(&f, buf, sizeof(buf), &len);
    kv_node_on_rx(&node_b, buf, len, NULL);
    TEST_ASSERT_EQUAL_size_t(1, radio_b.log_count);

    f.hdr.frag = false; /* unknown type */
    f.hdr.type = 0x7;
    f.hdr.seq = 1;
    kv_frame_encode(&f, buf, sizeof(buf), &len);
    kv_node_on_rx(&node_b, buf, len, NULL);

    kv_node_on_rx(&node_b, buf, 3, NULL); /* garbage */
    TEST_ASSERT_EQUAL_size_t(0, data_count);
    TEST_ASSERT_EQUAL_size_t(0, radio_b.sent_count);
}

static void test_null_arguments_are_safe(void)
{
    uint32_t at;

    kv_node_config_default(NULL);
    kv_node_on_rx(NULL, hello, sizeof(hello), NULL);
    kv_node_on_tx_done(NULL);
    kv_node_tick(NULL);
    TEST_ASSERT_FALSE(kv_node_next_deadline(NULL, &at));
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node_a, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_send(NULL, ADDR_B, hello, 1, false, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&node_a, NULL, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_send_builds_frame_with_random_start_seq);
    RUN_TEST(test_data_is_delivered);
    RUN_TEST(test_broadcast_is_delivered);
    RUN_TEST(test_foreign_frames_are_dropped);
    RUN_TEST(test_duplicate_is_delivered_once);
    RUN_TEST(test_duplicate_window_expires);
    RUN_TEST(test_ack_round_trip);
    RUN_TEST(test_lost_ack_triggers_retransmit_and_reack);
    RUN_TEST(test_ack_times_out_after_retries);
    RUN_TEST(test_ack_must_match_peer_and_seq);
    RUN_TEST(test_busy_while_transmitting);
    RUN_TEST(test_only_one_ack_outstanding);
    RUN_TEST(test_send_rejects_bad_requests);
    RUN_TEST(test_ack_dropped_while_radio_busy);
    RUN_TEST(test_retry_waits_for_busy_radio);
    RUN_TEST(test_radio_send_failure);
    RUN_TEST(test_unsupported_frames_are_dropped);
    RUN_TEST(test_null_arguments_are_safe);
    return UNITY_END();
}

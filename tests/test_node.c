/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/node.h"
#include "kervan/topic.h"
#include "unity.h"

#include <string.h>

/* ---- mock PAL: records transmissions, time is driven by the test ---- */

#define MOCK_MAX_FRAMES 16

typedef struct {
    uint32_t random_value;
    bool fail_send;
    size_t sent_count;
    uint8_t sent[MOCK_MAX_FRAMES][KV_MTU_MAX];
    size_t sent_len[MOCK_MAX_FRAMES];
    size_t log_count;
} mock_radio_t;

static uint32_t clock_ms;

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

static void mock_log(void *ctx, kv_log_level_t level, const char *msg)
{
    (void)level;
    (void)msg;
    ((mock_radio_t *)ctx)->log_count++;
}

/* ---- recorded callbacks (per node, via user pointer) ---- */

typedef struct {
    size_t msg_count;
    kv_message_t last;
    uint8_t data[KV_MAX_MSG_SIZE];
    size_t ack_count;
    uint8_t ack_seq;
    kv_status_t ack_result;
} events_t;

static void on_message(void *user, const kv_message_t *msg)
{
    events_t *ev = (events_t *)user;
    ev->msg_count++;
    ev->last = *msg;
    memcpy(ev->data, msg->data, msg->len);
}

static void on_ack(void *user, uint8_t seq, kv_status_t result)
{
    events_t *ev = (events_t *)user;
    ev->ack_count++;
    ev->ack_seq = seq;
    ev->ack_result = result;
}

/* ---- nodes A (1), B (2), C (3), D (4); net 7; LoRa-sized MTU ---- */

#define NET 7u
#define MTU 51u
#define TIMEOUT 1000u
#define TOPIC 0x1234u

static mock_radio_t radio[4];
static kv_node_t node[4];
static events_t ev[4];
#define A 0
#define B 1
#define C 2
#define D 3

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

static kv_node_config_t make_cfg(int i)
{
    kv_node_config_t cfg;
    kv_node_config_default(&cfg);
    cfg.addr = (uint16_t)(i + 1);
    cfg.net_id = NET;
    cfg.ack_timeout_ms = TIMEOUT;
    cfg.max_retries = 2;
    cfg.duty_cycle_permille = 0; /* logic tests; the budget has its own tests */
    cfg.relay = false;           /* mesh tests switch it on */
    cfg.relay_window_ms = 100;
    cfg.on_message = on_message;
    cfg.on_ack = on_ack;
    cfg.user = &ev[i];
    return cfg;
}

static void init_node(int i, const kv_node_config_t *cfg)
{
    kv_pal_t pal = make_pal(&radio[i]);
    memset(&radio[i], 0, sizeof(radio[i]));
    radio[i].random_value = 0x12345678u + (uint32_t)i * 0x51u;
    memset(&ev[i], 0, sizeof(ev[i]));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_init(&node[i], &pal, cfg));
}

/* Finish frame k of node `from` and hand it to node `to` (to < 0: nobody). */
static void air(int from, size_t k, int to)
{
    TEST_ASSERT_TRUE(k < radio[from].sent_count);
    kv_node_on_tx_done(&node[from]);
    if (to >= 0) {
        kv_node_on_rx(&node[to], radio[from].sent[k], radio[from].sent_len[k], NULL);
    }
}

static void hear(int to, int from, size_t k)
{
    kv_node_on_rx(&node[to], radio[from].sent[k], radio[from].sent_len[k], NULL);
}

static kv_frame_t decode(int from, size_t k)
{
    kv_frame_t f;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(radio[from].sent[k], radio[from].sent_len[k], &f));
    return f;
}

/* Advance to the node's next deadline and tick it. */
static void wait_for(int i)
{
    uint32_t at;
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node[i], &at));
    TEST_ASSERT_TRUE((int32_t)(at - clock_ms) >= 0);
    clock_ms = at;
    kv_node_tick(&node[i]);
}

void setUp(void)
{
    int i;
    clock_ms = 1000;
    for (i = 0; i < 4; i++) {
        kv_node_config_t cfg = make_cfg(i);
        init_node(i, &cfg);
    }
}

void tearDown(void) {}

static const uint8_t hello[] = {'h', 'i'};

/* ---- basics ---- */

static void test_init_rejects_bad_config(void)
{
    kv_pal_t pal = make_pal(&radio[A]);
    kv_node_config_t cfg = make_cfg(A);
    kv_node_t n;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_init(&n, &pal, &cfg));
    cfg.addr = KV_ADDR_BROADCAST;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    cfg = make_cfg(A);
    cfg.hop_limit = 8;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    cfg = make_cfg(A);
    pal.mtu = KV_FRAME_HEADER_LEN + KV_FRAME_FRAG_HEADER_LEN + 1u;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    pal.mtu = KV_MTU_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    pal.mtu = MTU;
    pal.random = NULL;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, &pal, &cfg));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_init(&n, NULL, &cfg));
}

static void test_topic_ids(void)
{
    TEST_ASSERT_EQUAL_HEX16(kv_topic_id("sensors/temp"), kv_topic_id("sensors/temp"));
    TEST_ASSERT_NOT_EQUAL(kv_topic_id("sensors/temp"), kv_topic_id("sensors/hum"));
    /* pinned from the FNV-1a test vectors: "" = 0x811C9DC5, "a" = 0xE40C292C, folded */
    TEST_ASSERT_EQUAL_HEX16(0x811Cu ^ 0x9DC5u, kv_topic_id(""));
    TEST_ASSERT_EQUAL_HEX16(0xE40Cu ^ 0x292Cu, kv_topic_id("a"));
    TEST_ASSERT_EQUAL_HEX16(0, kv_topic_id(NULL));
}

static void test_publish_builds_data_frame(void)
{
    kv_frame_t f;
    uint8_t seq = 0;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, &seq));
    TEST_ASSERT_EQUAL_UINT8(0x78, seq); /* random start */
    TEST_ASSERT_EQUAL_size_t(1, radio[A].sent_count);
    f = decode(A, 0);
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_DATA, f.hdr.type);
    TEST_ASSERT_FALSE(f.hdr.ack_req);
    TEST_ASSERT_FALSE(f.hdr.frag);
    TEST_ASSERT_EQUAL_HEX16(1, f.hdr.src);
    TEST_ASSERT_EQUAL_HEX16(2, f.hdr.dst);
    TEST_ASSERT_EQUAL_UINT8(KV_HOP_LIMIT_DEFAULT, f.hdr.hop_limit);
    TEST_ASSERT_EQUAL_size_t(4, f.payload_len);
    TEST_ASSERT_EQUAL_HEX8(0x34, f.payload[0]); /* topic, little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x12, f.payload[1]);
    TEST_ASSERT_EQUAL_MEMORY(hello, &f.payload[2], 2);
}

static void test_publish_rejects_bad_requests(void)
{
    uint8_t big[KV_MAX_MSG_SIZE];
    memset(big, 0, sizeof(big));

    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED,
                          kv_node_publish(&node[A], KV_ADDR_BROADCAST, TOPIC, hello, 2, KV_QOS1, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_publish(&node[A], 2, TOPIC, NULL, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_publish(&node[A], 2, TOPIC, hello, 2, 2, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_publish(&node[A], 1, TOPIC, hello, 2, 0, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_node_publish(&node[A], 2, TOPIC, big,
                                                           KV_MAX_MSG_SIZE - 1u, KV_QOS0, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_publish(NULL, 2, TOPIC, hello, 2, 0, NULL));
    TEST_ASSERT_EQUAL_size_t(0, radio[A].sent_count);
}

static void test_unicast_is_delivered_without_subscription(void)
{
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_HEX16(1, ev[B].last.src);
    TEST_ASSERT_EQUAL_HEX16(2, ev[B].last.dst);
    TEST_ASSERT_EQUAL_HEX16(TOPIC, ev[B].last.topic);
    TEST_ASSERT_EQUAL_UINT8(0, ev[B].last.hops);
    TEST_ASSERT_EQUAL_size_t(2, ev[B].last.len);
    TEST_ASSERT_EQUAL_MEMORY(hello, ev[B].data, 2);
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->delivered);
}

static void test_broadcast_needs_subscription(void)
{
    kv_node_publish(&node[A], KV_ADDR_BROADCAST, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(0, ev[B].msg_count);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_subscribe(&node[B], TOPIC));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_subscribe(&node[B], TOPIC)); /* idempotent */
    kv_node_publish(&node[A], KV_ADDR_BROADCAST, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 1, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_unsubscribe(&node[B], TOPIC));
    kv_node_publish(&node[A], KV_ADDR_BROADCAST, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 2, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
}

static void test_subscription_table_is_bounded(void)
{
    uint16_t t;
    for (t = 0; t < KV_MAX_SUBSCRIPTIONS; t++) {
        TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_subscribe(&node[B], t));
    }
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_node_subscribe(&node[B], 0xFFF0u));
}

static void test_foreign_and_own_frames_are_ignored(void)
{
    kv_node_config_t cfg = make_cfg(C);
    cfg.net_id = NET + 1u;
    init_node(C, &cfg);
    kv_node_publish(&node[C], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    air(C, 0, B); /* other network */
    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B); /* for someone else, B does not relay */
    hear(A, A, 0); /* our own frame */
    TEST_ASSERT_EQUAL_size_t(0, ev[A].msg_count + ev[B].msg_count);
    TEST_ASSERT_EQUAL_size_t(0, radio[B].sent_count);
}

static void test_duplicates_are_delivered_once(void)
{
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    hear(B, A, 0);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->rx_duplicates);
    clock_ms += KV_DEDUP_WINDOW_MS; /* the window expires */
    hear(B, A, 0);
    TEST_ASSERT_EQUAL_size_t(2, ev[B].msg_count);
}

static void test_qos0_messages_go_out_back_to_back(void)
{
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    TEST_ASSERT_EQUAL_size_t(1, radio[A].sent_count); /* radio busy */
    kv_node_on_tx_done(&node[A]);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count);
    kv_node_on_tx_done(&node[A]);
    TEST_ASSERT_EQUAL_UINT8(node[A].txq_count, 0);
    TEST_ASSERT_EQUAL_size_t(0, ev[A].ack_count); /* QoS 0 reports nothing */
}

/* ---- QoS 1 ---- */

static void test_qos1_round_trip(void)
{
    uint8_t seq = 0;
    uint32_t at;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, &seq));
    TEST_ASSERT_TRUE(decode(A, 0).hdr.ack_req);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node[A], &at)); /* transmitting */
    air(A, 0, B);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node[A], &at));
    TEST_ASSERT_TRUE(at >= clock_ms + TIMEOUT && at <= clock_ms + TIMEOUT + TIMEOUT / 2u);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count);
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_ACK, decode(B, 0).hdr.type);

    air(B, 0, A);
    TEST_ASSERT_EQUAL_size_t(1, ev[A].ack_count);
    TEST_ASSERT_EQUAL_UINT8(seq, ev[A].ack_seq);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev[A].ack_result);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node[A], &at));
}

static void test_qos1_lost_ack_retransmits_and_reacks(void)
{
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, B);
    kv_node_on_tx_done(&node[B]); /* B's ACK is lost */

    wait_for(A);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count);
    TEST_ASSERT_EQUAL_MEMORY(radio[A].sent[0], radio[A].sent[1], radio[A].sent_len[0]);
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[A])->retransmissions);

    air(A, 1, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);       /* not delivered twice */
    TEST_ASSERT_EQUAL_size_t(2, radio[B].sent_count);   /* but acknowledged again */
    air(B, 1, A);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev[A].ack_result);
}

static void test_qos1_times_out_with_backoff(void)
{
    uint32_t sent_at[3];
    int i;

    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL);
    for (i = 0; i < 3; i++) {
        sent_at[i] = clock_ms;
        kv_node_on_tx_done(&node[A]);
        wait_for(A);
    }
    TEST_ASSERT_EQUAL_size_t(3, radio[A].sent_count); /* first attempt + 2 retries */
    TEST_ASSERT_EQUAL_size_t(1, ev[A].ack_count);
    TEST_ASSERT_EQUAL_INT(KV_ERR_TIMEOUT, ev[A].ack_result);
    /* the wait doubles: >= 1 s, then >= 2 s */
    TEST_ASSERT_TRUE(sent_at[1] - sent_at[0] >= TIMEOUT);
    TEST_ASSERT_TRUE(sent_at[2] - sent_at[1] >= 2u * TIMEOUT);
}

static void test_qos1_messages_are_sent_one_after_another(void)
{
    uint8_t first = 0;
    uint8_t second = 0;

    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, &first);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, &second);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(1, radio[A].sent_count); /* waits for the first ACK */
    air(B, 0, A);
    TEST_ASSERT_EQUAL_UINT8(first, ev[A].ack_seq);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count);
    TEST_ASSERT_EQUAL_UINT8(second, decode(A, 1).hdr.seq);
}

static void test_queue_is_bounded(void)
{
    size_t i;
    for (i = 0; i < KV_TX_QUEUE; i++) {
        TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL));
    }
    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY, kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL));
}

static void test_ack_from_wrong_peer_or_seq_is_ignored(void)
{
    kv_frame_t f;
    uint8_t buf[16];
    size_t len;
    uint8_t seq = 0;
    uint8_t wrong;

    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, &seq);
    kv_node_on_tx_done(&node[A]);
    memset(&f, 0, sizeof(f));
    f.hdr.type = KV_FRAME_ACK;
    f.hdr.net_id = NET;
    f.hdr.dst = 1;
    f.hdr.src = 3; /* wrong peer */
    f.hdr.seq = 0x50;
    f.payload = &seq;
    f.payload_len = 1;
    kv_frame_encode(&f, buf, sizeof(buf), &len);
    kv_node_on_rx(&node[A], buf, len, NULL);
    f.hdr.src = 2;
    f.hdr.seq = 0x51;
    wrong = (uint8_t)(seq + 1u);
    f.payload = &wrong;
    kv_frame_encode(&f, buf, sizeof(buf), &len);
    kv_node_on_rx(&node[A], buf, len, NULL);
    TEST_ASSERT_EQUAL_size_t(0, ev[A].ack_count);
}

/* ---- fragmentation (MTU 51: 42 payload bytes per fragment) ---- */

static uint8_t pattern[200];

static void make_pattern(void)
{
    size_t i;
    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 7u + 1u);
    }
}

static void test_large_message_is_fragmented_and_reassembled(void)
{
    kv_frame_t f;
    make_pattern();

    /* 2 + 100 bytes -> 42 + 42 + 18 */
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, pattern, 100, KV_QOS0, NULL));
    kv_node_on_tx_done(&node[A]);
    kv_node_on_tx_done(&node[A]);
    TEST_ASSERT_EQUAL_size_t(3, radio[A].sent_count);
    f = decode(A, 2);
    TEST_ASSERT_TRUE(f.hdr.frag);
    TEST_ASSERT_EQUAL_UINT8(2, f.hdr.frag_index);
    TEST_ASSERT_EQUAL_UINT8(2, f.hdr.frag_last);
    TEST_ASSERT_EQUAL_size_t(18, f.payload_len);
    TEST_ASSERT_EQUAL_UINT8(decode(A, 0).hdr.seq, f.hdr.seq); /* one seq per message */

    /* out of order */
    hear(B, A, 2);
    hear(B, A, 0);
    TEST_ASSERT_EQUAL_size_t(0, ev[B].msg_count);
    hear(B, A, 1);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_size_t(100, ev[B].last.len);
    TEST_ASSERT_EQUAL_MEMORY(pattern, ev[B].data, 100);
}

static void test_missing_fragment_times_out(void)
{
    make_pattern();
    kv_node_publish(&node[A], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_on_tx_done(&node[A]);
    kv_node_on_tx_done(&node[A]);
    hear(B, A, 0);
    hear(B, A, 2); /* fragment 1 lost */
    TEST_ASSERT_EQUAL_size_t(0, ev[B].msg_count);

    /* after the timeout the slot is reclaimed for new messages */
    clock_ms += KV_REASSEMBLY_TIMEOUT_MS;
    kv_node_publish(&node[C], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_on_tx_done(&node[C]);
    kv_node_on_tx_done(&node[C]);
    kv_node_publish(&node[D], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_on_tx_done(&node[D]);
    kv_node_on_tx_done(&node[D]);
    hear(B, C, 0);
    hear(B, D, 0); /* both slots: one free, one expired */
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->reassembly_dropped);
}

static void test_reassembly_slots_are_never_evicted_while_live(void)
{
    make_pattern();
    kv_node_publish(&node[A], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_publish(&node[C], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_publish(&node[D], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    hear(B, A, 0);
    hear(B, C, 0);
    hear(B, D, 0); /* KV_REASSEMBLY_SLOTS == 2 */
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->reassembly_dropped);
}

static void test_fragmented_qos1_is_acked_once_complete(void)
{
    make_pattern();
    kv_node_publish(&node[A], 2, TOPIC, pattern, 100, KV_QOS1, NULL);
    air(A, 0, B);
    air(A, 1, B);
    TEST_ASSERT_EQUAL_size_t(0, radio[B].sent_count);
    air(A, 2, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count);
    air(B, 0, A);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev[A].ack_result);
}

static void test_too_many_fragments_are_refused(void)
{
    kv_pal_t pal = make_pal(&radio[A]);
    kv_node_config_t cfg = make_cfg(A);
    pal.mtu = 20; /* 11 bytes per fragment: 16 fragments carry at most 176 bytes */
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_init(&node[A], &pal, &cfg));
    make_pattern();
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, pattern, 174, KV_QOS0, NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE,
                          kv_node_publish(&node[A], 2, TOPIC, pattern, 175, KV_QOS0, NULL));
}

/* ---- duty-cycle budget ---- */

static void test_duty_cycle_enforces_time_off(void)
{
    kv_node_config_t cfg = make_cfg(A);
    uint32_t at;
    cfg.duty_cycle_permille = 10; /* 1 %: 100 ms on air -> 9.9 s off */
    init_node(A, &cfg);

    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    clock_ms += 100;
    kv_node_on_tx_done(&node[A]);
    TEST_ASSERT_EQUAL_UINT32(100, kv_node_stats(&node[A])->tx_airtime_ms);

    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    TEST_ASSERT_EQUAL_size_t(1, radio[A].sent_count); /* must wait */
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[A])->budget_waits);
    TEST_ASSERT_TRUE(kv_node_next_deadline(&node[A], &at));
    TEST_ASSERT_EQUAL_UINT32(clock_ms + 9900u, at);
    clock_ms = at - 1u;
    kv_node_tick(&node[A]);
    TEST_ASSERT_EQUAL_size_t(1, radio[A].sent_count);
    clock_ms = at;
    kv_node_tick(&node[A]);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count);
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[A])->budget_waits);
}

static void test_duty_burst_allows_credit(void)
{
    kv_node_config_t cfg = make_cfg(A);
    cfg.duty_cycle_permille = 10;
    cfg.duty_burst_ms = 100; /* idle time earns up to 100 ms of airtime */
    init_node(A, &cfg);

    clock_ms += 20000; /* earns 200 ms, capped at 100 */
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    clock_ms += 60;
    kv_node_on_tx_done(&node[A]);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count); /* 40 ms of credit left */
    clock_ms += 60;
    kv_node_on_tx_done(&node[A]);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    TEST_ASSERT_EQUAL_size_t(2, radio[A].sent_count); /* now in debt */
}

static void test_acks_may_overdraw_the_budget(void)
{
    kv_node_config_t cfg = make_cfg(B);
    cfg.duty_cycle_permille = 10;
    init_node(B, &cfg);

    kv_node_publish(&node[B], 1, TOPIC, hello, 2, KV_QOS0, NULL);
    clock_ms += 100;
    kv_node_on_tx_done(&node[B]); /* B is in time-off now */
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(2, radio[B].sent_count);
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_ACK, decode(B, 1).hdr.type);
}

static void test_ack_debt_is_bounded(void)
{
    kv_node_config_t cfg = make_cfg(B);
    cfg.duty_cycle_permille = 10;
    cfg.ack_debt_ms = 500;
    init_node(B, &cfg);

    kv_node_publish(&node[B], 1, TOPIC, hello, 2, KV_QOS0, NULL);
    clock_ms += 600; /* 600 ms on air: deeper in debt than ACKs may go */
    kv_node_on_tx_done(&node[B]);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count); /* no ACK */
}

/* Fragments paced by a duty cycle arrive far apart; the timeout counts from the latest one. */
static void test_reassembly_timeout_restarts_with_each_fragment(void)
{
    make_pattern();
    kv_node_publish(&node[A], 2, TOPIC, pattern, 100, KV_QOS0, NULL);
    kv_node_on_tx_done(&node[A]);
    kv_node_on_tx_done(&node[A]);
    hear(B, A, 0);
    clock_ms += KV_REASSEMBLY_TIMEOUT_MS - 1u;
    hear(B, A, 1);
    clock_ms += KV_REASSEMBLY_TIMEOUT_MS - 1u;
    hear(B, A, 2);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    TEST_ASSERT_EQUAL_MEMORY(pattern, ev[B].data, 100);
}

/* ---- mesh: managed flooding ---- */

static void enable_relay(int i)
{
    kv_node_config_t cfg = make_cfg(i);
    cfg.relay = true;
    init_node(i, &cfg);
}

/* A -- B -- C in a line: A and C cannot hear each other. */
static void test_unicast_is_relayed_and_acked_over_two_hops(void)
{
    kv_frame_t f;

    enable_relay(B);
    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(0, radio[B].sent_count); /* waits for its relay slot */
    wait_for(B);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count);
    f = decode(B, 0);
    TEST_ASSERT_EQUAL_HEX16(1, f.hdr.src); /* original sender and seq are kept */
    TEST_ASSERT_EQUAL_UINT8(decode(A, 0).hdr.seq, f.hdr.seq);
    TEST_ASSERT_EQUAL_UINT8(KV_HOP_LIMIT_DEFAULT - 1u, f.hdr.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(1, f.hdr.hop_count);

    air(B, 0, C);
    TEST_ASSERT_EQUAL_size_t(1, ev[C].msg_count);
    TEST_ASSERT_EQUAL_UINT8(1, ev[C].last.hops);
    TEST_ASSERT_EQUAL_size_t(1, radio[C].sent_count); /* ACK to A */

    air(C, 0, B); /* the ACK floods back the same way */
    wait_for(B);
    air(B, 1, A);
    TEST_ASSERT_EQUAL_size_t(1, ev[A].ack_count);
    TEST_ASSERT_EQUAL_INT(KV_OK, ev[A].ack_result);
    TEST_ASSERT_EQUAL_UINT32(2, kv_node_stats(&node[B])->relayed);
}

static void test_destination_does_not_relay(void)
{
    uint32_t at;
    enable_relay(B);
    kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node[B], &at));
}

static void test_broadcast_is_delivered_and_relayed(void)
{
    enable_relay(B);
    kv_node_subscribe(&node[B], TOPIC);
    kv_node_publish(&node[A], KV_ADDR_BROADCAST, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    TEST_ASSERT_EQUAL_size_t(1, ev[B].msg_count);
    wait_for(B);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count);
}

static void test_hop_limit_zero_is_not_relayed(void)
{
    kv_node_config_t cfg = make_cfg(A);
    uint32_t at;
    cfg.hop_limit = 0;
    init_node(A, &cfg);
    enable_relay(B);
    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node[B], &at));
}

static void test_relay_is_cancelled_when_someone_else_relays_first(void)
{
    enable_relay(B);
    enable_relay(D);
    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    hear(D, A, 0);
    wait_for(D);                 /* D relays first ... */
    TEST_ASSERT_EQUAL_size_t(1, radio[D].sent_count);
    air(D, 0, B);                /* ... B hears it and stays quiet */
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->relay_cancelled);
    clock_ms += 10000;
    kv_node_tick(&node[B]);
    TEST_ASSERT_EQUAL_size_t(0, radio[B].sent_count);
}

static void test_weaker_signal_relays_sooner(void)
{
    kv_rx_meta_t far = {-120, -18};
    kv_rx_meta_t near = {-60, 9};
    uint32_t far_at;
    uint32_t near_at;

    enable_relay(B);
    enable_relay(D);
    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS0, NULL);
    kv_node_on_tx_done(&node[A]);
    kv_node_on_rx(&node[B], radio[A].sent[0], radio[A].sent_len[0], &far);
    kv_node_on_rx(&node[D], radio[A].sent[0], radio[A].sent_len[0], &near);
    kv_node_next_deadline(&node[B], &far_at);
    kv_node_next_deadline(&node[D], &near_at);
    TEST_ASSERT_TRUE((int32_t)(near_at - far_at) > 0);
}

static void test_relay_without_budget_is_dropped(void)
{
    kv_node_config_t cfg = make_cfg(B);
    cfg.relay = true;
    cfg.duty_cycle_permille = 10;
    init_node(B, &cfg);
    kv_node_publish(&node[B], 1, TOPIC, hello, 2, KV_QOS0, NULL);
    clock_ms += 500;
    kv_node_on_tx_done(&node[B]); /* 49.5 s of time-off */

    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS0, NULL);
    air(A, 0, B);
    wait_for(B);
    TEST_ASSERT_EQUAL_size_t(1, radio[B].sent_count); /* only its own frame */
    TEST_ASSERT_EQUAL_UINT32(1, kv_node_stats(&node[B])->relay_dropped);
}

/* A relay that just spent its budget on the data may still carry the ACK back. */
static void test_relayed_ack_may_overdraw(void)
{
    kv_node_config_t cfg = make_cfg(B);
    cfg.relay = true;
    cfg.duty_cycle_permille = 10;
    init_node(B, &cfg);

    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, B);
    wait_for(B);
    clock_ms += 50;
    air(B, 0, C); /* B relayed the data: now in time-off */
    air(C, 0, B); /* C's ACK */
    wait_for(B);
    TEST_ASSERT_EQUAL_size_t(2, radio[B].sent_count);
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_ACK, decode(B, 1).hdr.type);
    TEST_ASSERT_EQUAL_UINT32(0, kv_node_stats(&node[B])->relay_dropped);
}

static void test_relayed_copies_do_not_trigger_extra_acks(void)
{
    kv_frame_t relayed;
    uint8_t buf[KV_MTU_MAX];
    size_t len;

    kv_node_publish(&node[A], 3, TOPIC, hello, 2, KV_QOS1, NULL);
    air(A, 0, C);
    TEST_ASSERT_EQUAL_size_t(1, radio[C].sent_count); /* ACK */
    kv_node_on_tx_done(&node[C]);

    relayed = decode(A, 0); /* the same message, relayed by a neighbour */
    relayed.hdr.hop_limit--;
    relayed.hdr.hop_count++;
    kv_frame_encode(&relayed, buf, sizeof(buf), &len);
    clock_ms += 300;
    kv_node_on_rx(&node[C], buf, len, NULL);
    TEST_ASSERT_EQUAL_size_t(1, radio[C].sent_count); /* no second ACK */

    clock_ms += 1000; /* a real retransmission comes after the ACK timeout */
    hear(C, A, 0);
    TEST_ASSERT_EQUAL_size_t(2, radio[C].sent_count);
    TEST_ASSERT_EQUAL_size_t(1, ev[C].msg_count);
}

/* ---- robustness ---- */

static void test_radio_failure_drops_the_message(void)
{
    radio[A].fail_send = true;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_publish(&node[A], 2, TOPIC, hello, 2, KV_QOS1, NULL));
    TEST_ASSERT_EQUAL_size_t(1, ev[A].ack_count);
    TEST_ASSERT_EQUAL_INT(KV_ERR_BUSY, ev[A].ack_result);
    TEST_ASSERT_EQUAL_UINT8(0, node[A].txq_count);
    TEST_ASSERT_TRUE(radio[A].log_count > 0u);
}

static void test_null_arguments_are_safe(void)
{
    uint32_t at;
    kv_node_config_default(NULL);
    kv_node_on_rx(NULL, hello, 2, NULL);
    kv_node_on_tx_done(NULL);
    kv_node_on_tx_done(&node[A]); /* nothing in flight */
    kv_node_tick(NULL);
    TEST_ASSERT_FALSE(kv_node_next_deadline(NULL, &at));
    TEST_ASSERT_FALSE(kv_node_next_deadline(&node[A], NULL));
    TEST_ASSERT_NULL(kv_node_stats(NULL));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_subscribe(NULL, 1));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_node_unsubscribe(NULL, 1));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_node_unsubscribe(&node[A], 99)); /* not subscribed */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_topic_ids);
    RUN_TEST(test_publish_builds_data_frame);
    RUN_TEST(test_publish_rejects_bad_requests);
    RUN_TEST(test_unicast_is_delivered_without_subscription);
    RUN_TEST(test_broadcast_needs_subscription);
    RUN_TEST(test_subscription_table_is_bounded);
    RUN_TEST(test_foreign_and_own_frames_are_ignored);
    RUN_TEST(test_duplicates_are_delivered_once);
    RUN_TEST(test_qos0_messages_go_out_back_to_back);
    RUN_TEST(test_qos1_round_trip);
    RUN_TEST(test_qos1_lost_ack_retransmits_and_reacks);
    RUN_TEST(test_qos1_times_out_with_backoff);
    RUN_TEST(test_qos1_messages_are_sent_one_after_another);
    RUN_TEST(test_queue_is_bounded);
    RUN_TEST(test_ack_from_wrong_peer_or_seq_is_ignored);
    RUN_TEST(test_large_message_is_fragmented_and_reassembled);
    RUN_TEST(test_missing_fragment_times_out);
    RUN_TEST(test_reassembly_slots_are_never_evicted_while_live);
    RUN_TEST(test_fragmented_qos1_is_acked_once_complete);
    RUN_TEST(test_too_many_fragments_are_refused);
    RUN_TEST(test_duty_cycle_enforces_time_off);
    RUN_TEST(test_duty_burst_allows_credit);
    RUN_TEST(test_acks_may_overdraw_the_budget);
    RUN_TEST(test_ack_debt_is_bounded);
    RUN_TEST(test_reassembly_timeout_restarts_with_each_fragment);
    RUN_TEST(test_unicast_is_relayed_and_acked_over_two_hops);
    RUN_TEST(test_destination_does_not_relay);
    RUN_TEST(test_broadcast_is_delivered_and_relayed);
    RUN_TEST(test_hop_limit_zero_is_not_relayed);
    RUN_TEST(test_relay_is_cancelled_when_someone_else_relays_first);
    RUN_TEST(test_weaker_signal_relays_sooner);
    RUN_TEST(test_relay_without_budget_is_dropped);
    RUN_TEST(test_relayed_ack_may_overdraw);
    RUN_TEST(test_relayed_copies_do_not_trigger_extra_acks);
    RUN_TEST(test_radio_failure_drops_the_message);
    RUN_TEST(test_null_arguments_are_safe);
    return UNITY_END();
}

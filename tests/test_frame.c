/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/frame.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const uint8_t payload2[] = {0xDE, 0xAD};

/* DATA, ack_req, hop 3/0, net 0x2A, 0xABCD -> 0x1234, seq 5, payload DE AD. */
static const uint8_t golden_data[] = {0x02, 0x60, 0x2A, 0x34, 0x12, 0xCD, 0xAB, 0x05, 0xDE, 0xAD};

/* CTRL, fragment 2 of 0..3, hop 2/1, net 0x01, 0x0002 -> broadcast, seq 0xFF, payload 0x7E. */
static const uint8_t golden_frag[] = {0x09, 0x44, 0x01, 0xFF, 0xFF, 0x02, 0x00, 0xFF, 0x23, 0x7E};

static kv_frame_t data_frame(void)
{
    kv_frame_t f;
    memset(&f, 0, sizeof(f));
    f.hdr.type = KV_FRAME_DATA;
    f.hdr.ack_req = true;
    f.hdr.hop_limit = KV_HOP_LIMIT_DEFAULT;
    f.hdr.net_id = 0x2A;
    f.hdr.dst = 0x1234;
    f.hdr.src = 0xABCD;
    f.hdr.seq = 5;
    f.payload = payload2;
    f.payload_len = sizeof(payload2);
    return f;
}

static void test_encode_matches_golden_bytes(void)
{
    kv_frame_t f = data_frame();
    uint8_t buf[32];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&f, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(golden_data), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_data, buf, len);
}

static void test_decode_golden_bytes(void)
{
    kv_frame_t f;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(golden_data, sizeof(golden_data), &f));
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_DATA, f.hdr.type);
    TEST_ASSERT_TRUE(f.hdr.ack_req);
    TEST_ASSERT_FALSE(f.hdr.frag);
    TEST_ASSERT_FALSE(f.hdr.sec);
    TEST_ASSERT_EQUAL_UINT8(3, f.hdr.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(0, f.hdr.hop_count);
    TEST_ASSERT_EQUAL_HEX8(0x2A, f.hdr.net_id);
    TEST_ASSERT_EQUAL_HEX16(0x1234, f.hdr.dst);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, f.hdr.src);
    TEST_ASSERT_EQUAL_UINT8(5, f.hdr.seq);
    TEST_ASSERT_EQUAL_size_t(2, f.payload_len);
    TEST_ASSERT_EQUAL_PTR(&golden_data[KV_FRAME_HEADER_LEN], f.payload);
}

static void test_fragment_golden_round_trip(void)
{
    kv_frame_t f;
    uint8_t buf[32];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(golden_frag, sizeof(golden_frag), &f));
    TEST_ASSERT_EQUAL_UINT8(KV_FRAME_CTRL, f.hdr.type);
    TEST_ASSERT_TRUE(f.hdr.frag);
    TEST_ASSERT_EQUAL_UINT8(2, f.hdr.frag_index);
    TEST_ASSERT_EQUAL_UINT8(3, f.hdr.frag_last);
    TEST_ASSERT_EQUAL_UINT8(2, f.hdr.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(1, f.hdr.hop_count);
    TEST_ASSERT_EQUAL_HEX16(KV_ADDR_BROADCAST, f.hdr.dst);
    TEST_ASSERT_EQUAL_size_t(1, f.payload_len);
    TEST_ASSERT_EQUAL_size_t(9, kv_frame_header_len(&f.hdr));

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&f, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_frag, buf, sizeof(golden_frag));
}

static void test_all_field_extremes_round_trip(void)
{
    kv_frame_t in = data_frame();
    kv_frame_t out;
    uint8_t buf[32];
    size_t len = 0;

    in.hdr.type = KV_FRAME_EXT;
    in.hdr.ack_req = false;
    in.hdr.frag = true;
    in.hdr.frag_index = 15;
    in.hdr.frag_last = 15;
    in.hdr.hop_limit = 0;
    in.hdr.hop_count = KV_HOP_LIMIT_MAX;
    in.hdr.net_id = 0xFF;
    in.hdr.dst = KV_ADDR_BROADCAST;
    in.hdr.src = 0xFFFE;
    in.hdr.seq = 0xFF;

    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&in, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(buf, len, &out));
    /* field by field: struct padding is not guaranteed to round-trip */
    TEST_ASSERT_EQUAL_UINT8(in.hdr.type, out.hdr.type);
    TEST_ASSERT_EQUAL(in.hdr.ack_req, out.hdr.ack_req);
    TEST_ASSERT_EQUAL(in.hdr.frag, out.hdr.frag);
    TEST_ASSERT_EQUAL(in.hdr.sec, out.hdr.sec);
    TEST_ASSERT_EQUAL_UINT8(in.hdr.hop_limit, out.hdr.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(in.hdr.hop_count, out.hdr.hop_count);
    TEST_ASSERT_EQUAL_HEX8(in.hdr.net_id, out.hdr.net_id);
    TEST_ASSERT_EQUAL_HEX16(in.hdr.dst, out.hdr.dst);
    TEST_ASSERT_EQUAL_HEX16(in.hdr.src, out.hdr.src);
    TEST_ASSERT_EQUAL_UINT8(in.hdr.seq, out.hdr.seq);
    TEST_ASSERT_EQUAL_UINT8(in.hdr.frag_index, out.hdr.frag_index);
    TEST_ASSERT_EQUAL_UINT8(in.hdr.frag_last, out.hdr.frag_last);
    TEST_ASSERT_EQUAL_MEMORY(payload2, out.payload, sizeof(payload2));
}

static void test_empty_payload(void)
{
    kv_frame_t f = data_frame();
    kv_frame_t out;
    uint8_t buf[KV_FRAME_HEADER_LEN];
    size_t len = 0;

    f.payload = NULL;
    f.payload_len = 0;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&f, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(KV_FRAME_HEADER_LEN, len);
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_size_t(0, out.payload_len);
}

static void test_encode_rejects_small_buffer(void)
{
    kv_frame_t f = data_frame();
    uint8_t buf[sizeof(golden_data)];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_frame_encode(&f, buf, sizeof(buf) - 1, &len));
    TEST_ASSERT_EQUAL_INT(KV_ERR_NO_SPACE, kv_frame_encode(&f, buf, 1, &len));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_encode(&f, buf, sizeof(buf), &len));
}

static void test_encode_rejects_out_of_range_fields(void)
{
    uint8_t buf[32];
    size_t len = 0;
    kv_frame_t f;

    f = data_frame();
    f.hdr.type = 16;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.hop_limit = 8;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.frag = true;
    f.hdr.frag_last = 16;
    f.hdr.frag_index = 0;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.payload = NULL;
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_encode(&f, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_encode(NULL, buf, sizeof(buf), &len));
}

static void test_encode_rejects_bad_combinations(void)
{
    uint8_t buf[32];
    size_t len = 0;
    kv_frame_t f;

    f = data_frame(); /* ack_req to broadcast: ACK storm */
    f.hdr.dst = KV_ADDR_BROADCAST;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.src = KV_ADDR_BROADCAST;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.hop_limit = 4;
    f.hdr.hop_count = 4;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.frag = true;
    f.hdr.frag_index = 3;
    f.hdr.frag_last = 2;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_encode(&f, buf, sizeof(buf), &len));

    f = data_frame();
    f.hdr.sec = true;
    TEST_ASSERT_EQUAL_INT(KV_ERR_UNSUPPORTED, kv_frame_encode(&f, buf, sizeof(buf), &len));
}

static void test_decode_rejects_truncated(void)
{
    kv_frame_t f;

    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID_ARG, kv_frame_decode(NULL, 8, &f));
    TEST_ASSERT_EQUAL_INT(KV_ERR_TRUNCATED, kv_frame_decode(golden_data, 0, &f));
    TEST_ASSERT_EQUAL_INT(KV_ERR_TRUNCATED,
                          kv_frame_decode(golden_data, KV_FRAME_HEADER_LEN - 1, &f));
    /* frag flag set but fragment byte missing */
    TEST_ASSERT_EQUAL_INT(KV_ERR_TRUNCATED, kv_frame_decode(golden_frag, KV_FRAME_HEADER_LEN, &f));
}

static void test_decode_rejects_other_version(void)
{
    uint8_t buf[sizeof(golden_data)];
    kv_frame_t f;

    memcpy(buf, golden_data, sizeof(buf));
    buf[0] = (uint8_t)(buf[0] | 0x40u);
    TEST_ASSERT_EQUAL_INT(KV_ERR_VERSION, kv_frame_decode(buf, sizeof(buf), &f));
}

static void test_decode_rejects_bad_combinations(void)
{
    uint8_t buf[sizeof(golden_data)];
    kv_frame_t f;

    memcpy(buf, golden_data, sizeof(buf)); /* ack_req to broadcast */
    buf[3] = 0xFF;
    buf[4] = 0xFF;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_decode(buf, sizeof(buf), &f));

    memcpy(buf, golden_data, sizeof(buf)); /* hop 7 + 7 */
    buf[1] = 0xFC;
    TEST_ASSERT_EQUAL_INT(KV_ERR_MALFORMED, kv_frame_decode(buf, sizeof(buf), &f));

    memcpy(buf, golden_data, sizeof(buf)); /* sec bit */
    buf[1] = (uint8_t)(buf[1] | 0x02u);
    TEST_ASSERT_EQUAL_INT(KV_ERR_UNSUPPORTED, kv_frame_decode(buf, sizeof(buf), &f));
}

static void test_decode_ignores_reserved_bit(void)
{
    uint8_t buf[sizeof(golden_data)];
    kv_frame_t f;

    memcpy(buf, golden_data, sizeof(buf));
    buf[1] = (uint8_t)(buf[1] | 0x01u);
    TEST_ASSERT_EQUAL_INT(KV_OK, kv_frame_decode(buf, sizeof(buf), &f));
    TEST_ASSERT_EQUAL_UINT8(3, f.hdr.hop_limit);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_matches_golden_bytes);
    RUN_TEST(test_decode_golden_bytes);
    RUN_TEST(test_fragment_golden_round_trip);
    RUN_TEST(test_all_field_extremes_round_trip);
    RUN_TEST(test_empty_payload);
    RUN_TEST(test_encode_rejects_small_buffer);
    RUN_TEST(test_encode_rejects_out_of_range_fields);
    RUN_TEST(test_encode_rejects_bad_combinations);
    RUN_TEST(test_decode_rejects_truncated);
    RUN_TEST(test_decode_rejects_other_version);
    RUN_TEST(test_decode_rejects_bad_combinations);
    RUN_TEST(test_decode_ignores_reserved_bit);
    return UNITY_END();
}

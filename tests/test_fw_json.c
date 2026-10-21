/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kfw/json.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    int count;
    char keys[8][16];
    kfw_json_value_t values[8];
    int stop_at; /* member index that returns false, -1 = none */
} seen_t;

static bool collect(void *ctx, const char *key, size_t key_len, const kfw_json_value_t *v)
{
    seen_t *s = (seen_t *)ctx;
    if (s->count == s->stop_at || s->count >= 8 || key_len >= 16) {
        return false;
    }
    memcpy(s->keys[s->count], key, key_len);
    s->keys[s->count][key_len] = '\0';
    s->values[s->count] = *v;
    s->count++;
    return true;
}

static bool parse(const char *text, seen_t *s)
{
    memset(s, 0, sizeof(*s));
    s->stop_at = -1;
    return kfw_json_parse(text, strlen(text), collect, s);
}

static void test_parses_flat_objects(void)
{
    seen_t s;
    TEST_ASSERT_TRUE(parse(" { \"cmd\" : \"config\", \"channel\":76,\"addr\":1099511627775,"
                           "\"neg\":-5, \"listen\":true,\"no_ack\":false, \"s\":\"\" } \r\n",
                           &s));
    TEST_ASSERT_EQUAL_INT(7, s.count);
    TEST_ASSERT_EQUAL_STRING("cmd", s.keys[0]);
    TEST_ASSERT_EQUAL_INT(KFW_JSON_STRING, s.values[0].type);
    TEST_ASSERT_EQUAL_size_t(6, s.values[0].len);
    TEST_ASSERT_EQUAL_MEMORY("config", s.values[0].s, 6);
    TEST_ASSERT_EQUAL_INT(KFW_JSON_INT, s.values[1].type);
    TEST_ASSERT_EQUAL_INT64(76, s.values[1].i);
    TEST_ASSERT_EQUAL_INT64(1099511627775LL, s.values[2].i);
    TEST_ASSERT_EQUAL_INT64(-5, s.values[3].i);
    TEST_ASSERT_EQUAL_INT(KFW_JSON_BOOL, s.values[4].type);
    TEST_ASSERT_TRUE(s.values[4].b);
    TEST_ASSERT_FALSE(s.values[5].b);
    TEST_ASSERT_EQUAL_size_t(0, s.values[6].len);

    TEST_ASSERT_TRUE(parse("{}", &s));
    TEST_ASSERT_EQUAL_INT(0, s.count);
    TEST_ASSERT_TRUE(parse("{\"big\":9223372036854775807}", &s));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.values[0].i);
    TEST_ASSERT_TRUE(parse("{\"zero\":0}", &s));
}

static void test_rejects_everything_else(void)
{
    static const char *const bad[] = {
        "",
        "[]",
        "{",
        "{\"a\":1",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{a:1}",
        "{\"a\":{\"b\":1}}",
        "{\"a\":[1]}",
        "{\"a\":1.5}",
        "{\"a\":1e3}",
        "{\"a\":01}",
        "{\"a\":-}",
        "{\"a\":9223372036854775808}",
        "{\"a\":\"x\\\"y\"}",
        "{\"a\":\"tab\there\"}",
        "{\"a\":\"open}",
        "{\"a\":null}",
        "{\"a\":tru}",
        "{\"a\":1} x",
        "{\"a\":1}{}",
    };
    size_t i;
    seen_t s;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse(bad[i], &s), bad[i]);
    }
}

static void test_callback_can_stop_parsing(void)
{
    seen_t s;
    memset(&s, 0, sizeof(s));
    s.stop_at = 1;
    TEST_ASSERT_FALSE(kfw_json_parse("{\"a\":1,\"b\":2}", 13, collect, &s));
    TEST_ASSERT_EQUAL_INT(1, s.count);
}

static void test_key_is(void)
{
    TEST_ASSERT_TRUE(kfw_json_key_is("cmdx", 3, "cmd"));
    TEST_ASSERT_FALSE(kfw_json_key_is("cmd", 3, "cmds"));
    TEST_ASSERT_FALSE(kfw_json_key_is("cmd", 3, "cm"));
}

static void test_writes_objects(void)
{
    char buf[160];
    static const uint8_t data[] = {0x00, 0x4D, 0xFF};
    kfw_json_out_t o;

    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "tx");
    kfw_json_uint(&o, "t", 18446744073709551615ull);
    kfw_json_int(&o, "min", INT64_MIN);
    kfw_json_int(&o, "neg", -42);
    kfw_json_int(&o, "zero", 0);
    kfw_json_bool(&o, "ack", true);
    kfw_json_bool(&o, "no", false);
    kfw_json_hex(&o, "data", data, sizeof(data));
    TEST_ASSERT_TRUE(kfw_json_end(&o));
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"tx\",\"t\":18446744073709551615,"
                             "\"min\":-9223372036854775808,\"neg\":-42,\"zero\":0,"
                             "\"ack\":true,\"no\":false,\"data\":\"004DFF\"}",
                             buf);

    kfw_json_begin(&o, buf, sizeof(buf));
    TEST_ASSERT_TRUE(kfw_json_end(&o));
    TEST_ASSERT_EQUAL_STRING("{}", buf);
}

static void test_writer_reports_overflow(void)
{
    char buf[12];
    kfw_json_out_t o;

    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_str(&o, "ev", "a long value");
    TEST_ASSERT_FALSE(kfw_json_end(&o));
    TEST_ASSERT_EQUAL_STRING("", buf);

    kfw_json_begin(&o, buf, 0); /* no room at all: nothing is written */
    TEST_ASSERT_FALSE(kfw_json_end(&o));

    /* exactly fits: 11 characters and the terminator */
    kfw_json_begin(&o, buf, sizeof(buf));
    kfw_json_int(&o, "abc", 123);
    TEST_ASSERT_TRUE(kfw_json_end(&o));
    TEST_ASSERT_EQUAL_STRING("{\"abc\":123}", buf);
}

static void test_hex_decode(void)
{
    uint8_t out[4];
    size_t n = 99;

    TEST_ASSERT_TRUE(kfw_hex_decode("00aBfF", 6, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[2]);
    TEST_ASSERT_TRUE(kfw_hex_decode("", 0, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_FALSE(kfw_hex_decode("abc", 3, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(kfw_hex_decode("0g", 2, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(kfw_hex_decode("g0", 2, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(kfw_hex_decode("0011223344", 10, out, sizeof(out), &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_flat_objects);
    RUN_TEST(test_rejects_everything_else);
    RUN_TEST(test_callback_can_stop_parsing);
    RUN_TEST(test_key_is);
    RUN_TEST(test_writes_objects);
    RUN_TEST(test_writer_reports_overflow);
    RUN_TEST(test_hex_decode);
    return UNITY_END();
}

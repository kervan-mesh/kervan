/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* The board firmware's logic: two boards with modelled nRF24 chips, driven by command lines. */

#include "fake_nrf24.h"
#include "kfw/app.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 256

typedef struct {
    char lines[MAX_LINES][KFW_LINE_MAX];
    int n;
    int activity;
} out_t;

static fake_air_t air;
static fake_nrf24_t chip_a, chip_b;
static kfw_app_t a, b;
static out_t out_a, out_b;
static bool lose_all;

static void write_line(void *ctx, const char *line)
{
    out_t *o = (out_t *)ctx;
    TEST_ASSERT_TRUE(strlen(line) < KFW_LINE_MAX);
    if (o->n < MAX_LINES) {
        memcpy(o->lines[o->n++], line, strlen(line) + 1u);
    }
}

static void activity(void *ctx)
{
    ((out_t *)ctx)->activity++;
}

static bool deliver(void *ctx, const fake_nrf24_t *from, const fake_nrf24_t *to, unsigned attempt)
{
    (void)ctx;
    (void)from;
    (void)to;
    (void)attempt;
    return !lose_all;
}

static void boot(kfw_app_t *app, fake_nrf24_t *chip, out_t *o, const char *id, bool with_radio)
{
    kfw_app_host_t host;
    kfw_nrf24_io_t io;

    memset(o, 0, sizeof(*o));
    host.write_line = write_line;
    host.activity = activity;
    host.ctx = o;
    host.board = "rpi-pico";
    host.board_id = id;
    fake_nrf24_init(chip, &air);
    io = fake_nrf24_io(chip);
    kfw_app_init(app, &host, with_radio ? &io : NULL);
}

void setUp(void)
{
    fake_air_init(&air);
    air.deliver = deliver;
    lose_all = false;
    boot(&a, &chip_a, &out_a, "E6605838", true);
    boot(&b, &chip_b, &out_b, "E6605839", true);
}

void tearDown(void) {}

static void cmd(kfw_app_t *app, const char *line, uint64_t now)
{
    kfw_app_line(app, line, strlen(line), now);
}

static const char *last(const out_t *o)
{
    return o->n > 0 ? o->lines[o->n - 1] : "";
}

static int count(const out_t *o, const char *needle)
{
    int i;
    int n = 0;
    for (i = 0; i < o->n; i++) {
        n += strstr(o->lines[i], needle) != NULL ? 1 : 0;
    }
    return n;
}

/* Runs both boards from `from` to `to` (µs) in 1 ms steps. */
static void run(uint64_t from, uint64_t to)
{
    uint64_t t;
    for (t = from; t <= to; t += 1000u) {
        kfw_app_poll(&a, t);
        kfw_app_poll(&b, t);
    }
}

/* A listens on 0xE7E7E7E701; B sends to it every 100 ms. */
static void start_pair(const char *extra_b, const char *extra_a)
{
    char line[KFW_LINE_MAX];
    snprintf(line, sizeof(line), "{\"cmd\":\"config\",\"node\":1,\"addr\":994074536705%s}",
             extra_a);
    cmd(&a, line, 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"ok\",\"cmd\":\"config\"}", last(&out_a));
    snprintf(line, sizeof(line),
             "{\"cmd\":\"config\",\"node\":2,\"addr\":994074536706,\"listen\":false,"
             "\"send_to\":994074536705,\"every_ms\":100,\"len\":4%s}",
             extra_b);
    cmd(&b, line, 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"ok\",\"cmd\":\"config\"}", last(&out_b));
    cmd(&a, "{\"cmd\":\"start\"}", 0);
    cmd(&b, "{\"cmd\":\"start\"}", 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"ok\",\"cmd\":\"start\"}", last(&out_b));
}

static void test_hello(void)
{
    kfw_app_t none;
    fake_nrf24_t chip;
    out_t o;

    cmd(&a, " {\"cmd\":\"hello\"} ", 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"hello\",\"fw\":\"kervan-nrf24\",\"version\":1,"
                             "\"proto\":1,\"board\":\"rpi-pico\",\"id\":\"E6605838\","
                             "\"radio\":\"nrf24l01p\"}",
                             last(&out_a));

    boot(&none, &chip, &o, "X", false);
    cmd(&none, "{\"cmd\":\"hello\"}", 0);
    TEST_ASSERT_NOT_NULL(strstr(last(&o), "\"radio\":\"none\""));
    cmd(&none, "{\"cmd\":\"config\"}", 0);
    TEST_ASSERT_NOT_NULL(strstr(last(&o), "no nRF24 radio found"));
}

static void test_rejects_bad_commands(void)
{
    static const char *const cases[][2] = {
        {"hello", "not a JSON object"},
        {"{\"cmd\":\"fly\"}", "unknown cmd"},
        {"{}", "unknown cmd"},
        {"{\"cmd\":7}", "bad value for 'cmd'"},
        {"{\"cmd\":\"averyveryverylongcommand\"}", "bad value for 'cmd'"},
        {"{\"cmd\":\"start\",\"node\":1}", "only config takes options"},
        {"{\"cmd\":\"config\",\"colour\":1}", "unknown key 'colour'"},
        {"{\"cmd\":\"config\",\"channel\":126}", "out of range: 'channel'"},
        {"{\"cmd\":\"config\",\"channel\":\"5\"}", "out of range: 'channel'"},
        {"{\"cmd\":\"config\",\"send_to\":1099511627776}", "out of range: 'send_to'"},
        {"{\"cmd\":\"config\",\"listen\":1}", "needs true or false: 'listen'"},
        {"{\"cmd\":\"config\",\"payload\":\"0g\"}", "needs up to 32 bytes as hex: 'payload'"},
        {"{\"cmd\":\"config\",\"payload\":5}", "needs up to 32 bytes as hex: 'payload'"},
        {"{\"cmd\":\"config\",\"every_ms\":10}", "sending (every_ms) needs send_to"},
        {"{\"cmd\":\"config\",\"rate\":500000}", "radio settings out of range"},
        {"{\"cmd\":\"start\"}", "start needs a config first"},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cmd(&a, cases[i][0], 0);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last(&out_a), "\"ev\":\"error\""), cases[i][0]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last(&out_a), cases[i][1]), last(&out_a));
    }
}

static void test_sends_and_reports_like_the_esb_stack(void)
{
    char want[64];
    const char *first;
    unsigned long long t;

    start_pair("", "");
    run(0, 1000000);
    /* ten periods of 100 ms in one second: 10 sends (11 if the first falls on 0 ms) */
    TEST_ASSERT_TRUE(count(&out_b, "\"ev\":\"tx\"") >= 10);
    TEST_ASSERT_EQUAL_INT(count(&out_b, "\"ev\":\"tx\""), count(&out_b, "\"ack\":true"));
    TEST_ASSERT_EQUAL_INT(count(&out_b, "\"ev\":\"tx\""), count(&out_a, "\"ev\":\"rx\""));
    TEST_ASSERT_EQUAL_INT(count(&out_b, "\"ev\":\"tx\""), out_b.activity);

    /* test data: the sender, then seq + i */
    first = out_b.lines[2];
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(first, "\"seq\":0,\"data\":\"02010203\",\"ack\":true,"
                                               "\"retries\":0}"),
                                 first);
    TEST_ASSERT_NOT_NULL(strstr(out_b.lines[3], "\"seq\":1,\"data\":\"02020304\""));
    t = strtoull(strstr(first, "\"t\":") + 4, NULL, 10);
    TEST_ASSERT_TRUE(t < 100000u);
    snprintf(want, sizeof(want), "\"pipe\":1,\"data\":\"02010203\",\"rpd\":false}");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out_a.lines[2], want), out_a.lines[2]);
}

static void test_fixed_payload_ack_payload_and_lost_frames(void)
{
    start_pair(",\"payload\":\"4D6572\",\"arc\":2", ",\"ack_payload_len\":2");
    run(0, 300000);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out_b.lines[2], "\"data\":\"4D6572\",\"ack\":true,"
                                                        "\"retries\":0,\"ack_data\":\"A0A1\"}"),
                                 out_b.lines[2]);
    lose_all = true;
    out_b.n = 0;
    run(301000, 500000);
    TEST_ASSERT_TRUE(out_b.n >= 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out_b.lines[0], "\"ack\":false,\"retries\":2}"),
                                 out_b.lines[0]);
}

static void test_no_ack(void)
{
    start_pair(",\"no_ack\":true", "");
    run(0, 200000);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out_b.lines[2], "\"ack\":false,\"no_ack\":true,"
                                                        "\"retries\":0}"),
                                 out_b.lines[2]);
    TEST_ASSERT_TRUE(count(&out_a, "\"ev\":\"rx\"") >= 1);
}

static void test_a_stuck_radio_times_out_and_reports_busy_periods(void)
{
    start_pair(",\"every_ms\":30", "");
    chip_b.stuck = true;
    run(0, 160000);
    TEST_ASSERT_TRUE(count(&out_b, "\"ev\":\"tx_busy\"") >= 2);
    TEST_ASSERT_EQUAL_INT(1, count(&out_b, "did not finish sending"));
    TEST_ASSERT_EQUAL_INT(0, count(&out_b, "\"ev\":\"tx\""));
}

static void test_jitter_varies_the_period(void)
{
    int i;
    unsigned long long prev = 0;
    bool varied = false;

    start_pair(",\"jitter_ms\":20", "");
    run(0, 2000000);
    for (i = 2; i < out_b.n; i++) {
        const unsigned long long t = strtoull(strstr(out_b.lines[i], "\"t\":") + 4, NULL, 10);
        if (i > 2) {
            const unsigned long long d = t - prev;
            TEST_ASSERT_TRUE(d >= 80000u && d <= 121000u);
            varied = varied || d != 100000u;
        }
        prev = t;
    }
    TEST_ASSERT_TRUE(varied);
}

static void test_stop_ping_and_disconnect(void)
{
    int sent;

    start_pair("", "");
    run(0, 300000);
    cmd(&b, "{\"cmd\":\"ping\"}", 350000);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"pong\",\"t\":350000}", last(&out_b));
    cmd(&b, "{\"cmd\":\"stop\"}", 350000);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"ok\",\"cmd\":\"stop\"}", last(&out_b));
    sent = count(&out_b, "\"ev\":\"tx\"");
    run(351000, 800000);
    TEST_ASSERT_EQUAL_INT(sent, count(&out_b, "\"ev\":\"tx\""));
    cmd(&b, "{\"cmd\":\"ping\"}", 900000);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"pong\",\"t\":0}", last(&out_b));

    cmd(&b, "{\"cmd\":\"start\"}", 1000000);
    run(1000000, 1300000);
    TEST_ASSERT_TRUE(count(&out_b, "\"ev\":\"tx\"") > sent);
    kfw_app_disconnected(&b);
    sent = count(&out_b, "\"ev\":\"tx\"");
    run(1301000, 1600000);
    TEST_ASSERT_EQUAL_INT(sent, count(&out_b, "\"ev\":\"tx\""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hello);
    RUN_TEST(test_rejects_bad_commands);
    RUN_TEST(test_sends_and_reports_like_the_esb_stack);
    RUN_TEST(test_fixed_payload_ack_payload_and_lost_frames);
    RUN_TEST(test_no_ack);
    RUN_TEST(test_a_stuck_radio_times_out_and_reports_busy_periods);
    RUN_TEST(test_jitter_varies_the_period);
    RUN_TEST(test_stop_ping_and_disconnect);
    return UNITY_END();
}

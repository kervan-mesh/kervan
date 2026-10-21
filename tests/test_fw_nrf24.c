/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* The nRF24L01+ driver against the register model in firmware/testing. */

#include "fake_nrf24.h"
#include "kfw/nrf24.h"
#include "unity.h"

#include <string.h>

static fake_air_t air;
static fake_nrf24_t chip_a, chip_b;
static kfw_nrf24_t a, b;
static unsigned drop_first; /* attempts lost before one arrives */

static bool deliver(void *ctx, const fake_nrf24_t *from, const fake_nrf24_t *to, unsigned attempt)
{
    (void)ctx;
    (void)from;
    (void)to;
    return attempt >= drop_first;
}

static kfw_nrf24_config_t config(uint64_t addr)
{
    kfw_nrf24_config_t c;
    memset(&c, 0, sizeof(c));
    c.channel = 76;
    c.data_rate = 1000000;
    c.crc_bytes = 2;
    c.pa_level = 3;
    c.addr_width = 5;
    c.addr = addr;
    c.pipes = 1;
    c.ard_us = 500;
    c.arc = 3;
    return c;
}

void setUp(void)
{
    kfw_nrf24_io_t io;
    kfw_nrf24_config_t c;

    fake_air_init(&air);
    air.deliver = deliver;
    drop_first = 0;
    fake_nrf24_init(&chip_a, &air);
    fake_nrf24_init(&chip_b, &air);
    io = fake_nrf24_io(&chip_a);
    TEST_ASSERT_TRUE(kfw_nrf24_probe(&a, &io));
    io = fake_nrf24_io(&chip_b);
    TEST_ASSERT_TRUE(kfw_nrf24_probe(&b, &io));
    c = config(0xE7E7E7E701ull);
    TEST_ASSERT_TRUE(kfw_nrf24_configure(&a, &c));
    c = config(0xE7E7E7E702ull);
    TEST_ASSERT_TRUE(kfw_nrf24_configure(&b, &c));
}

void tearDown(void) {}

static void test_probe_fails_without_a_chip(void)
{
    fake_nrf24_t none;
    kfw_nrf24_t r;
    kfw_nrf24_io_t io;

    fake_nrf24_init(&none, NULL);
    none.present = false;
    io = fake_nrf24_io(&none);
    TEST_ASSERT_FALSE(kfw_nrf24_probe(&r, &io));
}

static void test_configure_sets_the_registers(void)
{
    kfw_nrf24_config_t c = config(0x0102030405ull);
    static const uint8_t addr[5] = {0x05, 0x04, 0x03, 0x02, 0x01};

    c.channel = 100;
    c.data_rate = 250000;
    c.crc_bytes = 1;
    c.pa_level = 1;
    c.pipes = 3;
    c.ard_us = 1500;
    c.arc = 15;
    TEST_ASSERT_TRUE(kfw_nrf24_configure(&a, &c));
    TEST_ASSERT_EQUAL_HEX8(100, kfw_nrf24_read(&a, KFW_NRF24_RF_CH));
    TEST_ASSERT_EQUAL_HEX8(0x22, kfw_nrf24_read(&a, KFW_NRF24_RF_SETUP)); /* RF_DR_LOW, -12 dBm */
    TEST_ASSERT_EQUAL_HEX8(0x5F, kfw_nrf24_read(&a, KFW_NRF24_SETUP_RETR));
    TEST_ASSERT_EQUAL_HEX8(0x03, kfw_nrf24_read(&a, KFW_NRF24_SETUP_AW));
    TEST_ASSERT_EQUAL_HEX8(0x0A, kfw_nrf24_read(&a, KFW_NRF24_CONFIG)); /* EN_CRC, PWR_UP */
    TEST_ASSERT_EQUAL_HEX8(0x0F, kfw_nrf24_read(&a, KFW_NRF24_EN_AA));
    TEST_ASSERT_EQUAL_HEX8(0x0E, kfw_nrf24_read(&a, KFW_NRF24_EN_RXADDR));
    TEST_ASSERT_EQUAL_HEX8(0x0F, kfw_nrf24_read(&a, KFW_NRF24_DYNPD));
    TEST_ASSERT_EQUAL_HEX8(0x07, kfw_nrf24_read(&a, KFW_NRF24_FEATURE));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(addr, chip_a.addr[1], 5);
    TEST_ASSERT_EQUAL_HEX8(0x06, chip_a.addr[2][0]);
    TEST_ASSERT_EQUAL_HEX8(0x07, chip_a.addr[3][0]);
    TEST_ASSERT_TRUE(chip_a.delay_us >= 1500u); /* waited for the oscillator */

    c = config(1);
    c.data_rate = 2000000;
    c.pa_level = 0;
    c.addr_width = 3;
    TEST_ASSERT_TRUE(kfw_nrf24_configure(&a, &c));
    TEST_ASSERT_EQUAL_HEX8(0x08, kfw_nrf24_read(&a, KFW_NRF24_RF_SETUP));
    TEST_ASSERT_EQUAL_HEX8(0x01, kfw_nrf24_read(&a, KFW_NRF24_SETUP_AW));
    TEST_ASSERT_EQUAL_HEX8(0x0E, kfw_nrf24_read(&a, KFW_NRF24_CONFIG)); /* CRCO: 2 bytes */
}

static void test_configure_rejects_bad_values(void)
{
    const kfw_nrf24_config_t good = config(1);
    kfw_nrf24_config_t c;

#define BAD(field, value)                                                                          \
    c = good;                                                                                      \
    c.field = value;                                                                               \
    TEST_ASSERT_FALSE_MESSAGE(kfw_nrf24_configure(&a, &c), #field)
    BAD(channel, 126);
    BAD(data_rate, 500000);
    BAD(crc_bytes, 0);
    BAD(crc_bytes, 3);
    BAD(pa_level, 4);
    BAD(addr_width, 2);
    BAD(addr_width, 6);
    BAD(pipes, 0);
    BAD(pipes, 6);
    BAD(ard_us, 0);
    BAD(ard_us, 4250);
    BAD(ard_us, 300);
    BAD(arc, 16);
#undef BAD
}

static void test_sends_and_receives_with_ack(void)
{
    static const uint8_t data[] = {1, 2, 3, 4, 5};
    uint8_t retries = 99;
    uint8_t ack[32];
    uint8_t ack_len = 99;
    uint8_t pipe = 0;
    uint8_t got[32];
    uint8_t len = 0;
    bool rpd = true;

    kfw_nrf24_listen(&a);
    TEST_ASSERT_TRUE(a.rx_mode);
    TEST_ASSERT_FALSE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_PENDING, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));

    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data, sizeof(data), false);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    TEST_ASSERT_EQUAL_UINT8(0, retries);
    TEST_ASSERT_EQUAL_UINT8(0, ack_len);
    TEST_ASSERT_FALSE(chip_b.ce);
    TEST_ASSERT_EQUAL_HEX8(0x02, kfw_nrf24_read(&b, KFW_NRF24_EN_RXADDR)); /* pipe 0 off again */

    TEST_ASSERT_TRUE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_EQUAL_UINT8(1, pipe);
    TEST_ASSERT_EQUAL_UINT8(sizeof(data), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, got, sizeof(data));
    TEST_ASSERT_FALSE(rpd);
    TEST_ASSERT_FALSE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_EQUAL_HEX8(0, kfw_nrf24_read(&a, KFW_NRF24_STATUS) & KFW_NRF24_RX_DR);
}

static void test_counts_retransmissions(void)
{
    static const uint8_t data[] = {9};
    uint8_t retries = 0;
    uint8_t ack[32];
    uint8_t ack_len = 0;
    uint8_t pipe = 0;
    uint8_t got[32];
    uint8_t len = 0;
    bool rpd = false;

    kfw_nrf24_listen(&a);
    drop_first = 2;
    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data, 1, false);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    TEST_ASSERT_EQUAL_UINT8(2, retries);
    TEST_ASSERT_EQUAL_UINT(3, chip_b.sent);
    TEST_ASSERT_TRUE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_FALSE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd)); /* received once */
}

static void test_fails_after_every_retransmission(void)
{
    static const uint8_t data[] = {9};
    uint8_t retries = 0;
    uint8_t ack[32];
    uint8_t ack_len = 0;
    static const uint8_t data2[] = {7, 7};

    /* nobody listens */
    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data, 1, false);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_FAILED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    TEST_ASSERT_EQUAL_UINT8(3, retries);
    TEST_ASSERT_EQUAL_UINT(4, chip_b.sent);
    TEST_ASSERT_EQUAL_UINT8(0, chip_b.tx_n); /* flushed */
    TEST_ASSERT_EQUAL_HEX8(0, kfw_nrf24_read(&b, KFW_NRF24_STATUS) & KFW_NRF24_MAX_RT);

    /* the next send works again */
    kfw_nrf24_listen(&a);
    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data2, 2, false);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
}

static void test_no_ack_needs_no_receiver(void)
{
    static const uint8_t data[] = {1, 2};
    uint8_t retries = 99;
    uint8_t ack[32];
    uint8_t ack_len = 0;

    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data, 2, true);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    TEST_ASSERT_EQUAL_UINT8(0, retries);
    TEST_ASSERT_EQUAL_UINT(1, chip_b.sent);
}

static void test_ack_payloads_and_more_pipes(void)
{
    kfw_nrf24_config_t c = config(0xE7E7E7E701ull);
    static const uint8_t ackp[] = {0xA0, 0xA1, 0xA2};
    static const uint8_t data[33] = {0x55};
    uint8_t retries = 0;
    uint8_t ack[32];
    uint8_t ack_len = 0;
    uint8_t pipe = 0;
    uint8_t got[32];
    uint8_t len = 0;
    bool rpd = false;

    c.pipes = 3;
    TEST_ASSERT_TRUE(kfw_nrf24_configure(&a, &c));
    kfw_nrf24_listen(&a);
    kfw_nrf24_ack_payload(&a, 3, ackp, sizeof(ackp));
    kfw_nrf24_ack_payload(&a, 6, ackp, sizeof(ackp)); /* no such pipe: ignored */
    TEST_ASSERT_EQUAL_UINT8(1, chip_a.ack_n);

    kfw_nrf24_send(&b, 0xE7E7E7E703ull, data, sizeof(data), false); /* cut to 32 bytes */
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    TEST_ASSERT_EQUAL_UINT8(sizeof(ackp), ack_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ackp, ack, sizeof(ackp));
    TEST_ASSERT_TRUE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_EQUAL_UINT8(3, pipe);
    TEST_ASSERT_EQUAL_UINT8(32, len);
}

static void test_drops_payloads_with_a_bad_length(void)
{
    static const uint8_t data[] = {1};
    uint8_t pipe = 0;
    uint8_t got[32];
    uint8_t len = 0;
    bool rpd = false;
    uint8_t retries = 0;
    uint8_t ack[32];
    uint8_t ack_len = 0;

    kfw_nrf24_listen(&a);
    kfw_nrf24_send(&b, 0xE7E7E7E701ull, data, 1, false);
    TEST_ASSERT_EQUAL_INT(KFW_NRF24_TX_ACKED, kfw_nrf24_tx_poll(&b, &retries, ack, &ack_len));
    chip_a.rx[0].len = 40; /* as a corrupt R_RX_PL_WID would say */
    TEST_ASSERT_FALSE(kfw_nrf24_rx_poll(&a, &pipe, got, &len, &rpd));
    TEST_ASSERT_EQUAL_UINT8(0, chip_a.rx_n);
}

static void test_standby_and_power_down(void)
{
    kfw_nrf24_listen(&a);
    kfw_nrf24_standby(&a);
    TEST_ASSERT_FALSE(chip_a.ce);
    TEST_ASSERT_FALSE(a.rx_mode);
    kfw_nrf24_power_down(&a);
    TEST_ASSERT_EQUAL_HEX8(0, kfw_nrf24_read(&a, KFW_NRF24_CONFIG));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_fails_without_a_chip);
    RUN_TEST(test_configure_sets_the_registers);
    RUN_TEST(test_configure_rejects_bad_values);
    RUN_TEST(test_sends_and_receives_with_ack);
    RUN_TEST(test_counts_retransmissions);
    RUN_TEST(test_fails_after_every_retransmission);
    RUN_TEST(test_no_ack_needs_no_receiver);
    RUN_TEST(test_ack_payloads_and_more_pipes);
    RUN_TEST(test_drops_payloads_with_a_bad_length);
    RUN_TEST(test_standby_and_power_down);
    return UNITY_END();
}

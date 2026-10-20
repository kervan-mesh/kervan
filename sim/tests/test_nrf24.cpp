/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include "ksim/nrf24.hpp"
#include "ksim/phy.hpp"

using namespace ksim;

static Nrf24Params rate(uint32_t r, int channel = 76)
{
    Nrf24Params p;
    p.data_rate = r;
    p.channel = channel;
    return p;
}

// Frame = address (5) + PCF container (2) + payload. Airtime bits = 8 + 8*5 + 9 + 8*payload + 16.
TEST(Nrf24, AirtimeFromPacketFormat)
{
    EXPECT_EQ(nrf24_airtime_us(rate(1000000), 5 + 2 + 32), 329u); // full payload
    EXPECT_EQ(nrf24_airtime_us(rate(1000000), 5 + 2), 73u);       // empty ACK
    EXPECT_EQ(nrf24_airtime_us(rate(250000), 5 + 2), 292u);
    EXPECT_EQ(nrf24_airtime_us(rate(2000000), 5 + 2 + 32), 165u); // 164.5 rounds up
    Nrf24Params p = rate(1000000);
    p.crc_bytes = 1;
    EXPECT_EQ(nrf24_airtime_us(p, 5 + 2 + 32), 321u);
    EXPECT_EQ(nrf24_airtime_us(rate(1000000), 3 + 2), 57u); // 3-byte address
}

TEST(Nrf24, CarrierSensitivityCapture)
{
    EXPECT_EQ(nrf24_carrier_hz(rate(1000000, 0)), 2400000000u);
    EXPECT_EQ(nrf24_carrier_hz(rate(1000000, 125)), 2525000000u);
    EXPECT_DOUBLE_EQ(nrf24_sensitivity_dbm(250000), -94.0);
    EXPECT_DOUBLE_EQ(nrf24_sensitivity_dbm(1000000), -85.0);
    EXPECT_DOUBLE_EQ(nrf24_sensitivity_dbm(2000000), -82.0);
    EXPECT_DOUBLE_EQ(nrf24_capture_db(250000), 12.0);
    EXPECT_DOUBLE_EQ(nrf24_capture_db(1000000), 9.0);
    EXPECT_DOUBLE_EQ(nrf24_capture_db(2000000), 7.0);
}

TEST(Nrf24, OccupiedBandsOverlap)
{
    EXPECT_TRUE(nrf24_bands_overlap(rate(1000000, 10), rate(1000000, 10)));
    EXPECT_FALSE(nrf24_bands_overlap(rate(1000000, 10), rate(1000000, 11))); // 1 MHz apart
    EXPECT_TRUE(nrf24_bands_overlap(rate(2000000, 10), rate(1000000, 11)));  // 2 MHz wide
    EXPECT_TRUE(nrf24_bands_overlap(rate(2000000, 10), rate(2000000, 11)));
    EXPECT_FALSE(nrf24_bands_overlap(rate(2000000, 10), rate(2000000, 12)));
    EXPECT_FALSE(nrf24_bands_overlap(rate(250000, 10), rate(250000, 11)));
}

TEST(Nrf24, Validate)
{
    EXPECT_EQ(nrf24_validate(rate(1000000)), "");
    EXPECT_NE(nrf24_validate(rate(1000000, 126)), "");
    EXPECT_NE(nrf24_validate(rate(1000000, -1)), "");
    EXPECT_NE(nrf24_validate(rate(500000)), "");
    Nrf24Params p = rate(1000000);
    p.crc_bytes = 0;
    EXPECT_NE(nrf24_validate(p), "");
}

TEST(Phy, LookupAndRules)
{
    ASSERT_NE(phy_by_name("lora"), nullptr);
    ASSERT_NE(phy_by_name("nrf24"), nullptr);
    EXPECT_EQ(phy_by_name("ble"), nullptr);
    EXPECT_EQ(phy_for(PhyType::Nrf24).name(), std::string("nrf24"));

    const Phy &nrf = phy_for(PhyType::Nrf24);
    RadioConfig a;
    a.type = PhyType::Nrf24;
    RadioConfig b = a;
    EXPECT_TRUE(nrf.can_receive(a, b));
    b.nrf24.data_rate = 2000000; // same channel, other rate: cannot demodulate, still interferes
    EXPECT_FALSE(nrf.can_receive(a, b));
    EXPECT_TRUE(nrf.interferes(a, b));
    EXPECT_FALSE(nrf.frame_len_ok(4));
    EXPECT_TRUE(nrf.frame_len_ok(5));
    EXPECT_TRUE(nrf.frame_len_ok(39));
    EXPECT_FALSE(nrf.frame_len_ok(40));
    EXPECT_TRUE(nrf.decodable(a, -85.0, 0.0));
    EXPECT_FALSE(nrf.decodable(a, -85.01, 0.0));

    const Phy &lora = phy_for(PhyType::LoRa);
    RadioConfig l;
    EXPECT_FALSE(lora.frame_len_ok(0));
    EXPECT_TRUE(lora.frame_len_ok(255));
    EXPECT_DOUBLE_EQ(lora.capture_db(l, 4.5), 4.5); // scenario setting
}

TEST(Phy, SetAndGet)
{
    const Phy &nrf = phy_for(PhyType::Nrf24);
    RadioConfig r;
    r.type = PhyType::Nrf24;
    int64_t v = 0;
    EXPECT_EQ(nrf.set(r, KSIM_RADIO_NRF24_CHANNEL, 100), KSIM_OK);
    EXPECT_EQ(nrf.get(r, KSIM_RADIO_NRF24_CHANNEL, &v), KSIM_OK);
    EXPECT_EQ(v, 100);
    EXPECT_EQ(nrf.set(r, KSIM_RADIO_NRF24_CHANNEL, 126), KSIM_ERR_INVALID);
    EXPECT_EQ(nrf.set(r, KSIM_RADIO_NRF24_RATE, 250000), KSIM_OK);
    EXPECT_EQ(nrf.set(r, KSIM_RADIO_NRF24_RATE, 300000), KSIM_ERR_INVALID);
    EXPECT_EQ(nrf.set(r, KSIM_RADIO_LORA_SF, 7), KSIM_ERR_UNSUPPORTED);
    EXPECT_EQ(r.nrf24.channel, 100); // failed sets leave the config alone

    const Phy &lora = phy_for(PhyType::LoRa);
    RadioConfig l;
    EXPECT_EQ(lora.set(l, KSIM_RADIO_LORA_BW_HZ, 250000), KSIM_OK);
    EXPECT_EQ(lora.set(l, KSIM_RADIO_LORA_BW_HZ, 300000), KSIM_ERR_INVALID);
    EXPECT_EQ(lora.set(l, KSIM_RADIO_FREQ_HZ, 915000000), KSIM_OK);
    EXPECT_EQ(lora.get(l, KSIM_RADIO_FREQ_HZ, &v), KSIM_OK);
    EXPECT_EQ(v, 915000000);
    EXPECT_EQ(lora.get(l, KSIM_RADIO_LORA_BW_HZ, &v), KSIM_OK);
    EXPECT_EQ(v, 250000);
}

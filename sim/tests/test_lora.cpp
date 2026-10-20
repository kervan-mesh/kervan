/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include "ksim/lora.hpp"

using ksim::LoraParams;

static LoraParams lora(int sf, uint32_t bw = 125000)
{
    LoraParams p;
    p.sf = sf;
    p.bw_hz = bw;
    return p;
}

// Golden values worked out by hand from Semtech AN1200.13 (preamble 8, CR 4/5, CRC on,
// explicit header); they match the Semtech LoRa calculator.
TEST(LoraAirtime, Sf7Bw125TenBytes)
{
    // Tsym 1.024 ms; payload symbols 8 + ceil(96/28)*5 = 28; (12.25 + 28) * 1.024 ms
    EXPECT_EQ(ksim::lora_airtime_us(lora(7), 10), 41216u);
}

TEST(LoraAirtime, Sf12Bw125UsesLdro)
{
    EXPECT_TRUE(ksim::lora_ldro(lora(12)));
    EXPECT_TRUE(ksim::lora_ldro(lora(11)));
    EXPECT_FALSE(ksim::lora_ldro(lora(10)));
    // Tsym 32.768 ms; payload symbols 8 + ceil(156/40)*5 = 28; (12.25 + 28) * 32.768 ms
    EXPECT_EQ(ksim::lora_airtime_us(lora(12), 20), 1318912u);
    // 51 bytes (LoRa SF12 limit in EU868): 8 + ceil(404/40)*5 = 63 symbols
    EXPECT_EQ(ksim::lora_airtime_us(lora(12), 51), 2465792u);
}

TEST(LoraAirtime, WiderBandwidthIsFaster)
{
    EXPECT_EQ(ksim::lora_airtime_us(lora(7, 250000), 10), 41216u / 2u);
    EXPECT_EQ(ksim::lora_airtime_us(lora(7, 500000), 10), 41216u / 4u);
    EXPECT_FALSE(ksim::lora_ldro(lora(12, 500000)));
}

TEST(LoraAirtime, CodingRateAndHeaderOptions)
{
    LoraParams p = lora(7);
    p.cr = 4; // 4/8: 8 + 4*8 = 40 payload symbols
    EXPECT_EQ(ksim::lora_airtime_us(p, 10), static_cast<uint64_t>((12.25 + 40) * 1024));
    p = lora(7);
    p.crc = false;
    p.implicit_header = true; // num = 80 - 28 + 28 - 20 = 60 -> ceil(60/28) = 3 -> 23 symbols
    EXPECT_EQ(ksim::lora_airtime_us(p, 10), static_cast<uint64_t>((12.25 + 23) * 1024));
}

TEST(LoraAirtime, EmptyPayloadHasMinimumSymbols)
{
    LoraParams p = lora(12);
    p.crc = false;
    p.implicit_header = true; // num = -48 + 28 - 20 < 0 -> only the 8 fixed symbols
    EXPECT_EQ(ksim::lora_airtime_us(p, 0), static_cast<uint64_t>((12.25 + 8) * 32768));
}

TEST(Lora, SnrThresholds)
{
    EXPECT_DOUBLE_EQ(ksim::lora_snr_threshold_db(7), -7.5);
    EXPECT_DOUBLE_EQ(ksim::lora_snr_threshold_db(9), -12.5);
    EXPECT_DOUBLE_EQ(ksim::lora_snr_threshold_db(12), -20.0);
}

TEST(Lora, NoiseFloor)
{
    EXPECT_NEAR(ksim::noise_floor_dbm(125000, 6.0), -117.03, 0.01);
    EXPECT_NEAR(ksim::noise_floor_dbm(500000, 6.0), -111.01, 0.01);
}

TEST(Lora, Validate)
{
    EXPECT_EQ(ksim::lora_validate(lora(7)), "");
    EXPECT_NE(ksim::lora_validate(lora(6)), "");
    EXPECT_NE(ksim::lora_validate(lora(13)), "");
    EXPECT_NE(ksim::lora_validate(lora(7, 100000)), "");
    LoraParams p = lora(7);
    p.cr = 5;
    EXPECT_NE(ksim::lora_validate(p), "");
    p = lora(7);
    p.preamble = 5;
    EXPECT_NE(ksim::lora_validate(p), "");
}

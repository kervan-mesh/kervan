/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/nrf24.hpp"

#include <cmath>
#include <cstdlib>

namespace ksim {

std::string nrf24_validate(const Nrf24Params &p)
{
    if (p.channel < 0 || p.channel > 125) {
        return "channel must be 0..125";
    }
    if (p.data_rate != 250000 && p.data_rate != 1000000 && p.data_rate != 2000000) {
        return "data_rate must be 250000, 1000000 or 2000000";
    }
    if (p.crc_bytes != 1 && p.crc_bytes != 2) {
        return "crc_bytes must be 1 or 2";
    }
    return {};
}

uint64_t nrf24_carrier_hz(const Nrf24Params &p)
{
    return (2400u + static_cast<uint64_t>(p.channel)) * 1000000u;
}

Time nrf24_airtime_us(const Nrf24Params &p, size_t frame_len)
{
    // the two PCF container bytes are 9 bits on air
    const size_t bytes = frame_len >= 2 ? frame_len - 2 : 0;
    const uint64_t bits = 8u + 8u * bytes + 9u + 8u * static_cast<uint64_t>(p.crc_bytes);
    return static_cast<Time>(
        std::llround(static_cast<double>(bits) * 1e6 / static_cast<double>(p.data_rate)));
}

double nrf24_sensitivity_dbm(uint32_t data_rate)
{
    switch (data_rate) {
    case 250000:
        return -94.0;
    case 1000000:
        return -85.0;
    default:
        return -82.0;
    }
}

double nrf24_capture_db(uint32_t data_rate)
{
    switch (data_rate) {
    case 250000:
        return 12.0;
    case 1000000:
        return 9.0;
    default:
        return 7.0;
    }
}

double nrf24_half_band_mhz(uint32_t data_rate)
{
    return data_rate >= 2000000 ? 1.0 : 0.5;
}

bool nrf24_bands_overlap(const Nrf24Params &a, const Nrf24Params &b)
{
    const double distance = std::abs(static_cast<double>(a.channel - b.channel));
    return distance < nrf24_half_band_mhz(a.data_rate) + nrf24_half_band_mhz(b.data_rate);
}

} // namespace ksim

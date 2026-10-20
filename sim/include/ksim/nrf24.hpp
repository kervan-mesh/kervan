/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ksim/engine.hpp"

namespace ksim {

// nRF24L01+ radio settings (docs/nrf24-esb.md).
struct Nrf24Params {
    int channel = 76;              // 0..125, carrier 2400 + channel MHz
    uint32_t data_rate = 1000000;  // 250000, 1000000, 2000000 bit/s
    int crc_bytes = 2;             // 1 or 2
};

// Frame passed to radio_send: address (3-5 B) + 2-byte PCF container + payload (0-32 B).
constexpr size_t kNrf24MinFrame = 3 + 2;
constexpr size_t kNrf24MaxFrame = 5 + 2 + 32;

std::string nrf24_validate(const Nrf24Params &p);

uint64_t nrf24_carrier_hz(const Nrf24Params &p);

// preamble + address + 9-bit PCF + payload + CRC, at the data rate; rounded to 1 µs.
Time nrf24_airtime_us(const Nrf24Params &p, size_t frame_len);

double nrf24_sensitivity_dbm(uint32_t data_rate);

// Co-channel C/I: how much stronger the wanted frame must be to survive an interferer.
double nrf24_capture_db(uint32_t data_rate);

// Half of the occupied bandwidth in MHz (2 Mbps occupies 2 MHz).
double nrf24_half_band_mhz(uint32_t data_rate);

// Occupied bands of two transmissions overlap.
bool nrf24_bands_overlap(const Nrf24Params &a, const Nrf24Params &b);

} // namespace ksim

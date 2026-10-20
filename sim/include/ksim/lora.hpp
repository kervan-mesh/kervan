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

struct LoraParams {
    uint64_t freq_hz = 868100000;
    uint32_t bw_hz = 125000; // 125, 250 or 500 kHz
    int sf = 7;              // 7..12
    int cr = 1;              // coding rate 4/(4+cr), 1..4
    int preamble = 8;        // symbols
    bool crc = true;
    bool implicit_header = false;
};

// Empty string if valid, otherwise what is wrong.
std::string lora_validate(const LoraParams &p);

double lora_symbol_time_us(const LoraParams &p);

// Low data rate optimization: mandatory when a symbol lasts longer than 16 ms.
bool lora_ldro(const LoraParams &p);

// Time on air of a frame with `len` payload bytes (Semtech AN1200.13), rounded to 1 µs.
Time lora_airtime_us(const LoraParams &p, size_t len);

// Lowest SNR at which the SF still demodulates (SX127x/SX126x datasheets).
double lora_snr_threshold_db(int sf);

// Thermal noise over the bandwidth plus receiver noise figure.
double noise_floor_dbm(uint32_t bw_hz, double noise_figure_db);

} // namespace ksim

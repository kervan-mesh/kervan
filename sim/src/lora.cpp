/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/lora.hpp"

#include <cmath>

namespace ksim {

std::string lora_validate(const LoraParams &p)
{
    if (p.sf < 7 || p.sf > 12) {
        return "sf must be 7..12";
    }
    if (p.bw_hz != 125000 && p.bw_hz != 250000 && p.bw_hz != 500000) {
        return "bw_hz must be 125000, 250000 or 500000";
    }
    if (p.cr < 1 || p.cr > 4) {
        return "cr must be 1..4 (4/5..4/8)";
    }
    if (p.preamble < 6 || p.preamble > 65535) {
        return "preamble must be 6..65535";
    }
    if (p.freq_hz == 0) {
        return "freq_hz must be > 0";
    }
    return {};
}

double lora_symbol_time_us(const LoraParams &p)
{
    return static_cast<double>(1u << p.sf) * 1e6 / static_cast<double>(p.bw_hz);
}

bool lora_ldro(const LoraParams &p)
{
    return lora_symbol_time_us(p) > 16000.0;
}

Time lora_airtime_us(const LoraParams &p, size_t len)
{
    const long long de = lora_ldro(p) ? 1 : 0;
    const long long num = 8 * static_cast<long long>(len) - 4 * p.sf + 28 + (p.crc ? 16 : 0) -
                          (p.implicit_header ? 20 : 0);
    const long long den = 4 * (p.sf - 2 * de);
    long long payload_symbols = 8;
    if (num > 0) {
        payload_symbols += (num + den - 1) / den * (p.cr + 4);
    }
    const double symbols = static_cast<double>(p.preamble) + 4.25 +
                           static_cast<double>(payload_symbols);
    return static_cast<Time>(std::llround(symbols * lora_symbol_time_us(p)));
}

double lora_snr_threshold_db(int sf)
{
    return -7.5 - 2.5 * static_cast<double>(sf - 7);
}

double noise_floor_dbm(uint32_t bw_hz, double noise_figure_db)
{
    return -174.0 + 10.0 * std::log10(static_cast<double>(bw_hz)) + noise_figure_db;
}

} // namespace ksim

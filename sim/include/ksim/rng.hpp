/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace ksim {

// Own generator and distributions: <random> distributions differ between standard libraries,
// which would make the same seed produce different runs on Windows and Linux.

inline uint64_t splitmix64(uint64_t &state)
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Combine two values into a well-mixed stream id.
inline uint64_t mix(uint64_t a, uint64_t b)
{
    uint64_t x = a ^ (b * 0xD1B54A32D192ED03ull);
    return splitmix64(x);
}

// xoshiro256**: independent streams per (seed, stream) pair.
class Rng {
public:
    Rng(uint64_t seed, uint64_t stream)
    {
        uint64_t x = mix(seed, stream);
        for (auto &w : s_) {
            w = splitmix64(x);
        }
    }

    uint64_t next_u64()
    {
        const uint64_t result = rotl(s_[1] * 5u, 7) * 9u;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    uint32_t next_u32() { return static_cast<uint32_t>(next_u64() >> 32); }

    // [0, 1) with 53 random bits; exact in IEEE double on every platform.
    double uniform() { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

    // Standard normal (Irwin-Hall, 12 uniforms): only + and -, so bit-identical everywhere.
    double normal()
    {
        double sum = 0.0;
        for (int i = 0; i < 12; i++) {
            sum += uniform();
        }
        return sum - 6.0;
    }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t s_[4];
};

} // namespace ksim

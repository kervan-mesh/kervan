/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>

namespace ksim {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

double distance_m(const Vec3 &a, const Vec3 &b);

// Log-distance path loss: PL(d) = PL(d0) + 10 * n * log10(d / d0) + shadowing.
struct PathLoss {
    double exponent = 2.7;                // n: ~2 open field, 2.7-3.5 urban
    double ref_distance_m = 1.0;          // d0
    std::optional<double> ref_loss_db;    // PL(d0); default: free-space loss at the carrier
    double shadowing_sigma_db = 0.0;      // per-link normal shadowing, 0 = off
};

double free_space_loss_db(double d_m, uint64_t freq_hz);

// Without shadowing; distances below d0 count as d0.
double path_loss_db(const PathLoss &model, double d_m, uint64_t freq_hz);

} // namespace ksim

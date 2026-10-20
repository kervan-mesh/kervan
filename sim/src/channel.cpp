/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/channel.hpp"

#include <algorithm>
#include <cmath>

namespace ksim {

double distance_m(const Vec3 &a, const Vec3 &b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double free_space_loss_db(double d_m, uint64_t freq_hz)
{
    // 20 log10(d) + 20 log10(f) + 20 log10(4 pi / c)
    return 20.0 * std::log10(d_m) + 20.0 * std::log10(static_cast<double>(freq_hz)) -
           147.55;
}

double path_loss_db(const PathLoss &model, double d_m, uint64_t freq_hz)
{
    const double d0 = model.ref_distance_m;
    const double d = std::max(d_m, d0);
    const double pl0 = model.ref_loss_db.value_or(free_space_loss_db(d0, freq_hz));
    return pl0 + 10.0 * model.exponent * std::log10(d / d0);
}

} // namespace ksim

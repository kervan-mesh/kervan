/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// JSON the web UI gets for a finished run, shared by /api/run and live sessions.

#pragma once

#include <nlohmann/json.hpp>

#include "ksim/simulator.hpp"

namespace ksim::serve {

// Totals, per-node statistics, app events, versions and the trace hash (hex).
nlohmann::json summary_json(const Scenario &scn, const Summary &sum);

// Every directed pair whose signal is decodable without interference: from, to, rssi, snr.
nlohmann::json links_json(const Scenario &scn, const Simulator &sim);

} // namespace ksim::serve

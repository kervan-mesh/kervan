/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

#include "ksim/simulator.hpp"

namespace ksim {

// Parse a JSON scenario (format: docs/simulator.md). Throws std::runtime_error with the location
// of the problem; unknown keys are errors so typos never fall back to defaults silently.
Scenario parse_scenario(const std::string &json_text);

Scenario load_scenario_file(const std::string &path);

} // namespace ksim

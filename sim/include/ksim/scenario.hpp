/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

#include "ksim/device.hpp"
#include "ksim/simulator.hpp"

namespace ksim {

// Parse a JSON scenario (format: docs/simulator.md). Throws std::runtime_error with the location
// of the problem; unknown keys are errors so typos never fall back to defaults silently.
// Node "device" names are resolved in `devices`.
Scenario parse_scenario(const std::string &json_text, const DeviceRegistry &devices);
Scenario parse_scenario(const std::string &json_text); // built-in devices

// As above; "${scenario_dir}" in process command lines expands to base_dir.
Scenario parse_scenario(const std::string &json_text, const DeviceRegistry &devices,
                        const std::string &base_dir);

Scenario load_scenario_file(const std::string &path, const DeviceRegistry &devices);
Scenario load_scenario_file(const std::string &path); // built-in devices

// The interpreter for "${python}" and Python payloads: KSIM_PYTHON if set, else "python" on
// Windows and "python3" elsewhere.
std::string python_interpreter();

} // namespace ksim

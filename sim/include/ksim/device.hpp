/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ksim/phy.hpp"

namespace ksim {

// A board: MCU + radio, with the radio's defaults and hardware limits. Profiles are data files
// (sim/devices/*.json, format in docs/simulator.md) so new boards need no code.
struct DeviceProfile {
    struct Range {
        double min = 0.0;
        double max = 0.0;
    };

    std::string name;
    int version = 1;
    std::string description;
    std::string mcu;
    RadioConfig radio;                     // defaults for nodes using this device
    std::vector<double> tx_power_levels;   // discrete steps (e.g. nRF24); empty = use the range
    std::optional<Range> tx_power_range;   // dBm
    std::optional<Range> freq_hz_range;    // LoRa: supported band

    // Empty if this device can run with `r`, otherwise why not.
    std::string check(const RadioConfig &r) const;
};

// Throws std::runtime_error pointing at the problem.
DeviceProfile parse_device(const std::string &json_text, const std::string &where = "device");

class DeviceRegistry {
public:
    // The profiles shipped in sim/devices (compiled into the binary).
    static DeviceRegistry builtin();

    // Adds or replaces a profile with the same name.
    void add(DeviceProfile profile);
    // Load every *.json in a directory. Throws on the first invalid file.
    void load_dir(const std::string &dir);

    const DeviceProfile *find(const std::string &name) const;
    std::vector<const DeviceProfile *> all() const;

private:
    std::map<std::string, DeviceProfile> profiles_;
};

} // namespace ksim

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ksim/channel.hpp"
#include "ksim/device.hpp"
#include "ksim/engine.hpp"
#include "ksim/phy.hpp"
#include "ksim/stack.h"

namespace ksim {

// Simulator version, recorded in every trace.
const char *version();

struct NodeSpec {
    uint32_t id = 0;
    Vec3 pos;
    RadioConfig radio;
    std::optional<DeviceProfile> device; // limits radio settings, also at runtime
    std::string stack;
    std::string config; // "key=value;key=value", handed to the stack
};

struct Scenario {
    uint64_t seed = 1;
    Time duration_us = 60'000'000;
    size_t mtu = 255;
    RadioConfig radio; // default for nodes without a device
    PathLoss path_loss;
    double capture_threshold_db = 6.0; // LoRa; nRF24 uses its datasheet C/I
    double noise_figure_db = 6.0;
    double duty_cycle_limit = 0.01;
    std::vector<NodeSpec> nodes;
};

struct NodeStats {
    uint32_t id = 0;
    Time airtime_us = 0;
    uint64_t tx = 0;
    uint64_t rx_ok = 0;
    uint64_t lost_collision = 0;
    uint64_t lost_half_duplex = 0;
    uint64_t lost_rx_off = 0; // receiver off or retuned while the frame arrived
};

struct Summary {
    Time duration_us = 0;
    uint64_t tx = 0;
    uint64_t rx_ok = 0;
    uint64_t lost_collision = 0;
    uint64_t lost_half_duplex = 0;
    uint64_t lost_rx_off = 0;
    std::map<std::string, uint64_t> app_events; // from ksim_host_t::trace
    std::map<std::string, std::string> versions; // "ksim", "phy:lora", "stack:kervan", ...
    std::vector<NodeStats> nodes;                // scenario order
    uint64_t trace_hash = 0;                     // FNV-1a over every trace line
};

class Simulator {
public:
    // Throws std::invalid_argument for an inconsistent scenario.
    explicit Simulator(Scenario scenario);
    ~Simulator();
    Simulator(const Simulator &) = delete;
    Simulator &operator=(const Simulator &) = delete;

    // Throws std::invalid_argument on ABI mismatch or duplicate name.
    void register_stack(const ksim_stack_t *stack);

    // Optional JSON Lines trace output; the hash is computed either way.
    void set_trace(std::ostream *out);

    // Create all nodes, run for the scenario duration, destroy them. Call once.
    // Throws std::runtime_error if a stack is unknown or rejects its config.
    Summary run();

    // Received signal strength from one node at another with their scenario radios (0.01 dB).
    double rssi_dbm(uint32_t from_id, uint32_t to_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ksim

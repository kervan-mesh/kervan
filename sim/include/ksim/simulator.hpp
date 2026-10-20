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
#include <string>
#include <vector>

#include "ksim/channel.hpp"
#include "ksim/engine.hpp"
#include "ksim/lora.hpp"
#include "ksim/stack.h"

namespace ksim {

struct NodeSpec {
    uint32_t id = 0;
    Vec3 pos;
    LoraParams radio;
    std::string stack;
    std::string config; // "key=value;key=value", handed to the stack
};

struct Scenario {
    uint64_t seed = 1;
    Time duration_us = 60'000'000;
    size_t mtu = 255;
    LoraParams radio; // default for nodes
    PathLoss path_loss;
    double capture_threshold_db = 6.0;
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
};

struct Summary {
    Time duration_us = 0;
    uint64_t tx = 0;
    uint64_t rx_ok = 0;
    uint64_t lost_collision = 0;
    uint64_t lost_half_duplex = 0;
    std::map<std::string, uint64_t> app_events; // from ksim_host_t::trace
    std::vector<NodeStats> nodes;               // scenario order
    uint64_t trace_hash = 0;                    // FNV-1a over every trace line
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

    // Received signal strength from one node at another (0.01 dB resolution).
    double rssi_dbm(uint32_t from_id, uint32_t to_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ksim

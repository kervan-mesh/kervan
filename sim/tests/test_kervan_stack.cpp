/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The unmodified Kervan core running inside the simulator.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"

using namespace ksim;

namespace {

uint64_t count(const Summary &s, const std::string &name)
{
    auto it = s.app_events.find(name);
    return it == s.app_events.end() ? 0u : it->second;
}

Summary run(const Scenario &s)
{
    Simulator sim(s);
    sim.register_stack(ksim_stack_kervan());
    return sim.run();
}

NodeSpec kervan_node(uint32_t id, double x, const std::string &config)
{
    NodeSpec n;
    n.id = id;
    n.pos.x = x;
    n.stack = "kervan";
    n.config = config;
    return n;
}

Scenario base(Time duration_us)
{
    Scenario s;
    s.duration_us = duration_us;
    s.mtu = 51;
    s.path_loss.exponent = 3.0;
    s.path_loss.ref_loss_db = 40.0; // SF7 range about 1.9 km (see test_sim.cpp)
    return s;
}

} // namespace

TEST(KervanStack, AcknowledgedExchange)
{
    Scenario s = base(120'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=2;send_every_ms=10000;ack=1"),
               kervan_node(2, 500, "")};
    const Summary sum = run(s);

    const uint64_t sent = count(sum, "app_tx");
    EXPECT_GE(sent, 11u);
    EXPECT_EQ(count(sum, "app_rx"), sent);
    // the last message may still be waiting for its ACK when the run ends
    EXPECT_GE(count(sum, "app_ack"), sent - 1);
    EXPECT_EQ(count(sum, "app_ack_timeout"), 0u);
    EXPECT_EQ(count(sum, "app_tx_busy"), 0u);
    EXPECT_EQ(sum.lost_collision + sum.lost_half_duplex, 0u);
}

TEST(KervanStack, UnreachableDestinationTimesOut)
{
    // 1 and 3 are 3 km apart; node 2 hears both but does not relay (mesh is phase 4)
    Scenario s = base(120'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=3;send_every_ms=30000;ack=1;ack_timeout_ms=2000"),
               kervan_node(2, 1500, ""), kervan_node(3, 3000, "")};
    const Summary sum = run(s);

    EXPECT_GE(count(sum, "app_tx"), 3u);
    EXPECT_EQ(count(sum, "app_ack"), 0u);
    EXPECT_EQ(count(sum, "app_rx"), 0u);
    EXPECT_GE(count(sum, "app_ack_timeout"), 3u);
    // first attempt + 3 retries per message, all heard by node 2
    EXPECT_GE(sum.nodes[1].rx_ok, 4 * count(sum, "app_ack_timeout"));
}

TEST(KervanStack, RejectsBadConfig)
{
    for (const char *cfg : {"bogus=1", "addr=65535", "len=60", "ack=1", "send_to"}) {
        SCOPED_TRACE(cfg);
        Scenario s = base(1'000'000);
        s.nodes = {kervan_node(1, 0, cfg)};
        EXPECT_THROW(run(s), std::runtime_error);
    }
}

// Golden run: pins the complete behaviour (channel model, RNG, Kervan core). If this changes,
// the simulation results changed; update the constant only when that is intended.
TEST(KervanStack, GoldenLineScenario)
{
    const Scenario s = load_scenario_file(std::string(KSIM_SCENARIO_DIR) + "/line_5.json");
    const Summary a = run(s);
    const Summary b = run(s);
    EXPECT_EQ(a.trace_hash, b.trace_hash);
    EXPECT_GT(count(a, "app_ack"), 0u);
    EXPECT_EQ(a.trace_hash, 0x6c0ae003c6372e89ull) << std::hex << "actual: 0x" << a.trace_hash;
}

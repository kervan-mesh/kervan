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
    s.nodes = {kervan_node(1, 0, "send_to=2;send_every_ms=10000;qos=1"),
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
    EXPECT_EQ(count(sum, "kv_stats"), 2u); // one summary per node
}

// 1 and 3 are 3 km apart, node 2 sits in the middle and hears both.
TEST(KervanStack, WithoutRelayingTheDestinationIsUnreachable)
{
    Scenario s = base(120'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=3;send_every_ms=30000;qos=1;ack_timeout_ms=2000"),
               kervan_node(2, 1500, "relay=0"), kervan_node(3, 3000, "")};
    const Summary sum = run(s);

    EXPECT_GE(count(sum, "app_tx"), 3u);
    EXPECT_EQ(count(sum, "app_ack"), 0u);
    EXPECT_EQ(count(sum, "app_rx"), 0u);
    EXPECT_GE(count(sum, "app_ack_timeout"), 3u);
    EXPECT_GE(count(sum, "kv_retx"), 3u * count(sum, "app_ack_timeout"));
    EXPECT_EQ(count(sum, "kv_relay"), 0u);
}

TEST(KervanStack, RelayingReachesTheDestinationOverTwoHops)
{
    Scenario s = base(120'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=3;send_every_ms=30000;qos=1"),
               kervan_node(2, 1500, ""), kervan_node(3, 3000, "")};
    const Summary sum = run(s);

    const uint64_t sent = count(sum, "app_tx");
    EXPECT_GE(sent, 3u);
    EXPECT_GE(count(sum, "app_rx"), sent - 1);
    EXPECT_GE(count(sum, "app_ack"), sent - 1);
    EXPECT_EQ(count(sum, "app_ack_timeout"), 0u);
    // node 2 carries the message one way and the ACK the other
    EXPECT_GE(count(sum, "kv_relay"), 2u * (sent - 1));
}

TEST(KervanStack, LargeMessagesAreFragmented)
{
    Scenario s = base(300'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=2;send_every_ms=60000;qos=1;len=150"),
               kervan_node(2, 500, "")};
    const Summary sum = run(s);

    const uint64_t sent = count(sum, "app_tx");
    EXPECT_GE(sent, 5u);
    EXPECT_GE(count(sum, "app_rx"), sent - 1);
    EXPECT_GE(count(sum, "app_ack"), sent - 1);
    // 152 bytes over a 51-byte MTU: 4 fragments per message, each followed by ~10 s of
    // off-time at 1 % (SF7, 102 ms): the receiver's timeout restarts with every fragment
    EXPECT_GE(sum.nodes[0].tx, 4u * (sent - 1));
}

// A node that wants to send every second at SF7 would use ~5 % of the time; the budget keeps
// it at 1 % (plus at most one frame).
TEST(KervanStack, DutyCycleBudgetHoldsTheLimit)
{
    Scenario s = base(600'000'000);
    s.nodes = {kervan_node(1, 0, "send_to=2;send_every_ms=1000;len=40"),
               kervan_node(2, 500, "")};
    const Summary sum = run(s);

    const double duty = static_cast<double>(sum.nodes[0].airtime_us) /
                        static_cast<double>(sum.duration_us);
    EXPECT_LE(duty, 0.0101);
    EXPECT_GT(duty, 0.009);
    EXPECT_GT(count(sum, "kv_budget_wait"), 0u);
    EXPECT_GT(count(sum, "app_tx_busy"), 0u); // the queue fills up while waiting
}

TEST(KervanStack, RejectsBadConfig)
{
    for (const char *cfg : {"bogus=1", "addr=65535", "len=300", "qos=1", "qos=2", "send_to",
                            "relay=2", "duty_permille=1001", "send_to=1", "retries=x"}) {
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
    EXPECT_EQ(a.trace_hash, 0x7475269e53d90d42ull) << std::hex << "actual: 0x" << a.trace_hash;
}

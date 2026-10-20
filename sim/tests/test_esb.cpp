/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// nRF24L01+ Enhanced ShockBurst model against the rules in docs/nrf24-esb.md.

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;
using nlohmann::json;

namespace {

// Path loss 40 dB at 1 m, exponent 3; 0 dBm: RSSI(d) = -40 - 30 log10(d).
// Sensitivity -85 dBm at 1 Mbps reaches about 31 m; -94 dBm at 250 kbps about 63 m.
Scenario base(Time duration_us)
{
    Scenario s;
    s.duration_us = duration_us;
    s.path_loss.exponent = 3.0;
    s.path_loss.ref_loss_db = 40.0;
    return s;
}

NodeSpec esb(uint32_t id, double x, const std::string &config, uint32_t rate = 1000000)
{
    NodeSpec n;
    n.id = id;
    n.pos.x = x;
    n.stack = "nrf24-esb";
    n.config = config;
    n.radio.type = PhyType::Nrf24;
    n.radio.tx_power_dbm = 0.0;
    n.radio.nrf24.data_rate = rate;
    return n;
}

struct Outcome {
    Summary sum;
    std::vector<json> trace;

    uint64_t count(const std::string &name) const
    {
        auto it = sum.app_events.find(name);
        return it == sum.app_events.end() ? 0u : it->second;
    }

    // app events of one node
    uint64_t count(uint32_t node, const std::string &name) const
    {
        uint64_t n = 0;
        for (const json &j : trace) {
            n += j["ev"] == "app" && j["node"] == node && j["name"] == name ? 1u : 0u;
        }
        return n;
    }

    std::vector<json> events(const std::string &ev, uint32_t node) const
    {
        std::vector<json> out;
        for (const json &j : trace) {
            if (j["ev"] == ev && j["node"] == node) {
                out.push_back(j);
            }
        }
        return out;
    }
};

Outcome run(const Scenario &s)
{
    Simulator sim(s);
    sim.register_stack(ksim_stack_nrf24_esb());
    sim.register_stack(ksim_stack_kervan());
    std::ostringstream out;
    sim.set_trace(&out);
    Outcome r;
    r.sum = sim.run();
    std::istringstream lines(out.str());
    std::string line;
    while (std::getline(lines, line)) {
        r.trace.push_back(json::parse(line));
    }
    return r;
}

const std::string kTo2 = "send_to=0xE7E7E7E702;listen=0;";

} // namespace

TEST(Esb, AcknowledgedDelivery)
{
    Scenario s = base(2'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100"), esb(2, 10, "")};
    const Outcome r = run(s);

    const uint64_t sent = r.count("esb_tx");
    EXPECT_GE(sent, 19u);
    EXPECT_GE(r.count("esb_tx_ds"), sent - 1);
    EXPECT_GE(r.count("esb_rx"), sent - 1);
    EXPECT_EQ(r.count("esb_max_rt"), 0u);
    EXPECT_EQ(r.count("esb_retx"), 0u);
    EXPECT_EQ(r.count("esb_rx_dup"), 0u);
}

TEST(Esb, TimingFollowsTheDatasheet)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=1000"), esb(2, 10, "")};
    const Outcome r = run(s);

    const auto app = r.events("app", 1);
    const auto ptx = r.events("tx", 1);
    const auto prx = r.events("tx", 2);
    ASSERT_EQ(ptx.size(), 1u);
    ASSERT_EQ(prx.size(), 1u);
    ASSERT_GE(app.size(), 2u);

    const uint64_t t0 = app[0]["t"]; // esb_tx: leaves RX, settles
    EXPECT_EQ(app[0]["name"], "esb_tx");
    EXPECT_EQ(ptx[0]["t"], t0 + 130);
    EXPECT_EQ(ptx[0]["len"], 5 + 2 + 8);
    EXPECT_EQ(ptx[0]["airtime"], 137); // 8 + 40 + 9 + 64 + 16 bits at 1 Mbps
    const uint64_t frame_end = t0 + 130 + 137;
    EXPECT_EQ(prx[0]["t"], frame_end + 130); // auto-ACK after the turnaround
    EXPECT_EQ(prx[0]["len"], 5 + 2);
    EXPECT_EQ(prx[0]["airtime"], 73);
    EXPECT_EQ(app[1]["name"], "esb_tx_ds");
    EXPECT_EQ(app[1]["t"], frame_end + 130 + 73);
}

TEST(Esb, NoAckNeedsNoAnswer)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100;no_ack=1"), esb(2, 10, "")};
    const Outcome r = run(s);
    EXPECT_EQ(r.count("esb_tx_ds"), r.count("esb_tx"));
    EXPECT_EQ(r.sum.nodes[1].tx, 0u); // the receiver never transmits
    EXPECT_GE(r.count("esb_rx"), r.count("esb_tx") - 1);
}

// Datasheet: at 250 kbps ARD must be at least 500 us, even without ACK payload.
TEST(Esb, Ard250IsTooShortAt250kbps)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100;ard_us=250", 250000),
               esb(2, 10, "", 250000)};
    const Outcome r = run(s);
    const uint64_t sent = r.count("esb_tx");
    EXPECT_GE(sent, 9u);
    EXPECT_EQ(r.count("esb_tx_ds"), 0u);
    EXPECT_GE(r.count("esb_max_rt"), sent - 1);
    EXPECT_GE(r.count("esb_retx"), 3 * (sent - 1));
    // every payload still reaches the receiver once
    EXPECT_GE(r.count("esb_rx"), sent - 1);
    EXPECT_LE(r.count("esb_rx"), sent);
    // The first retransmission (frame end + 250 us) starts while the receiver is still sending
    // its ACK (frame end + 130..422 us) and is missed; the next one arrives as a duplicate.
    EXPECT_GE(r.count("esb_rx_dup"), sent - 1);
    EXPECT_GE(r.sum.lost_rx_off + r.sum.lost_half_duplex, 2 * (sent - 1));
}

TEST(Esb, Ard500WorksAt250kbps)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100;ard_us=500", 250000),
               esb(2, 10, "", 250000)};
    const Outcome r = run(s);
    EXPECT_GE(r.count("esb_tx_ds"), r.count("esb_tx") - 1);
    EXPECT_EQ(r.count("esb_max_rt"), 0u);
}

// With ARD = 250 us at 1 Mbps, an ACK payload of up to 5 bytes fits, 6 bytes do not.
TEST(Esb, AckPayloadMustFitTheArd)
{
    for (int len : {5, 6}) {
        SCOPED_TRACE(len);
        Scenario s = base(1'000'000);
        s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100;ard_us=250"),
                   esb(2, 10, "ack_payload_len=" + std::to_string(len))};
        const Outcome r = run(s);
        const uint64_t sent = r.count("esb_tx");
        if (len == 5) {
            EXPECT_GE(r.count("esb_tx_ds"), sent - 1);
            EXPECT_EQ(r.count("esb_ack_payload"), r.count("esb_tx_ds"));
        } else {
            EXPECT_EQ(r.count("esb_tx_ds"), 0u);
            EXPECT_GE(r.count("esb_max_rt"), sent - 1);
        }
    }
}

TEST(Esb, OutOfRangeRetriesArcTimesThenMaxRt)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, kTo2 + "send_every_ms=100;arc=2"), esb(2, 1000, "")};
    const Outcome r = run(s);
    const uint64_t failed = r.count("esb_max_rt");
    EXPECT_GE(failed, 9u);
    EXPECT_EQ(r.count("esb_tx_ds"), 0u);
    EXPECT_GE(r.count("esb_retx"), 2 * failed);
    EXPECT_GE(r.sum.nodes[0].tx, 3 * failed); // first attempt + 2 retransmissions each
}

TEST(Esb, PipesShareUpperAddressBytes)
{
    Scenario s = base(1'000'000);
    s.nodes = {esb(1, 0, "send_to=0xE7E7E7E702;listen=0;send_every_ms=97"),
               esb(2, 10, "pipes=2"), // listens on ...02 and ...03
               esb(3, 5, "send_to=0xE7E7E7E703;listen=0;send_every_ms=101"),
               esb(4, 5, "send_to=0xE7E7E7E704;listen=0;send_every_ms=103")};
    const Outcome r = run(s);

    EXPECT_GE(r.count(1, "esb_tx_ds"), 1u);
    EXPECT_GE(r.count(3, "esb_tx_ds"), 1u);
    EXPECT_EQ(r.count(4, "esb_tx_ds"), 0u); // nobody listens on ...04
    EXPECT_GE(r.count(4, "esb_max_rt"), 1u);
    bool pipe1 = false;
    bool pipe2 = false;
    for (const json &j : r.events("app", 2)) {
        const std::string d = j["detail"];
        pipe1 = pipe1 || d.rfind("pipe=1 ", 0) == 0;
        pipe2 = pipe2 || d.rfind("pipe=2 ", 0) == 0;
    }
    EXPECT_TRUE(pipe1);
    EXPECT_TRUE(pipe2);
}

TEST(Esb, StacksCheckTheRadioType)
{
    Scenario s = base(1'000'000);
    NodeSpec lora = esb(1, 0, "");
    lora.radio = RadioConfig{}; // LoRa radio with the ESB stack
    s.nodes = {lora};
    EXPECT_THROW(run(s), std::runtime_error);

    NodeSpec kv = esb(1, 0, "");
    kv.stack = "kervan"; // Kervan stack on an nRF24 radio
    s.nodes = {kv};
    EXPECT_THROW(run(s), std::runtime_error);
}

TEST(Esb, RejectsBadConfig)
{
    for (const char *cfg : {"ard_us=300", "ard_us=4250", "arc=16", "len=33", "aw=6", "aw=2",
                            "pipes=6", "send_every_ms=10", "foo=1", "ack_payload_len=33",
                            "listen=2", "addr"}) {
        SCOPED_TRACE(cfg);
        Scenario s = base(1'000'000);
        s.nodes = {esb(1, 0, cfg)};
        EXPECT_THROW(run(s), std::runtime_error);
    }
}

// Golden run: pins the complete nRF24 behaviour. Update only for intended changes.
TEST(Esb, GoldenStarScenario)
{
    const Scenario s = load_scenario_file(std::string(KSIM_SCENARIO_DIR) + "/nrf24_star.json");
    const Outcome a = run(s);
    const Outcome b = run(s);
    EXPECT_EQ(a.sum.trace_hash, b.sum.trace_hash);
    EXPECT_GT(a.count("esb_tx_ds"), 0u);
    EXPECT_EQ(a.sum.trace_hash, 0xbd578facf1580370ull) << std::hex << "actual: 0x"
                                                       << a.sum.trace_hash;
}

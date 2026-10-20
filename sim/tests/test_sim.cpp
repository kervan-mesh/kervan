/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Channel rules end to end, driven by a minimal scripted stack.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ksim/simulator.hpp"

using namespace ksim;

namespace {

// ---- scripted test stack ----
// config: send_at=<us>  len=<bytes>  twice=1 (send again while busy)
//         timers=1 (timer self-test)  reject=1 (fail create)
//         rx_off_at=<us>  rx_on_at=<us>  retune_at=<us> (set SF 7 again)
//         probe=1 (radio_get/radio_set calls at create and while transmitting)

struct RxRec {
    Time t;
    uint32_t from; // first payload byte = sender id
    size_t len;
    double rssi;
    double snr;
};

struct Rec {
    std::vector<RxRec> rx;
    int tx_done = 0;
    int send_result = 999;
    int second_send_result = 999;
    std::vector<std::pair<uint32_t, Time>> timers;
    std::map<std::string, int> probe; // name -> radio_get/radio_set result
    std::map<std::string, int64_t> probe_value;
};

std::map<uint32_t, Rec> g_rec;

struct Inst {
    ksim_host_t host;
    size_t len = 10;
    bool twice = false;
    bool probe = false;
};

constexpr uint32_t kSendTimer = 100;
constexpr uint32_t kRxOffTimer = 101;
constexpr uint32_t kRxOnTimer = 102;
constexpr uint32_t kRetuneTimer = 103;

void probe_at_create(const ksim_host_t *h, Rec &rec)
{
    int64_t v = 0;
    rec.probe["get_phy"] = h->radio_get(h->ctx, KSIM_RADIO_PHY, &v);
    rec.probe_value["phy"] = v;
    rec.probe["get_sf"] = h->radio_get(h->ctx, KSIM_RADIO_LORA_SF, &v);
    rec.probe_value["sf"] = v;
    rec.probe["get_power"] = h->radio_get(h->ctx, KSIM_RADIO_TX_POWER_CDBM, &v);
    rec.probe_value["power"] = v;
    rec.probe["get_nrf_channel"] = h->radio_get(h->ctx, KSIM_RADIO_NRF24_CHANNEL, &v);
    rec.probe["set_phy"] = h->radio_set(h->ctx, KSIM_RADIO_PHY, KSIM_PHY_NRF24);
    rec.probe["set_sf_13"] = h->radio_set(h->ctx, KSIM_RADIO_LORA_SF, 13);
    rec.probe["set_sf_9"] = h->radio_set(h->ctx, KSIM_RADIO_LORA_SF, 9);
    rec.probe["set_power_25dbm"] = h->radio_set(h->ctx, KSIM_RADIO_TX_POWER_CDBM, 2500);
    rec.probe["set_power_20dbm"] = h->radio_set(h->ctx, KSIM_RADIO_TX_POWER_CDBM, 2000);
    rec.probe["set_freq_2g4"] = h->radio_set(h->ctx, KSIM_RADIO_FREQ_HZ, 2400000000);
    rec.probe["set_nrf_channel"] = h->radio_set(h->ctx, KSIM_RADIO_NRF24_CHANNEL, 5);
    h->radio_get(h->ctx, KSIM_RADIO_TX_POWER_CDBM, &v);
    rec.probe_value["power_after"] = v;
    h->radio_get(h->ctx, KSIM_RADIO_LORA_SF, &v);
    rec.probe_value["sf_after"] = v;
}

std::map<std::string, std::string> parse(const char *config)
{
    std::map<std::string, std::string> out;
    std::stringstream ss(config);
    std::string item;
    while (std::getline(ss, item, ';')) {
        const auto eq = item.find('=');
        out[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return out;
}

void *t_create(const ksim_host_t *host, const char *config)
{
    auto cfg = parse(config);
    if (cfg.count("reject") != 0) {
        return nullptr;
    }
    auto *inst = new Inst{*host};
    g_rec[host->node_id] = Rec{};
    if (cfg.count("len") != 0) {
        inst->len = std::stoul(cfg["len"]);
    }
    inst->twice = cfg.count("twice") != 0;
    if (cfg.count("send_at") != 0) {
        host->timer_set(host->ctx, kSendTimer, std::stoull(cfg["send_at"]));
    }
    if (cfg.count("timers") != 0) {
        host->timer_set(host->ctx, 1, 500);
        host->timer_set(host->ctx, 2, 300);
        host->timer_set(host->ctx, 1, 700); // replaces 500
        host->timer_set(host->ctx, 3, 400);
        host->timer_cancel(host->ctx, 3);
    }
    if (cfg.count("rx_off_at") != 0) {
        host->timer_set(host->ctx, kRxOffTimer, std::stoull(cfg["rx_off_at"]));
    }
    if (cfg.count("rx_on_at") != 0) {
        host->timer_set(host->ctx, kRxOnTimer, std::stoull(cfg["rx_on_at"]));
    }
    if (cfg.count("retune_at") != 0) {
        host->timer_set(host->ctx, kRetuneTimer, std::stoull(cfg["retune_at"]));
    }
    inst->probe = cfg.count("probe") != 0;
    if (inst->probe) {
        probe_at_create(host, g_rec[host->node_id]);
    }
    return inst;
}

void t_destroy(void *inst)
{
    delete static_cast<Inst *>(inst);
}

void t_on_rx(void *p, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    auto *inst = static_cast<Inst *>(p);
    g_rec[inst->host.node_id].rx.push_back(
        {inst->host.now_us(inst->host.ctx), buf[0], len, meta->rssi_dbm, meta->snr_db});
}

void t_on_tx_done(void *p)
{
    auto *inst = static_cast<Inst *>(p);
    g_rec[inst->host.node_id].tx_done++;
}

void t_on_timer(void *p, uint32_t id)
{
    auto *inst = static_cast<Inst *>(p);
    Rec &rec = g_rec[inst->host.node_id];
    const ksim_host_t &h = inst->host;
    if (id == kRxOffTimer || id == kRxOnTimer) {
        h.radio_set(h.ctx, KSIM_RADIO_RX_ENABLED, id == kRxOnTimer ? 1 : 0);
        return;
    }
    if (id == kRetuneTimer) {
        h.radio_set(h.ctx, KSIM_RADIO_LORA_SF, 7);
        return;
    }
    if (id != kSendTimer) {
        rec.timers.emplace_back(id, inst->host.now_us(inst->host.ctx));
        return;
    }
    std::vector<uint8_t> data(inst->len, static_cast<uint8_t>(inst->host.node_id));
    rec.send_result = inst->host.radio_send(inst->host.ctx, data.data(), data.size());
    if (inst->twice) {
        rec.second_send_result = inst->host.radio_send(inst->host.ctx, data.data(), data.size());
    }
    if (inst->probe) {
        rec.probe["set_while_tx"] = h.radio_set(h.ctx, KSIM_RADIO_LORA_SF, 7);
    }
}

const ksim_stack_t kTestStack = {KSIM_ABI_VERSION, "test", "1",          t_create,
                                 t_destroy,        t_on_rx, t_on_tx_done, t_on_timer,
                                 nullptr};

// ---- scenario helpers ----
// Path loss 40 dB at 1 m, exponent 3, 14 dBm: RSSI(d) = -26 - 30 log10(d).
// Noise floor at 125 kHz is -117.03 dBm, so SF7 (-7.5 dB) reaches about 1.9 km.

Scenario base()
{
    Scenario s;
    s.duration_us = 10'000'000;
    s.path_loss.exponent = 3.0;
    s.path_loss.ref_distance_m = 1.0;
    s.path_loss.ref_loss_db = 40.0;
    return s;
}

NodeSpec node(uint32_t id, double x, const std::string &config = "", int sf = 7)
{
    NodeSpec n;
    n.id = id;
    n.pos.x = x;
    n.stack = "test";
    n.config = config;
    n.radio.lora.sf = sf;
    return n;
}

Summary run(const Scenario &s, std::ostream *trace = nullptr)
{
    Simulator sim(s);
    sim.register_stack(&kTestStack);
    sim.set_trace(trace);
    return sim.run();
}

constexpr Time kAirtime10 = 41216; // SF7, 10 bytes

class Sim : public ::testing::Test {
protected:
    void SetUp() override { g_rec.clear(); }
};

} // namespace

TEST_F(Sim, InRangeFrameIsDeliveredAfterAirtime)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100)};
    const Summary sum = run(s);

    ASSERT_EQ(g_rec[2].rx.size(), 1u);
    const RxRec &rx = g_rec[2].rx[0];
    EXPECT_EQ(rx.t, 1000 + kAirtime10);
    EXPECT_EQ(rx.from, 1u);
    EXPECT_EQ(rx.len, 10u);
    EXPECT_DOUBLE_EQ(rx.rssi, -86.0);
    EXPECT_DOUBLE_EQ(rx.snr, 31.03);
    EXPECT_EQ(g_rec[1].send_result, KSIM_OK);
    EXPECT_EQ(g_rec[1].tx_done, 1);
    EXPECT_EQ(sum.tx, 1u);
    EXPECT_EQ(sum.rx_ok, 1u);
    EXPECT_EQ(sum.nodes[0].airtime_us, kAirtime10);
}

TEST_F(Sim, OutOfRangeIsSilent)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 3000)};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.rx_ok + sum.lost_collision + sum.lost_half_duplex, 0u);
}

TEST_F(Sim, HigherSfReachesFurther)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000", 12), node(2, 3000, "", 12)};
    run(s);
    EXPECT_EQ(g_rec[2].rx.size(), 1u); // SF12 floor is -20 dB: about 5.5 km here
}

TEST_F(Sim, EqualPowerCollisionLosesBoth)
{
    Scenario s = base();
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), node(3, 100, "send_at=1000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.nodes[1].lost_collision, 2u);
}

TEST_F(Sim, PartialOverlapAlsoCollides)
{
    Scenario s = base();
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), node(3, 100, "send_at=21000")};
    run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
}

TEST_F(Sim, BackToBackFramesDoNotCollide)
{
    Scenario s = base();
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0),
               node(3, 100, "send_at=" + std::to_string(1000 + kAirtime10))};
    run(s);
    EXPECT_EQ(g_rec[2].rx.size(), 2u);
}

TEST_F(Sim, StrongerFrameIsCaptured)
{
    Scenario s = base();
    // at the receiver: node 1 -86 dBm, node 3 -116 dBm (30 dB weaker, still above sensitivity)
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), node(3, 1000, "send_at=1000")};
    const Summary sum = run(s);
    ASSERT_EQ(g_rec[2].rx.size(), 1u);
    EXPECT_EQ(g_rec[2].rx[0].from, 1u);
    EXPECT_EQ(sum.nodes[1].lost_collision, 1u);
}

TEST_F(Sim, CaptureThresholdIsConfigurable)
{
    Scenario s = base();
    s.capture_threshold_db = 40.0; // 30 dB stronger is no longer enough
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), node(3, 1000, "send_at=1000")};
    run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
}

TEST_F(Sim, DifferentSfDoesNotInterfere)
{
    Scenario s = base();
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), node(3, 100, "send_at=1000", 9)};
    const Summary sum = run(s);
    ASSERT_EQ(g_rec[2].rx.size(), 1u); // listens on SF7 only
    EXPECT_EQ(g_rec[2].rx[0].from, 1u);
    EXPECT_EQ(sum.lost_collision, 0u);
}

TEST_F(Sim, DifferentFrequencyDoesNotInterfere)
{
    Scenario s = base();
    NodeSpec other = node(3, 100, "send_at=1000");
    other.radio.lora.freq_hz = 868300000;
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), other};
    run(s);
    ASSERT_EQ(g_rec[2].rx.size(), 1u);
    EXPECT_EQ(g_rec[2].rx[0].from, 1u);
}

TEST_F(Sim, HalfDuplexLosesFramesBothWays)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "send_at=11000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[1].rx.empty());
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.lost_half_duplex, 2u);
}

TEST_F(Sim, SendWhileBusyAndInvalidLength)
{
    Scenario s = base();
    s.mtu = 51;
    s.nodes = {node(1, 0, "send_at=1000;twice=1"), node(2, 100, "send_at=1000;len=52")};
    run(s);
    EXPECT_EQ(g_rec[1].send_result, KSIM_OK);
    EXPECT_EQ(g_rec[1].second_send_result, KSIM_ERR_BUSY);
    EXPECT_EQ(g_rec[2].send_result, KSIM_ERR_INVALID);
}

TEST_F(Sim, TimersFireReplaceAndCancel)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "timers=1")};
    run(s);
    const std::vector<std::pair<uint32_t, Time>> expected{{2, 300}, {1, 700}};
    EXPECT_EQ(g_rec[1].timers, expected);
}

TEST_F(Sim, SameSeedSameTraceDifferentSeedDifferentTrace)
{
    Scenario s = base();
    s.path_loss.shadowing_sigma_db = 6.0;
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 900), node(3, 1800), node(4, 2500)};
    std::ostringstream t1;
    std::ostringstream t2;
    const Summary a = run(s, &t1);
    const Summary b = run(s, &t2);
    EXPECT_EQ(a.trace_hash, b.trace_hash);
    EXPECT_EQ(t1.str(), t2.str());
    EXPECT_FALSE(t1.str().empty());

    s.seed = 2;
    EXPECT_NE(run(s).trace_hash, a.trace_hash);
}

TEST_F(Sim, ShadowingIsPerLinkSymmetricAndStable)
{
    Scenario s = base();
    s.path_loss.shadowing_sigma_db = 6.0;
    s.nodes = {node(1, 0), node(2, 500)};
    const double before = Simulator(s).rssi_dbm(1, 2);
    EXPECT_NE(before, -26.0 - 30.0 * std::log10(500.0)); // shadowing applied
    EXPECT_DOUBLE_EQ(Simulator(s).rssi_dbm(2, 1), before);

    s.nodes.push_back(node(3, 250)); // another node must not change this link
    EXPECT_DOUBLE_EQ(Simulator(s).rssi_dbm(1, 2), before);
}

TEST_F(Sim, ConfigurationErrors)
{
    Scenario s = base();
    s.nodes = {node(1, 0), node(1, 10)};
    EXPECT_THROW(Simulator{s}, std::invalid_argument);

    s.nodes = {node(1, 0, "", 6)};
    EXPECT_THROW(Simulator{s}, std::invalid_argument);

    s.nodes = {node(1, 0)};
    s.nodes[0].stack = "nope";
    EXPECT_THROW(run(s), std::runtime_error);

    s.nodes = {node(1, 0, "reject=1")};
    EXPECT_THROW(run(s), std::runtime_error);

    Simulator sim(base());
    ksim_stack_t old = kTestStack;
    old.abi_version = 0;
    EXPECT_THROW(sim.register_stack(&old), std::invalid_argument);
    sim.register_stack(&kTestStack);
    EXPECT_THROW(sim.register_stack(&kTestStack), std::invalid_argument);
}

TEST_F(Sim, TraceLinesAreJson)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100)};
    std::ostringstream trace;
    run(s, &trace);
    EXPECT_EQ(trace.str(),
              "{\"t\":0,\"ev\":\"meta\",\"ksim\":\"0.1.0\",\"abi\":3,\"phy\":{\"lora\":\"1\"},"
              "\"stack\":{\"test\":\"1\"},\"device\":{}}\n"
              "{\"t\":1000,\"ev\":\"tx\",\"node\":1,\"len\":10,\"airtime\":41216}\n"
              "{\"t\":42216,\"ev\":\"rx\",\"node\":2,\"from\":1,\"rssi\":-86.0,\"snr\":31.03}\n");
}

TEST_F(Sim, ReceiverOffDuringFrameMissesIt)
{
    Scenario s = base();
    // frame 1 arrives 1000..42216; node 2 switches RX off and on again in the middle
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "rx_off_at=10000;rx_on_at=20000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.lost_rx_off, 1u);
    EXPECT_EQ(sum.nodes[1].lost_rx_off, 1u);
}

TEST_F(Sim, ReceiverBackOnBeforeFrameStartsHearsIt)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "rx_off_at=100;rx_on_at=1000")};
    run(s);
    EXPECT_EQ(g_rec[2].rx.size(), 1u);
}

TEST_F(Sim, ReceiverOffAtTheEndMissesIt)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "rx_off_at=30000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.lost_rx_off, 1u);
}

TEST_F(Sim, RetuningDuringFrameMissesIt)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "retune_at=10000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.lost_rx_off, 1u);
}

TEST_F(Sim, RadioGetSetRespectPhyAndDevice)
{
    Scenario s = base();
    NodeSpec n = node(1, 0, "probe=1;send_at=1000");
    n.device = *DeviceRegistry::builtin().find("esp32-sx1276"); // 2..20 dBm, 137-1020 MHz
    s.nodes = {n};
    run(s);

    const Rec &r = g_rec[1];
    EXPECT_EQ(r.probe.at("get_phy"), KSIM_OK);
    EXPECT_EQ(r.probe_value.at("phy"), KSIM_PHY_LORA);
    EXPECT_EQ(r.probe_value.at("sf"), 7);
    EXPECT_EQ(r.probe_value.at("power"), 1400);
    EXPECT_EQ(r.probe.at("get_nrf_channel"), KSIM_ERR_UNSUPPORTED);
    EXPECT_EQ(r.probe.at("set_phy"), KSIM_ERR_UNSUPPORTED);
    EXPECT_EQ(r.probe.at("set_sf_13"), KSIM_ERR_INVALID);
    EXPECT_EQ(r.probe.at("set_sf_9"), KSIM_OK);
    EXPECT_EQ(r.probe.at("set_power_25dbm"), KSIM_ERR_INVALID); // device limit
    EXPECT_EQ(r.probe.at("set_power_20dbm"), KSIM_OK);
    EXPECT_EQ(r.probe.at("set_freq_2g4"), KSIM_ERR_INVALID); // outside the device's band
    EXPECT_EQ(r.probe.at("set_nrf_channel"), KSIM_ERR_UNSUPPORTED);
    EXPECT_EQ(r.probe_value.at("power_after"), 2000);
    EXPECT_EQ(r.probe_value.at("sf_after"), 9);
    EXPECT_EQ(r.probe.at("set_while_tx"), KSIM_ERR_BUSY);
}

TEST_F(Sim, TxPowerChangesTheLinkBudget)
{
    Scenario s = base();
    NodeSpec n = node(1, 0, "send_at=1000");
    n.radio.tx_power_dbm = 4.0; // 10 dB less than the default
    s.nodes = {n, node(2, 100)};
    run(s);
    ASSERT_EQ(g_rec[2].rx.size(), 1u);
    EXPECT_DOUBLE_EQ(g_rec[2].rx[0].rssi, -96.0);
}

TEST_F(Sim, DifferentPhysNeverInteract)
{
    Scenario s = base();
    NodeSpec nrf = node(3, 100, "send_at=1000");
    nrf.radio = RadioConfig{};
    nrf.radio.type = PhyType::Nrf24;
    nrf.radio.tx_power_dbm = 0.0;
    s.nodes = {node(1, -100, "send_at=1000"), node(2, 0), nrf};
    const Summary sum = run(s);
    ASSERT_EQ(g_rec[2].rx.size(), 1u); // the LoRa frame survives the nRF24 one
    EXPECT_EQ(sum.lost_collision, 0u);
    EXPECT_EQ(sum.versions.at("phy:nrf24"), "1");
    EXPECT_EQ(sum.versions.at("phy:lora"), "1");
}

TEST_F(Sim, ReceiverOffForTheWholeFrameIsSilent)
{
    Scenario s = base();
    s.nodes = {node(1, 0, "send_at=1000"), node(2, 100, "rx_off_at=500;rx_on_at=50000")};
    const Summary sum = run(s);
    EXPECT_TRUE(g_rec[2].rx.empty());
    EXPECT_EQ(sum.lost_rx_off, 0u); // not listening is not a loss
}

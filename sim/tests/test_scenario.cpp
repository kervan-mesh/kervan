/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "ksim/scenario.hpp"

using ksim::parse_scenario;

namespace {

// Expect parse_scenario to fail with a message containing `needle`.
void expect_error(const std::string &json, const std::string &needle)
{
    try {
        parse_scenario(json);
        ADD_FAILURE() << "no error for: " << json;
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
            << "message: " << e.what();
    }
}

const char *kMinimal = R"({"nodes": [{"id": 1, "pos": [0, 0], "stack": "kervan"}]})";

} // namespace

TEST(Scenario, MinimalUsesDefaults)
{
    const ksim::Scenario s = parse_scenario(kMinimal);
    EXPECT_EQ(s.seed, 1u);
    EXPECT_EQ(s.duration_us, 60'000'000u);
    EXPECT_EQ(s.mtu, 255u);
    ASSERT_EQ(s.nodes.size(), 1u);
    EXPECT_EQ(s.nodes[0].radio.sf, 7);
    EXPECT_EQ(s.nodes[0].config, "");
    EXPECT_FALSE(s.path_loss.ref_loss_db.has_value());
}

TEST(Scenario, FullFile)
{
    const ksim::Scenario s = parse_scenario(R"({
        "description": "test",
        "seed": 7, "duration_s": 1.5, "mtu": 51,
        "radio": {"type": "lora", "sf": 9, "bw_hz": 250000, "tx_power_dbm": 20},
        "path_loss": {"model": "log_distance", "exponent": 3.1, "ref_distance_m": 10,
                      "ref_loss_db": 70, "shadowing_sigma_db": 4},
        "capture_threshold_db": 3, "noise_figure_db": 5, "duty_cycle_limit": 0.1,
        "nodes": [
            {"id": 1, "pos": [1, 2, 3], "stack": "kervan",
             "config": {"send_to": 2, "ack": true, "len": 12}},
            {"id": 2, "pos": [0, 0], "stack": "kervan", "radio": {"sf": 12}}
        ]})");
    EXPECT_EQ(s.seed, 7u);
    EXPECT_EQ(s.duration_us, 1'500'000u);
    EXPECT_EQ(s.mtu, 51u);
    EXPECT_DOUBLE_EQ(s.path_loss.exponent, 3.1);
    EXPECT_DOUBLE_EQ(*s.path_loss.ref_loss_db, 70.0);
    EXPECT_DOUBLE_EQ(s.capture_threshold_db, 3.0);
    EXPECT_DOUBLE_EQ(s.nodes[0].pos.z, 3.0);
    EXPECT_EQ(s.nodes[0].radio.sf, 9);
    EXPECT_EQ(s.nodes[0].radio.bw_hz, 250000u);
    EXPECT_EQ(s.nodes[0].config, "ack=1;len=12;send_to=2"); // sorted keys
    EXPECT_EQ(s.nodes[1].radio.sf, 12);                     // override
    EXPECT_DOUBLE_EQ(s.nodes[1].radio.tx_power_dbm, 20.0);  // rest inherited
}

TEST(Scenario, ErrorsPointAtTheProblem)
{
    expect_error("{", "invalid JSON");
    expect_error(R"({"seed": 1})", "scenario.nodes");
    expect_error(R"({"sedd": 1, "nodes": []})", "unknown key 'sedd'");
    expect_error(R"({"radio": {"sff": 7}, "nodes": []})", "scenario.radio: unknown key 'sff'");
    expect_error(R"({"radio": {"sf": 13}, "nodes": []})", "sf must be 7..12");
    expect_error(R"({"seed": -1, "nodes": []})", "scenario.seed");
    expect_error(R"({"seed": 1.5, "nodes": []})", "scenario.seed");
    expect_error(R"({"duration_s": 0, "nodes": []})", "duration_s");
    expect_error(R"({"mtu": 300, "nodes": []})", "at most 255");
    expect_error(R"({"nodes": [{"id": 1, "stack": "kervan"}]})", "needs \"id\", \"pos\"");
    expect_error(R"({"nodes": [{"id": 1, "pos": [0], "stack": "kervan"}]})", "nodes[0].pos");
    expect_error(R"({"nodes": [{"id": 1, "pos": [0, 0], "stack": "kervan"},
                               {"id": 1, "pos": [0, 0], "stack": "kervan"}]})",
                 "duplicate id 1");
    expect_error(R"({"nodes": [{"id": 1, "pos": [0, 0], "stack": "kervan",
                                "config": {"a": "x;y"}}]})",
                 "must not contain");
    expect_error(R"({"path_loss": {"model": "hata"}, "nodes": []})", "log_distance");
}

TEST(Scenario, ShippedScenariosParse)
{
    for (const char *name : {"two_nodes.json", "line_5.json", "star_collisions.json"}) {
        SCOPED_TRACE(name);
        EXPECT_NO_THROW(ksim::load_scenario_file(std::string(KSIM_SCENARIO_DIR) + "/" + name));
    }
    EXPECT_THROW(ksim::load_scenario_file("does/not/exist.json"), std::runtime_error);
}

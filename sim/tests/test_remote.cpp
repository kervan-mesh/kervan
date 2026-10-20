/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Out-of-process stacks (docs/remote-protocol.md). The key property: a stack produces the same
// trace in process and out of process.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include "ksim/remote.h"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;

namespace {

const std::string kScenarios = KSIM_SCENARIO_DIR;

Summary run(const Scenario &s, std::function<void(uint32_t, uint16_t)> on_wait = {})
{
    Simulator sim(s);
    sim.register_stack(ksim_stack_kervan());
    sim.register_stack(ksim_stack_nrf24_esb());
    sim.set_remote_options(0, "", std::move(on_wait));
    return sim.run();
}

ProcessSpec spawn(std::vector<std::string> argv, double timeout_s = 0.0)
{
    ProcessSpec p;
    p.spawn = std::move(argv);
    p.timeout_s = timeout_s;
    return p;
}

// Move the selected nodes (all if empty) into ksim-node processes.
Scenario out_of_process(Scenario s, const std::vector<uint32_t> &ids = {})
{
    for (NodeSpec &n : s.nodes) {
        if (ids.empty() || std::find(ids.begin(), ids.end(), n.id) != ids.end()) {
            n.process = spawn({KSIM_NODE_EXE, "--stack", n.stack});
        }
    }
    return s;
}

// Expect run(s) to throw with `needle` in the message.
void expect_failure(const Scenario &s, const std::string &needle)
{
    try {
        run(s);
        ADD_FAILURE() << "the run did not fail";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
            << "message: " << e.what();
    }
}

Scenario one_kervan_node()
{
    Scenario s;
    s.duration_us = 1'000'000;
    NodeSpec n;
    n.id = 1;
    n.stack = "kervan";
    s.nodes = {n};
    return s;
}

} // namespace

TEST(Remote, KervanOutOfProcessGivesTheSameTrace)
{
    const Scenario s = load_scenario_file(kScenarios + "/line_5.json");
    const Summary in = run(s);
    const Summary out = run(out_of_process(s));
    EXPECT_EQ(out.trace_hash, in.trace_hash);
    EXPECT_EQ(out.app_events, in.app_events);
    EXPECT_EQ(out.versions, in.versions);
}

TEST(Remote, EsbOutOfProcessGivesTheSameTrace)
{
    const Scenario s = load_scenario_file(kScenarios + "/nrf24_star.json");
    EXPECT_EQ(run(out_of_process(s)).trace_hash, run(s).trace_hash);
}

TEST(Remote, MixedInAndOutOfProcess)
{
    const Scenario s = load_scenario_file(kScenarios + "/line_5.json");
    EXPECT_EQ(run(out_of_process(s, {2, 4})).trace_hash, run(s).trace_hash);
}

// "wait" mode: ksim waits for a process started by someone else (here: a thread of this test).
TEST(Remote, WaitModeAcceptsAnExternalProcess)
{
    Scenario s = load_scenario_file(kScenarios + "/two_nodes.json");
    const Summary in = run(s);

    ProcessSpec wait;
    wait.wait = true;
    wait.timeout_s = 20;
    s.nodes[1].process = wait;
    std::thread client;
    int client_result = -1;
    const Summary out = run(s, [&](uint32_t node, uint16_t port) {
        client = std::thread([&client_result, node, port] {
            client_result = ksim_remote_serve(ksim_stack_kervan(), port, node);
        });
    });
    ASSERT_TRUE(client.joinable());
    client.join();
    EXPECT_EQ(client_result, 0);
    EXPECT_EQ(out.trace_hash, in.trace_hash);
}

TEST(Remote, ProcessThatExitsEarlyIsReported)
{
    Scenario s = one_kervan_node();
    s.nodes[0].process = spawn({KSIM_NODE_EXE, "--stack", "no-such-stack"});
    expect_failure(s, "node 1: process '");
    expect_failure(s, "exited with code 2 before connecting");
}

TEST(Remote, MissingProgramIsReported)
{
    Scenario s = one_kervan_node();
    s.nodes[0].process = spawn({"ksim-no-such-program-here"});
    expect_failure(s, "node 1: ");
}

TEST(Remote, StackMismatchIsReported)
{
    Scenario s = one_kervan_node();
    s.nodes[0].process = spawn({KSIM_NODE_EXE, "--stack", "nrf24-esb"});
    expect_failure(s, "serves stack 'nrf24-esb' but the scenario expects 'kervan'");
}

TEST(Remote, RejectedConfigIsReported)
{
    Scenario s = one_kervan_node();
    s.nodes[0].config = "bogus=1";
    s.nodes[0].process = spawn({KSIM_NODE_EXE, "--stack", "kervan"});
    expect_failure(s, "node 1: stack 'kervan' rejected config 'bogus=1'");
}

TEST(Remote, InProcessInstancesAreFreedWhenARemoteFails)
{
    // node 1 is created in process, node 2's process fails: no leak (checked by ASan builds)
    Scenario s = one_kervan_node();
    NodeSpec bad = s.nodes[0];
    bad.id = 2;
    bad.config = "bogus=1";
    bad.process = spawn({KSIM_NODE_EXE, "--stack", "kervan"});
    s.nodes.push_back(bad);
    expect_failure(s, "node 2");
}

#if defined(KSIM_PYTHON)

TEST(Remote, PythonStack)
{
    Scenario s;
    s.seed = 5;
    s.duration_us = 60'000'000;
    for (uint32_t id = 1; id <= 3; id++) {
        NodeSpec n;
        n.id = id;
        n.pos.x = 300.0 * id;
        n.stack = "py-beacon";
        n.config = "period_ms=2000";
        n.process = spawn({KSIM_PYTHON, std::string(KSIM_PY_DIR) + "/examples/beacon.py"});
        s.nodes.push_back(n);
    }
    const Summary a = run(s);
    EXPECT_GE(a.app_events.at("py_tx"), 3u * 29u);
    EXPECT_GT(a.app_events.at("py_rx"), 0u);
    EXPECT_EQ(a.versions.at("stack:py-beacon"), "1");
    EXPECT_EQ(run(s).trace_hash, a.trace_hash); // deterministic across runs

    s.nodes[0].config = "period_ms=2000;colour=red";
    expect_failure(s, "node 1: stack 'py-beacon' rejected config");
}

TEST(Remote, ProtocolViolationsAreReported)
{
    const std::pair<const char *, const char *> cases[] = {
        {"no_hello", "without sending HELLO"},
        {"bad_protocol", "speaks remote protocol 9"},
        {"bad_abi", "built for stack ABI 1"},
        {"bad_mode", "asks for mode 1"},
        {"wrong_node", "node 101: the stack process is not an out-of-process node"},
        {"unknown_request", "node 1: protocol error: unexpected message type 126"},
        {"short_request", "node 1: protocol error: malformed message"},
        {"bad_length", "node 1: malformed message length 0"},
        {"disconnect", "node 1: connection closed by the stack process"},
    };
    for (const auto &[what, message] : cases) {
        SCOPED_TRACE(what);
        Scenario s = one_kervan_node();
        s.nodes[0].process =
            spawn({KSIM_PYTHON, std::string(KSIM_PY_DIR) + "/examples/misbehave.py", what}, 5.0);
        expect_failure(s, message);
    }
}

TEST(Remote, UnresponsiveProcessTimesOut)
{
    Scenario s = one_kervan_node();
    s.nodes[0].process = spawn({KSIM_PYTHON, std::string(KSIM_PY_DIR) + "/examples/hang.py"}, 1.0);
    expect_failure(s, "node 1: no answer within");
}

#endif

#if defined(KSIM_PYTHON)
// The shipped example: Kervan in and out of process plus Python beacons on one channel.
TEST(Remote, ShippedMixedScenario)
{
#if defined(_WIN32)
    _putenv_s("KSIM_PYTHON", KSIM_PYTHON);
#else
    setenv("KSIM_PYTHON", KSIM_PYTHON, 1);
#endif
    const Scenario s = load_scenario_file(kScenarios + "/remote_mixed.json");
    Simulator sim(s);
    sim.register_stack(ksim_stack_kervan());
    const std::string node_dir = std::filesystem::path(KSIM_NODE_EXE).parent_path().string();
    sim.set_remote_options(0, node_dir); // "ksim-node" is found next to it, as next to ksim
    const Summary sum = sim.run();
    EXPECT_GT(sum.app_events.at("app_ack"), 0u);
    EXPECT_GT(sum.app_events.at("py_rx"), 0u);
    EXPECT_EQ(sum.versions.at("stack:py-beacon"), "1");
    EXPECT_EQ(sum.versions.at("stack:kervan"), "2");
}
#endif

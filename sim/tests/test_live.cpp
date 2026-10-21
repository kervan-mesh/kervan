/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Live sessions: the simulator paced by the wall clock, streamed while it runs.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api.hpp"
#include "http.hpp"
#include "live.hpp"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;
using namespace ksim::serve;
using nlohmann::json;

namespace {

const std::vector<const ksim_stack_t *> kStacks = {ksim_stack_kervan(), ksim_stack_nrf24_esb()};

std::string read_text(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

Scenario scenario(const std::string &name, double duration_s)
{
    json j = json::parse(read_text(std::string(KSIM_SCENARIO_DIR) + "/" + name + ".json"));
    j["duration_s"] = duration_s;
    return parse_scenario(j.dump());
}

std::string hex(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t batch_hash(const Scenario &s)
{
    Simulator sim(s);
    for (const ksim_stack_t *st : kStacks) {
        sim.register_stack(st);
    }
    return sim.run().trace_hash;
}

// Poll until the session ends; returns every line.
std::vector<std::string> drain(const LiveSession &live, LiveSession::Snapshot &last)
{
    std::vector<std::string> lines;
    for (int i = 0; i < 2000; i++) {
        last = live.poll(lines.size(), 100);
        EXPECT_EQ(last.from, lines.size());
        lines.insert(lines.end(), last.lines.begin(), last.lines.end());
        if (last.state == "done" || last.state == "error") {
            break;
        }
    }
    return lines;
}

// Runs a server on a free port until the end of the scope, also when an assertion fails.
class RunningServer {
public:
    explicit RunningServer(const Api &api)
        : server_(api, ""), port_(server_.bind(0)), thread_([this] { server_.listen(); })
    {
    }
    ~RunningServer()
    {
        server_.stop();
        thread_.join();
    }
    RunningServer(const RunningServer &) = delete;
    RunningServer &operator=(const RunningServer &) = delete;
    uint16_t port() const { return port_; }

private:
    HttpServer server_;
    uint16_t port_;
    std::thread thread_;
};

LiveSession::Options fast()
{
    LiveSession::Options o;
    o.speed = 10000; // 600 s of virtual time in about 60 ms
    o.tick_ms = 2;
    return o;
}

} // namespace

TEST(Live, SameRunAsBatch)
{
    const Scenario s = scenario("line_5", 600);
    LiveSession live(s, kStacks, fast());
    LiveSession::Snapshot last;
    const std::vector<std::string> lines = drain(live, last);
    ASSERT_EQ(last.state, "done") << last.error;
    EXPECT_EQ(last.t_us, s.duration_us);
    EXPECT_EQ(last.total_lines, lines.size());
    EXPECT_EQ(json::parse(lines.at(0)).at("ev"), "meta");

    // the stream is the whole trace, in order, and the result is that of `ksim run`
    uint64_t prev = 0;
    for (const std::string &l : lines) {
        const uint64_t t = json::parse(l).at("t").get<uint64_t>();
        EXPECT_GE(t, prev);
        prev = t;
    }
    const json result = json::parse(live.result());
    EXPECT_EQ(result.at("summary").at("trace_hash"), hex(batch_hash(s)));
    EXPECT_EQ(result.at("truncated"), false);
    EXPECT_FALSE(result.at("links").empty());
}

TEST(Live, PacedByTheClock)
{
    LiveSession::Options o;
    o.speed = 100; // 0.1 s of wall time = 10 s of virtual time
    const Scenario s = scenario("two_nodes", 3600);
    LiveSession live(s, kStacks, o);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    LiveSession::Snapshot snap = live.poll(~size_t{0}, 0);
    EXPECT_EQ(snap.state, "running");
    EXPECT_GT(snap.t_us, 5'000'000u);
    EXPECT_LT(snap.t_us, 120'000'000u); // far from done: it is paced, not run flat out
    EXPECT_DOUBLE_EQ(snap.speed, 100.0);

    live.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const Time paused_at = live.poll(~size_t{0}, 0).t_us;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    snap = live.poll(~size_t{0}, 0);
    EXPECT_EQ(snap.state, "paused");
    EXPECT_EQ(snap.t_us, paused_at); // the clock stands still

    live.set_speed(1000);
    live.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    snap = live.poll(~size_t{0}, 0);
    EXPECT_EQ(snap.state, "running");
    EXPECT_GT(snap.t_us, paused_at + 20'000'000u); // ~100 s at 1000x
    EXPECT_DOUBLE_EQ(snap.speed, 1000.0);

    live.stop();
    LiveSession::Snapshot last;
    drain(live, last);
    EXPECT_EQ(last.state, "done");
    EXPECT_LT(last.t_us, s.duration_us);
    const json result = json::parse(live.result());
    EXPECT_EQ(result.at("summary").at("duration_us"), last.t_us); // the time run so far

    live.set_speed(0); // clamped, harmless after the end
    EXPECT_DOUBLE_EQ(live.poll(~size_t{0}, 0).speed, 0.01);
}

TEST(Live, Errors)
{
    // rejected at start: the constructor throws, like Simulator::run
    Scenario bad = parse_scenario(
        R"({"nodes": [{"id": 1, "pos": [0, 0], "stack": "kervan", "config": {"len": 999}}]})");
    EXPECT_THROW(LiveSession(bad, kStacks, fast()), std::runtime_error);

    // failing while it runs: the session ends in "error" with the message
    Scenario late = parse_scenario(R"({"duration_s": 30, "nodes": [
        {"id": 1, "pos": [0, 0], "device": "arduino-nrf24l01p", "stack": "nrf24-esb",
         "config": {"send_to": "0xE7E7E7E702", "send_every_ms": 1000},
         "payload": {"hex": ")" + std::string(80, 'a') + R"("}},
        {"id": 2, "pos": [3, 0], "device": "arduino-nrf24l01p", "stack": "nrf24-esb"}]})");
    LiveSession live(late, kStacks, fast());
    LiveSession::Snapshot last;
    drain(live, last);
    EXPECT_EQ(last.state, "error");
    EXPECT_NE(last.error.find("payload of 40 bytes"), std::string::npos) << last.error;
    EXPECT_TRUE(live.result().empty());
}

TEST(Live, LinesBeyondTheLimitAreDropped)
{
    LiveSession::Options o = fast();
    o.max_trace_bytes = 500;
    LiveSession live(scenario("line_5", 600), kStacks, o);
    LiveSession::Snapshot last;
    const auto lines = drain(live, last);
    EXPECT_EQ(last.state, "done");
    EXPECT_TRUE(last.truncated);
    EXPECT_GE(lines.size(), 1u);
    EXPECT_LT(lines.size(), 10u);
    EXPECT_EQ(json::parse(live.result()).at("truncated"), true);
}

TEST(Live, Api)
{
    Options o;
    o.stacks = kStacks;
    o.scenario_dir = KSIM_SCENARIO_DIR;
    const Api api(o);
    const auto call = [&](const std::string &method, const std::string &path,
                          const std::string &body = "") {
        return api.handle(Request{method, path, body});
    };

    EXPECT_EQ(call("GET", "/api/live/status").status, 404);
    EXPECT_EQ(call("POST", "/api/live/control", R"({"action": "pause"})").status, 404);
    EXPECT_EQ(call("GET", "/api/live/start").status, 405);
    EXPECT_EQ(call("POST", "/api/live/start", "nope").status, 400);
    EXPECT_EQ(call("POST", "/api/live/start", R"({"speed": 1})").status, 400);
    EXPECT_EQ(call("POST", "/api/live/start", R"({"scenario": {"nodes": []}})").status, 400);
    Response r = call("POST", "/api/live/start", R"({"scenario": {"nodes": [{"id": 1,
        "pos": [0, 0], "stack": "kervan", "config": {"len": 999}}]}})");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("rejected config"), std::string::npos);
    r = call("POST", "/api/live/start", R"({"scenario": {"nodes": [{"id": 1, "pos": [0, 0],
        "stack": "kervan", "payload": {"python": "def payload(n, s, t): return b''"}}]}})");
    EXPECT_EQ(r.status, 403); // same rules as /api/run

    json scn = json::parse(read_text(std::string(KSIM_SCENARIO_DIR) + "/two_nodes.json"));
    scn["duration_s"] = 3600;
    r = call("POST", "/api/live/start", json{{"scenario", scn}, {"speed", 1}}.dump());
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).at("state"), "running");
    EXPECT_EQ(json::parse(r.body).at("duration_us"), 3'600'000'000ull);

    EXPECT_EQ(call("GET", "/api/live/result").status, 409);
    r = call("POST", "/api/live/control", R"({"action": "pause"})");
    EXPECT_EQ(json::parse(r.body).at("state"), "paused");
    r = call("POST", "/api/live/control", R"({"action": "speed", "speed": 50})");
    EXPECT_EQ(json::parse(r.body).at("speed"), 50);
    r = call("POST", "/api/live/control", R"({"action": "resume"})");
    EXPECT_EQ(json::parse(r.body).at("state"), "running");
    EXPECT_EQ(call("POST", "/api/live/control", R"({"action": "jump"})").status, 400);
    EXPECT_EQ(call("POST", "/api/live/control", "{").status, 400);
    EXPECT_EQ(call("GET", "/api/live/control").status, 405);
    EXPECT_EQ(call("POST", "/api/live/status").status, 405);
    EXPECT_EQ(call("POST", "/api/live/result").status, 405);
    EXPECT_EQ(call("GET", "/api/live/nope").status, 404);

    // a new session replaces the running one
    scn["duration_s"] = 60;
    r = call("POST", "/api/live/start", json{{"scenario", scn}, {"speed", 10000}}.dump());
    ASSERT_EQ(r.status, 200);
    const auto session = api.live_session();
    ASSERT_TRUE(session);
    LiveSession::Snapshot last;
    drain(*session, last);
    EXPECT_EQ(last.duration_us, 60'000'000u);
    r = call("GET", "/api/live/result");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body).at("summary").at("nodes").size(), 2u);

    // stream messages: status + lines, valid JSON
    last.lines = {R"({"t":0,"ev":"meta"})", R"({"t":5,"ev":"tx"})"};
    last.from = 7;
    const json msg = json::parse(Api::live_message(last));
    EXPECT_EQ(msg.at("from"), 7);
    EXPECT_EQ(msg.at("lines").size(), 2u);
    EXPECT_EQ(msg.at("lines").at(1).at("ev"), "tx");
    EXPECT_EQ(msg.at("state"), "done");
}

TEST(Live, StreamsOverHttp)
{
    Options o;
    o.stacks = kStacks;
    o.scenario_dir = KSIM_SCENARIO_DIR;
    const Api api(o);
    const RunningServer server(api);
    httplib::Client client("127.0.0.1", server.port());
    auto res = client.Get("/api/live/stream");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404); // nothing live yet

    json scn = json::parse(read_text(std::string(KSIM_SCENARIO_DIR) + "/two_nodes.json"));
    scn["duration_s"] = 120;
    res = client.Post("/api/live/start", json{{"scenario", scn}, {"speed", 2000}}.dump(),
                      "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;

    std::string body;
    res = client.Get("/api/live/stream?from=0", [&](const char *data, size_t len) {
        body.append(data, len);
        return true;
    });
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Content-Type"), "text/event-stream");

    size_t lines = 0;
    std::string state;
    std::istringstream in(body);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("data: ", 0) != 0) {
            continue;
        }
        const json msg = json::parse(line.substr(6));
        EXPECT_EQ(msg.at("from"), lines); // messages continue each other
        lines += msg.at("lines").size();
        state = msg.at("state");
    }
    EXPECT_EQ(state, "done");
    EXPECT_GT(lines, 10u);

    res = client.Get("/api/live/stream?from=x");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    res = client.Get("/api/live/stream", httplib::Headers{{"Host", "evil.example"}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

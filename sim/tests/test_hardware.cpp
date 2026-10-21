/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Real boards in live sessions (docs/hardware.md), tested with virtual boards: the board
// firmware's own code (firmware/common) on modelled nRF24 chips, talking over in-memory lines.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "api.hpp"
#include "board.hpp"
#include "kfw/app.h"
#include "ksim/scenario.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"
#include "live.hpp"
#include "serial.hpp"
#include "virtual_board.hpp"

using namespace ksim;
using namespace ksim::serve;
using nlohmann::json;

namespace {

const std::vector<const ksim_stack_t *> kStacks = {ksim_stack_kervan(), ksim_stack_nrf24_esb()};

// Lets a test pull the plug on a board.
class Unpluggable final : public hw::LineIo {
public:
    explicit Unpluggable(std::unique_ptr<hw::LineIo> inner) : inner_(std::move(inner)) {}
    void write_line(const std::string &line) override
    {
        if (unplugged) {
            throw std::runtime_error("unplugged");
        }
        inner_->write_line(line);
    }
    std::optional<std::string> read_line(int timeout_ms) override
    {
        if (unplugged) {
            throw std::runtime_error("disconnected");
        }
        return inner_->read_line(timeout_ms);
    }
    std::atomic<bool> unplugged{false};

private:
    std::unique_ptr<hw::LineIo> inner_;
};

// Two Picos as in sim/scenarios/pico_nrf24.json, on ports "A" (receiver) and "B" (sender).
const char *kScenario = R"({
  "duration_s": 1.2,
  "nodes": [
    { "id": 1, "pos": [0, 0], "mcu": "rpi-pico", "module": "nrf24l01p", "stack": "nrf24-esb",
      "config": { "addr": "0xE7E7E7E701" }, "hardware": { "port": "A" } },
    { "id": 2, "pos": [5, 0], "mcu": "rpi-pico", "module": "nrf24l01p-pa-lna", "stack": "nrf24-esb",
      "radio": { "tx_power_dbm": 8 },
      "config": { "send_to": "0xE7E7E7E701", "listen": 0, "send_every_ms": 100 },
      "payload": { "hex": "4D 65 72 68 61 62 61" }, "hardware": { "port": "B" } }
  ]
})";

// Virtual boards (the firmware's logic on modelled chips) on ports "A" and "B"; "none" has no
// radio, anything else does not exist.
struct Boards {
    hw::VirtualBoards virt{2};
    std::map<std::string, Unpluggable *> opened;

    std::function<std::unique_ptr<hw::LineIo>(const std::string &)> opener()
    {
        return [this](const std::string &port) -> std::unique_ptr<hw::LineIo> {
            const std::map<std::string, std::string> names = {
                {"A", "virtual:1"}, {"B", "virtual:2"}, {"none", "virtual:1"}};
            const auto it = names.find(port);
            if (it == names.end()) {
                throw std::runtime_error(port + ": no such port");
            }
            auto b = std::make_unique<Unpluggable>(virt.open(it->second, port != "none"));
            opened[port] = b.get();
            return b;
        };
    }
};

std::vector<json> run_live(const Scenario &scn, Boards &boards, LiveSession::Snapshot &last,
                           std::string &result, std::function<void(LiveSession &)> during = {})
{
    LiveSession::Options o;
    o.open_port = boards.opener();
    o.hold_ms = 50;
    LiveSession live(scn, kStacks, o);
    if (during) {
        during(live);
    }
    std::vector<json> lines;
    size_t n = 0;
    for (int i = 0; i < 400; i++) {
        last = live.poll(n, 100);
        n += last.lines.size();
        for (const std::string &l : last.lines) {
            lines.push_back(json::parse(l));
        }
        if (last.state == "done" || last.state == "error") {
            break;
        }
    }
    result = live.result();
    return lines;
}

std::vector<json> hw_lines(const std::vector<json> &lines, const std::string &kind, uint32_t node)
{
    std::vector<json> out;
    for (const json &l : lines) {
        if (l.at("ev") == "hw" && l.at("kind") == kind && l.at("node") == node) {
            out.push_back(l);
        }
    }
    return out;
}

} // namespace

TEST(Hardware, LineSplitter)
{
    hw::LineSplitter s(8);
    s.feed("ab\r\ncd", 6);
    EXPECT_EQ(s.next().value(), "ab");
    EXPECT_FALSE(s.next());
    s.feed("e\n\n0123456789\n", 14);
    EXPECT_EQ(s.next().value(), "cde");
    EXPECT_EQ(s.next().value(), "");
    EXPECT_EQ(s.next().value(), "01234567"); // cut at 8
    EXPECT_FALSE(s.next());
}

TEST(Hardware, ConfigCommandMatchesTheSimulatedNode)
{
    const Scenario s = parse_scenario(kScenario);
    EXPECT_EQ(hw::config_command(s.nodes[0]),
              R"({"cmd":"config","node":1,"channel":76,"rate":1000000,"crc":2,"pa":3,"aw":5,)"
              R"("addr":996028180225,"pipes":1,"listen":true,"every_ms":0,"jitter_ms":0,"len":8,)"
              R"("no_ack":false,"ard_us":500,"arc":3,"ack_payload_len":0})");
    EXPECT_EQ(hw::config_command(s.nodes[1]),
              R"({"cmd":"config","node":2,"channel":76,"rate":1000000,"crc":2,"pa":1,"aw":5,)"
              R"("addr":996028180226,"pipes":1,"listen":false,"send_to":996028180225,)"
              R"("every_ms":100,"jitter_ms":0,"len":8,"no_ack":false,"ard_us":500,"arc":3,)"
              R"("ack_payload_len":0,"payload":"4D657268616261"})");
}

TEST(Hardware, PaLevel)
{
    Scenario s = parse_scenario(kScenario);
    EXPECT_EQ(hw::pa_level(s.nodes[1]), 1); // PA/LNA module: 2, 8, 14, 20 dBm
    s.nodes[1].radio.tx_power_dbm = 20;
    EXPECT_EQ(hw::pa_level(s.nodes[1]), 3);
    NodeSpec bare;
    bare.radio.tx_power_dbm = -7; // no module: nearest chip step
    EXPECT_EQ(hw::pa_level(bare), 2);
    bare.radio.tx_power_dbm = 5;
    EXPECT_EQ(hw::pa_level(bare), 3);
    bare.radio.tx_power_dbm = -40;
    EXPECT_EQ(hw::pa_level(bare), 0);
}

TEST(Hardware, NodesThatCannotRunOnABoard)
{
    const auto error = [](const std::string &node) {
        const std::string text = R"({"nodes": [)" + node + "]}";
        try {
            hw::config_command(parse_scenario(text).nodes[0]);
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(error(R"({"id": 1, "pos": [0, 0], "stack": "kervan"})"),
              "node 1: real boards run the nrf24-esb stack, not kervan");
    const std::string esb =
        R"("id": 3, "pos": [0, 0], "module": "nrf24l01p", "stack": "nrf24-esb")";
    EXPECT_EQ(error("{" + esb + R"(, "payload": {"python": "def payload(n, s, t): return b''"}})"),
              "node 3: Python payloads do not run on real boards yet: use fixed bytes");
    EXPECT_EQ(error("{" + esb + R"(, "payload": {"hex": ")" + std::string(66, '1') + "\"}}"),
              "node 3: an nRF24 payload holds at most 32 bytes");
    NodeSpec n = parse_scenario(kScenario).nodes[0];
    const auto config_error = [&](const std::string &config) {
        n.config = config;
        try {
            hw::config_command(n);
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(config_error("colour=1"),
              "node 1: the board firmware does not know the option 'colour'");
    EXPECT_EQ(config_error("len=x"), "node 1: invalid value for 'len'");
    EXPECT_EQ(config_error("len="), "node 1: invalid value for 'len'");
    EXPECT_EQ(config_error("len"), "node 1: invalid config item 'len'");
    EXPECT_EQ(config_error("len=4;arc=2"), "no error");
}

TEST(Hardware, ScenarioKey)
{
    const auto error = [](const std::string &nodes) {
        try {
            parse_scenario(R"({"nodes": [)" + nodes + "]}");
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    const std::string n1 = R"({"id": 1, "pos": [0, 0], "stack": "kervan", "hardware": )";
    const std::string n2 = R"({"id": 2, "pos": [0, 0], "stack": "kervan", "hardware": )";
    EXPECT_EQ(error(n1 + R"({"port": "COM5"}})"), "no error");
    EXPECT_NE(error(n1 + R"({"port": ""}})").find("must name a serial port"), std::string::npos);
    EXPECT_NE(error(n1 + R"({"port": "COM5", "baud": 9600}})").find("baud"), std::string::npos);
    EXPECT_NE(error(n1 + R"({"port": "COM5"}}, )" + n2 + R"({"port": "COM5"}})")
                  .find("port COM5 is already used by node 1"),
              std::string::npos);
}

TEST(Hardware, HelloAndCommands)
{
    Boards boards;
    auto a = boards.opener()("A");
    const hw::BoardHello h = hw::hello(*a, 1000);
    EXPECT_EQ(h.fw, "kervan-nrf24");
    EXPECT_EQ(h.version, KFW_VERSION);
    EXPECT_EQ(h.proto, hw::kProtocol);
    EXPECT_EQ(h.board, "virtual");
    EXPECT_EQ(h.id, "VIRTUAL-1");
    EXPECT_EQ(h.radio, "nrf24l01p");
    EXPECT_NO_THROW(hw::command(*a, R"({"cmd":"config"})", "config", 1000));
    try {
        hw::command(*a, R"({"cmd":"config","channel":200})", "config", 1000);
        FAIL() << "no error";
    } catch (const std::runtime_error &e) {
        EXPECT_STREQ(e.what(), "config failed: out of range: 'channel'");
    }

    // a board that never answers
    class Silent final : public hw::LineIo {
    public:
        void write_line(const std::string &) override {}
        std::optional<std::string> read_line(int timeout_ms) override
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return std::string("boot noise");
        }
    } silent;
    try {
        hw::hello(silent, 50);
        FAIL() << "no error";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("is the Kervan firmware on the board?"),
                  std::string::npos);
    }
}

TEST(Hardware, LiveSessionWithTwoBoards)
{
    Boards boards;
    LiveSession::Snapshot last;
    std::string result;
    bool paused = true;
    bool sped = true;
    const auto lines =
        run_live(parse_scenario(kScenario), boards, last, result, [&](LiveSession &live) {
            paused = live.pause();
            sped = live.set_speed(100);
            EXPECT_FALSE(live.resume());
            EXPECT_TRUE(live.poll(0, 0).hardware);
        });
    ASSERT_EQ(last.state, "done") << last.error;
    EXPECT_FALSE(paused);
    EXPECT_FALSE(sped);
    EXPECT_EQ(last.speed, 1.0);

    ASSERT_EQ(hw_lines(lines, "hello", 1).size(), 1u);
    EXPECT_EQ(hw_lines(lines, "hello", 1)[0].at("port"), "A");
    EXPECT_EQ(hw_lines(lines, "hello", 2)[0].at("id"), "VIRTUAL-2");

    // 1.2 s at one frame per 100 ms: about 12 frames, all acknowledged
    const auto tx = hw_lines(lines, "tx", 2);
    const auto rx = hw_lines(lines, "rx", 1);
    EXPECT_GE(tx.size(), 9u);
    EXPECT_LE(tx.size(), 13u);
    for (const json &l : tx) {
        EXPECT_EQ(l.at("data"), "4D657268616261");
        EXPECT_EQ(l.at("ack"), true);
        EXPECT_EQ(l.at("retries"), 0);
    }
    EXPECT_GE(rx.size() + 1, tx.size()); // the last frame may arrive after the end
    for (const json &l : rx) {
        EXPECT_EQ(l.at("pipe"), 1);
        EXPECT_EQ(l.at("data"), "4D657268616261");
    }
    // board reports are in time order and lie within the run (later ones are dropped)
    uint64_t prev = 0;
    for (const json &l : lines) {
        if (l.at("ev") == "hw") {
            EXPECT_GE(l.at("t").get<uint64_t>(), prev);
            prev = l.at("t").get<uint64_t>();
        }
    }
    EXPECT_LE(prev, 1'200'000u);
    // the simulation of the same nodes ran too
    bool simulated_tx = false;
    for (const json &l : lines) {
        simulated_tx = simulated_tx || (l.at("ev") == "tx" && l.at("node") == 2);
    }
    EXPECT_TRUE(simulated_tx);

    const json r = json::parse(result);
    ASSERT_EQ(r.at("hardware").size(), 2u);
    EXPECT_EQ(r.at("hardware")[1].at("port"), "B");
    EXPECT_EQ(r.at("hardware")[1].at("fw"), "kervan-nrf24");
    EXPECT_GT(r.at("summary").at("tx").get<int>(), 0);
}

TEST(Hardware, AnUnpluggedBoardIsReported)
{
    Boards boards;
    LiveSession::Snapshot last;
    std::string result;
    const auto lines =
        run_live(parse_scenario(kScenario), boards, last, result, [&](LiveSession &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            boards.opened.at("B")->unplugged = true;
        });
    ASSERT_EQ(last.state, "done");
    const auto errors = hw_lines(lines, "error", 2);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].at("msg"), "disconnected");
}

TEST(Hardware, LiveSessionErrors)
{
    const auto error = [](const std::string &from, const std::string &to) {
        std::string text = kScenario;
        text.replace(text.find(from), from.size(), to);
        Boards boards;
        LiveSession::Options o;
        o.open_port = boards.opener();
        try {
            LiveSession live(parse_scenario(text), kStacks, o);
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(error(R"("port": "A")", R"("port": "COM77")"), "node 1 on COM77: no such port");
    EXPECT_EQ(error(R"("port": "A")", R"("port": "none")"),
              "node 1 on none: the board found no nRF24 radio: check the wiring "
              "(ksim wiring virtual nrf24l01p)");
    // the simulation checks the scenario before any board is asked
    EXPECT_EQ(error(R"("send_every_ms": 100)", R"("send_every_ms": 100, "len": 40)"),
              "node 2: stack 'nrf24-esb' rejected config "
              "'len=40;listen=0;send_every_ms=100;send_to=0xE7E7E7E701'");
}

TEST(Hardware, Api)
{
    Boards boards;
    Options opt;
    opt.stacks = kStacks;
    opt.open_port = boards.opener();
    opt.list_ports = [] { return std::vector<std::string>{"A", "B"}; };
    const Api api(opt);
    const auto call = [&](const std::string &method, const std::string &path,
                          const std::string &body = "") {
        return api.handle({method, path, body});
    };

    Response r = call("GET", "/api/ports");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, R"({"ports":[{"port":"A"},{"port":"B"}]})");
    EXPECT_EQ(call("POST", "/api/ports").status, 405);
    EXPECT_EQ(call("GET", "/api/ports/what").status, 404);

    r = call("POST", "/api/ports/hello", R"({"port":"B"})");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).at("hello").at("id"), "VIRTUAL-2");
    r = call("POST", "/api/ports/hello", R"({"port":"Q"})");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(json::parse(r.body).at("error"), "Q: no such port");
    EXPECT_EQ(call("POST", "/api/ports/hello", R"({"port":""})").status, 400);
    EXPECT_EQ(call("POST", "/api/ports/hello", "{").status, 400);
    EXPECT_EQ(call("GET", "/api/ports/hello").status, 405);

    json start;
    start["scenario"] = json::parse(kScenario);
    start["speed"] = 10;
    r = call("POST", "/api/live/start", start.dump());
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).at("hardware"), true);
    EXPECT_EQ(json::parse(r.body).at("speed"), 1.0);
    r = call("POST", "/api/live/control", R"({"action":"pause"})");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(json::parse(r.body).at("error"),
              "real boards run in real time: no pause or speed change");
    EXPECT_EQ(call("POST", "/api/live/control", R"({"action":"stop"})").status, 200);
}

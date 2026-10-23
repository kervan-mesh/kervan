/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ksim::hw {

namespace {

using nlohmann::ordered_json;

[[noreturn]] void fail(const NodeSpec &n, const std::string &msg)
{
    throw std::runtime_error("node " + std::to_string(n.id) + ": " + msg);
}

// The nrf24-esb stack's "key=value;key=value" config as numbers.
std::map<std::string, unsigned long long> esb_options(const NodeSpec &n)
{
    static const char *const known[] = {
        "addr",      "aw",  "pipes",  "listen", "send_to", "send_every_ms",
        "jitter_ms", "len", "no_ack", "ard_us", "arc",     "ack_payload_len"};
    std::map<std::string, unsigned long long> out;
    size_t pos = 0;
    while (pos < n.config.size()) {
        size_t end = n.config.find(';', pos);
        if (end == std::string::npos) {
            end = n.config.size();
        }
        const std::string item = n.config.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            fail(n, "invalid config item '" + item + "'");
        }
        const std::string key = item.substr(0, eq);
        bool ok = false;
        for (const char *k : known) {
            ok = ok || key == k;
        }
        if (!ok) {
            fail(n, "the board firmware does not know the option '" + key + "'");
        }
        char *stop = nullptr;
        const std::string value = item.substr(eq + 1);
        out[key] = std::strtoull(value.c_str(), &stop, 0);
        if (value.empty() || *stop != '\0') {
            fail(n, "invalid value for '" + key + "'");
        }
    }
    return out;
}

std::string hex(const std::vector<uint8_t> &bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 0x0F];
    }
    return s;
}

using Clock = std::chrono::steady_clock;

// Waits for a line with "ev" in `events`; other lines are skipped.
ordered_json wait_for(LineIo &io, std::initializer_list<const char *> events, int timeout_ms,
                      const std::string &what)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const auto line = io.read_line(static_cast<int>(std::max<long long>(left, 1)));
        if (!line) {
            continue;
        }
        const ordered_json j = ordered_json::parse(*line, nullptr, false);
        if (!j.is_object() || !j.contains("ev") || !j.at("ev").is_string()) {
            continue; // boot messages, noise
        }
        const std::string ev = j.at("ev").get<std::string>();
        for (const char *e : events) {
            if (ev == e) {
                return j;
            }
        }
    }
    throw std::runtime_error("no answer to " + what + " within " +
                             std::to_string(timeout_ms / 1000.0).substr(0, 3) +
                             " s: is the Kervan firmware on the board?");
}

} // namespace

int pa_level(const NodeSpec &n)
{
    const double dbm = n.radio.tx_power_dbm;
    if (n.device && n.device->tx_power_levels.size() == 4) {
        for (size_t i = 0; i < 4; i++) {
            if (std::abs(n.device->tx_power_levels[i] - dbm) < 0.01) {
                return static_cast<int>(i);
            }
        }
    }
    static const double steps[] = {-18.0, -12.0, -6.0, 0.0};
    int best = 0;
    for (int i = 1; i < 4; i++) {
        if (std::abs(steps[i] - dbm) < std::abs(steps[best] - dbm)) {
            best = i;
        }
    }
    return best;
}

std::string config_command(const NodeSpec &n)
{
    if (n.stack != "nrf24-esb") {
        fail(n, "real boards run the nrf24-esb stack, not " + n.stack);
    }
    if (n.radio.type != PhyType::Nrf24) {
        fail(n, "real boards need an nrf24 radio");
    }
    if (n.process) {
        fail(n, "a node on a real board cannot also run in a process");
    }
    const auto opt = esb_options(n);
    const auto get = [&](const char *key, unsigned long long def) {
        const auto it = opt.find(key);
        return it != opt.end() ? it->second : def;
    };

    ordered_json j;
    j["cmd"] = "config";
    j["node"] = n.id;
    j["channel"] = n.radio.nrf24.channel;
    j["rate"] = n.radio.nrf24.data_rate;
    j["crc"] = n.radio.nrf24.crc_bytes;
    j["pa"] = pa_level(n);
    j["aw"] = get("aw", 5);
    j["addr"] = get("addr", 0xE7E7E7E700ull | (n.id & 0xFFu));
    j["pipes"] = get("pipes", 1);
    j["listen"] = get("listen", 1) != 0;
    if (opt.count("send_to") != 0) {
        j["send_to"] = get("send_to", 0);
    }
    j["every_ms"] = get("send_every_ms", 0);
    j["jitter_ms"] = get("jitter_ms", 0);
    j["len"] = get("len", 8);
    j["no_ack"] = get("no_ack", 0) != 0;
    j["ard_us"] = get("ard_us", 500);
    j["arc"] = get("arc", 3);
    j["ack_payload_len"] = get("ack_payload_len", 0);
    if (n.payload) {
        if (!n.payload->python.empty()) {
            fail(n, "Python payloads do not run on real boards yet: use fixed bytes");
        }
        if (n.payload->bytes.size() > 32) {
            fail(n, "an nRF24 payload holds at most 32 bytes");
        }
        j["payload"] = hex(n.payload->bytes);
    }
    return j.dump();
}

BoardHello hello(LineIo &io, int timeout_ms)
{
    io.write_line(R"({"cmd":"hello"})");
    const ordered_json j = wait_for(io, {"hello"}, timeout_ms, "hello");
    BoardHello h;
    h.fw = j.value("fw", "");
    h.version = j.value("version", 0);
    h.proto = j.value("proto", 0);
    h.board = j.value("board", "");
    h.id = j.value("id", "");
    h.radio = j.value("radio", "");
    h.wiring = j.value("wiring", "");
    h.json = j.dump();
    return h;
}

void command(LineIo &io, const std::string &line, const std::string &cmd, int timeout_ms)
{
    io.write_line(line);
    while (true) {
        const ordered_json j = wait_for(io, {"ok", "error"}, timeout_ms, cmd);
        if (j.at("ev") == "error") {
            throw std::runtime_error(cmd + " failed: " + j.value("msg", std::string("?")));
        }
        if (j.value("cmd", "") == cmd) {
            return;
        }
    }
}

} // namespace ksim::hw

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "payload_scripts.hpp"

#include <chrono>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ksim/scenario.hpp"

namespace ksim {

// Generated at build time from sim/remote/python/ksim_payload.py (see sim/CMakeLists.txt).
extern const char *const kPayloadHelper;

namespace {

constexpr int kStartTimeoutMs = 15000; // interpreter start-up, also on a slow CI machine
constexpr int kCallTimeoutMs = 10000;

using nlohmann::json;

[[noreturn]] void fail(const std::string &msg)
{
    throw std::runtime_error("python payload: " + msg);
}

} // namespace

PayloadScripts::PayloadScripts(const std::map<uint32_t, std::string> &code, uint64_t seed)
    : listener_(net::Socket::listen_loopback(0))
{
    const std::string python = python_interpreter();
    try {
        proc_ = Process::spawn({python, "-c", kPayloadHelper, std::to_string(listener_.local_port())},
                               {}, "");
    } catch (const std::exception &e) {
        fail("cannot start '" + python + "' (" + e.what() + "); set KSIM_PYTHON to the interpreter");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStartTimeoutMs);
    while (!sock_.valid()) {
        if (proc_.exit_code()) {
            fail("'" + python + "' exited at start-up (is it Python 3?)");
        }
        if (std::chrono::steady_clock::now() > deadline) {
            fail("'" + python + "' did not connect within " +
                 std::to_string(kStartTimeoutMs / 1000) + " s");
        }
        sock_ = listener_.accept(50);
    }

    json init;
    init["seed"] = seed;
    json functions = json::object();
    for (const auto &[node, src] : code) {
        functions[std::to_string(node)] = src;
    }
    init["code"] = functions;
    const json reply = json::parse(exchange(init.dump(), kStartTimeoutMs));
    if (reply.contains("error")) {
        fail(reply.at("error").get<std::string>());
    }
}

PayloadScripts::~PayloadScripts()
{
    sock_.close(); // the helper exits when the connection closes
    proc_.wait_or_kill(2000);
}

std::string PayloadScripts::exchange(const std::string &request, int timeout_ms)
{
    const auto len = static_cast<uint32_t>(request.size());
    const uint8_t head[4] = {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                             static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24)};
    try {
        sock_.send_all(head, 4);
        sock_.send_all(reinterpret_cast<const uint8_t *>(request.data()), request.size());
        uint8_t rhead[4];
        if (!sock_.recv_exact(rhead, 4, timeout_ms)) {
            fail("no answer within " + std::to_string(timeout_ms / 1000) +
                 " s (endless loop in payload()?)");
        }
        const uint32_t rlen = static_cast<uint32_t>(rhead[0]) | static_cast<uint32_t>(rhead[1]) << 8 |
                              static_cast<uint32_t>(rhead[2]) << 16 |
                              static_cast<uint32_t>(rhead[3]) << 24;
        if (rlen > (1u << 20)) {
            fail("malformed answer");
        }
        std::string body(rlen, '\0');
        if (rlen > 0 && !sock_.recv_exact(reinterpret_cast<uint8_t *>(body.data()), rlen, timeout_ms)) {
            fail("answer cut off");
        }
        return body;
    } catch (const std::runtime_error &e) {
        const std::string what = e.what();
        if (what.rfind("python payload:", 0) == 0) {
            throw;
        }
        fail("the Python process failed (" + what + ")");
    }
}

std::vector<uint8_t> PayloadScripts::call(uint32_t node, uint32_t seq, Time t_us)
{
    const json req{{"node", node}, {"seq", seq}, {"t_us", t_us}};
    const json reply = json::parse(exchange(req.dump(), kCallTimeoutMs));
    if (reply.contains("error")) {
        fail(reply.at("error").get<std::string>());
    }
    const std::string hex = reply.at("data").get<std::string>();
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

} // namespace ksim

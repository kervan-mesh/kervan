/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Python payload functions of a scenario, run by one helper process per simulation
// (sim/remote/python/ksim_payload.py, embedded in ksim). Calls are synchronous, so the run stays
// deterministic.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ksim/engine.hpp"
#include "remote/net.hpp"
#include "remote/process.hpp"

namespace ksim {

class PayloadScripts {
public:
    // code: node id -> Python source defining payload(node, seq, t). Starts the interpreter
    // (python_interpreter()) and compiles every function. Throws std::runtime_error.
    PayloadScripts(const std::map<uint32_t, std::string> &code, uint64_t seed);
    ~PayloadScripts();
    PayloadScripts(const PayloadScripts &) = delete;
    PayloadScripts &operator=(const PayloadScripts &) = delete;

    // The payload for a node's message `seq` at virtual time t_us. Throws std::runtime_error
    // with the Python error.
    std::vector<uint8_t> call(uint32_t node, uint32_t seq, Time t_us);

private:
    std::string exchange(const std::string &request, int timeout_ms);

    net::Socket listener_;
    net::Socket sock_;
    Process proc_;
};

} // namespace ksim

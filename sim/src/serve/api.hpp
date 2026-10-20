/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The web UI's HTTP API without the HTTP: requests in, responses out, so it can be tested without
// sockets. Routes and formats: docs/web-ui.md.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ksim/device.hpp"
#include "ksim/stack.h"

namespace ksim::serve {

struct Options {
    std::vector<const ksim_stack_t *> stacks;
    DeviceRegistry devices = DeviceRegistry::builtin();
    std::string scenario_dir;           // listed, opened and saved by the UI; empty = none
    std::string program_dir;            // where ksim-node is looked up for out-of-process nodes
    bool allow_processes = false;       // let UI runs spawn out-of-process stacks
    size_t max_trace_bytes = 256u << 20; // longer traces are cut (the summary stays complete)
};

struct Request {
    std::string method; // "GET", "PUT", "POST"
    std::string path;   // "/api/..."
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

class Api {
public:
    explicit Api(Options options);

    // Safe to call from several threads at once.
    Response handle(const Request &req) const;

private:
    Response info() const;
    Response devices() const;
    Response stacks() const;
    Response phys() const;
    Response scenario_list() const;
    Response scenario_get(const std::string &name) const;
    Response scenario_put(const std::string &name, const std::string &body) const;
    Response check(const std::string &body) const;
    Response run(const std::string &body) const;
    Response decode(const std::string &body) const;

    Options opt_;
};

} // namespace ksim::serve

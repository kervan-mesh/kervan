/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The web UI's HTTP API without the HTTP: requests in, responses out, so it can be tested without
// sockets. Routes and formats: docs/web-ui.md.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ksim/device.hpp"
#include "ksim/simulator.hpp"
#include "firmware.hpp"
#include "live.hpp"
#include "ksim/stack.h"

namespace ksim::serve {

struct Options {
    std::vector<const ksim_stack_t *> stacks;
    DeviceRegistry devices = DeviceRegistry::builtin();
    std::string scenario_dir;           // listed, opened and saved by the UI; empty = none
    std::string program_dir;            // where ksim-node is looked up for out-of-process nodes
    bool allow_processes = false;       // let UI runs spawn out-of-process stacks
    size_t max_trace_bytes = 256u << 20; // longer traces are cut (the summary stays complete)
    // Real boards (docs/hardware.md); empty: serial ports. Tests put simulated boards here.
    std::function<std::unique_ptr<hw::LineIo>(const std::string &port)> open_port;
    std::function<std::vector<std::string>()> list_ports;
    // Firmware builds (docs/hardware.md); null: building is not offered.
    std::shared_ptr<hw::FirmwareBuilder> builder;
    // Boards' USB drives for flashing; empty: hw::boot_drives. Tests use directories.
    std::function<std::vector<hw::BootDrive>()> list_drives;
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

    // The live session (POST /api/live/start), for streaming; null if there is none.
    std::shared_ptr<LiveSession> live_session() const;
    // One message of the live stream: status plus the new trace lines, as JSON.
    static std::string live_message(const LiveSession::Snapshot &s);

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
    Response wiring(const std::string &path) const;
    Response ports(const std::string &action, const std::string &method,
                   const std::string &body) const;
    Response wiring_for(const std::string &body) const;
    Response firmware(const std::string &action, const std::string &method,
                      const std::string &body) const;
    Response live(const std::string &method, const std::string &action,
                  const std::string &body) const;
    std::optional<Response> prepare(const std::string &body, Scenario &scn) const;

    Options opt_;
    mutable std::mutex live_m_;
    mutable std::shared_ptr<LiveSession> live_;
};

} // namespace ksim::serve

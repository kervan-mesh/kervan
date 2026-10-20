/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Out-of-process stacks (docs/remote-protocol.md). A RemoteNode looks like any other stack
// instance to the simulator; every callback becomes one lockstep exchange over its connection.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ksim/simulator.hpp"
#include "ksim/stack.h"
#include "net.hpp"
#include "process.hpp"
#include "wire.hpp"

namespace ksim {

class RemoteNode {
public:
    RemoteNode(uint32_t node_id, net::Socket sock, Process proc, int timeout_ms,
               std::string stack_name, std::string stack_version);

    const std::string &stack_name() const { return stack_name_; }
    const std::string &stack_version() const { return stack_version_; }

    // Mirror ksim_stack_t; each throws std::runtime_error naming the node if the process fails.
    bool create(const ksim_host_t *host, Time now, const std::string &config);
    void on_rx(const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta);
    void on_tx_done();
    void on_timer(uint32_t timer_id);
    void destroy();

    // ksim_stack_t whose callbacks forward to the RemoteNode passed as instance.
    static const ksim_stack_t *vtable();

private:
    bool exchange(wire::Writer &event);
    wire::Reader receive();
    void send(wire::Writer &msg);
    void reply(int32_t status, int64_t value);
    [[noreturn]] void fail(const std::string &what);

    uint32_t node_id_;
    net::Socket sock_;
    Process proc_;
    int timeout_ms_;
    std::string stack_name_;
    std::string stack_version_;
    const ksim_host_t *host_ = nullptr;
};

class RemoteHub {
public:
    // Listens on 127.0.0.1:port (0 = any free port). bare program names in "spawn" are looked up
    // in program_dir first.
    RemoteHub(uint16_t port, std::string program_dir);
    uint16_t port() const { return port_; }

    using WaitCallback = std::function<void(uint32_t node_id, uint16_t port)>;

    // Start the "spawn" processes, announce the "wait" nodes, and accept connections until every
    // node has said HELLO. Throws if a process dies, does not connect in time or does not match.
    std::map<uint32_t, std::unique_ptr<RemoteNode>> connect(const std::vector<const NodeSpec *> &nodes,
                                                            const WaitCallback &on_wait);

private:
    net::Socket listener_;
    uint16_t port_ = 0;
    std::string program_dir_;
};

} // namespace ksim

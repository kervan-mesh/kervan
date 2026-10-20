/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Minimal blocking TCP on 127.0.0.1 with timeouts (Winsock / POSIX).

#pragma once

#include <cstddef>
#include <cstdint>

namespace ksim::net {

class Socket {
public:
    Socket() = default;
    explicit Socket(uintptr_t handle) : handle_(handle) {}
    ~Socket();
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    bool valid() const;
    void close();

    // Throws std::runtime_error if the peer is gone.
    void send_all(const uint8_t *data, size_t len);
    // Waits up to timeout_ms (< 0: forever). Returns false on timeout; throws if the peer is gone.
    bool recv_exact(uint8_t *data, size_t len, int timeout_ms);

    // Listening socket on 127.0.0.1:port (0 = any free port). Throws on failure.
    static Socket listen_loopback(uint16_t port);
    uint16_t local_port() const;
    // Invalid socket on timeout.
    Socket accept(int timeout_ms);

private:
    bool wait_readable(int timeout_ms);
    uintptr_t handle_ = ~uintptr_t{0};
};

} // namespace ksim::net

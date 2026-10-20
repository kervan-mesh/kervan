/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// `ksim serve`: the API and the built web UI over HTTP on 127.0.0.1.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "api.hpp"

namespace ksim::serve {

class HttpServer {
public:
    // web_dir holds the built UI (web/dist); without an index.html a page explains how to build it.
    HttpServer(const Api &api, std::string web_dir);
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // Bind 127.0.0.1:port (0 = any free port) and return the port. Throws std::runtime_error.
    uint16_t bind(uint16_t port);
    // Serve until stop() is called (from another thread). Call bind() first.
    void listen();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ksim::serve

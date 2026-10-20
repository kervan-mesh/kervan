/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

#include <httplib.h>

namespace ksim::serve {

namespace {

const char *const kNotBuilt = R"(<!doctype html>
<html><head><meta charset="utf-8"><title>ksim</title></head>
<body style="font-family: sans-serif; max-width: 40em; margin: 3em auto">
<h1>ksim serve</h1>
<p>The API is running, but the web UI has not been built yet:</p>
<pre>cd web
npm ci
npm run build</pre>
<p>then reload this page (or pass <code>--web DIR</code>).</p>
</body></html>
)";

} // namespace

struct HttpServer::Impl {
    const Api &api;
    std::string web_dir;
    httplib::Server server;
    uint16_t port = 0;

    Impl(const Api &a, std::string dir) : api(a), web_dir(std::move(dir)) {}

    // The server only answers pages it served itself: a Host check stops DNS rebinding, an
    // Origin check and a JSON content type stop other web sites from posting to the API.
    bool allowed(const httplib::Request &req, httplib::Response &res) const
    {
        const std::string p = std::to_string(port);
        const std::string host = req.get_header_value("Host");
        if (host != "127.0.0.1:" + p && host != "localhost:" + p) {
            res.status = 403;
            res.set_content("forbidden host", "text/plain");
            return false;
        }
        if (req.has_header("Origin")) {
            const std::string origin = req.get_header_value("Origin");
            if (origin != "http://127.0.0.1:" + p && origin != "http://localhost:" + p) {
                res.status = 403;
                res.set_content("forbidden origin", "text/plain");
                return false;
            }
        }
        if ((req.method == "POST" || req.method == "PUT") &&
            req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            res.status = 415;
            res.set_content("use Content-Type: application/json", "text/plain");
            return false;
        }
        return true;
    }

    void setup()
    {
        // httplib's defaults (SO_REUSEPORT, or SO_REUSEADDR on Windows) let a second server
        // share the port silently; a port in use must be an error.
        server.set_socket_options([](auto sock) {
            int on = 1;
#ifdef _WIN32
            setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&on),
                       sizeof(on));
#else
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); // restart after TIME_WAIT
#endif
        });

        // Requests with a body are checked in their handler: httplib reads the body after
        // pre-routing, and answering before it is read would break the connection.
        server.set_pre_routing_handler([this](const httplib::Request &req,
                                              httplib::Response &res) {
            res.set_header("X-Content-Type-Options", "nosniff");
            const bool body =
                req.has_header("Content-Length") || req.has_header("Transfer-Encoding");
            return body || allowed(req, res) ? httplib::Server::HandlerResponse::Unhandled
                                             : httplib::Server::HandlerResponse::Handled;
        });

        auto api_handler = [this](const httplib::Request &req, httplib::Response &res) {
            if (!allowed(req, res)) {
                return;
            }
            const Response r = api.handle(Request{req.method, req.path, req.body});
            res.status = r.status;
            res.set_header("Cache-Control", "no-store");
            res.set_content(r.body, r.content_type);
        };
        server.Get(R"(/api/.*)", api_handler);
        server.Put(R"(/api/.*)", api_handler);
        server.Post(R"(/api/.*)", api_handler);

        std::error_code ec;
        if (!web_dir.empty() && std::filesystem::exists(web_dir + "/index.html", ec)) {
            server.set_mount_point("/", web_dir);
        } else {
            server.Get("/", [](const httplib::Request &, httplib::Response &res) {
                res.set_content(kNotBuilt, "text/html; charset=utf-8");
            });
        }
    }
};

HttpServer::HttpServer(const Api &api, std::string web_dir)
    : impl_(std::make_unique<Impl>(api, std::move(web_dir)))
{
    impl_->setup();
}

HttpServer::~HttpServer() = default;

uint16_t HttpServer::bind(uint16_t port)
{
    Impl &s = *impl_;
    if (port == 0) {
        const int p = s.server.bind_to_any_port("127.0.0.1");
        if (p <= 0) {
            throw std::runtime_error("cannot bind 127.0.0.1");
        }
        s.port = static_cast<uint16_t>(p);
    } else {
        if (!s.server.bind_to_port("127.0.0.1", port)) {
            throw std::runtime_error("cannot bind 127.0.0.1:" + std::to_string(port) +
                                     " (in use? try --port)");
        }
        s.port = port;
    }
    return s.port;
}

void HttpServer::listen()
{
    impl_->server.listen_after_bind();
}

void HttpServer::stop()
{
    impl_->server.stop();
}

} // namespace ksim::serve

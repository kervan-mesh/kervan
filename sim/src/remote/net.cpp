/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using native_t = SOCKET;
using io_len_t = int;
#define KSIM_POLL WSAPoll
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using native_t = int;
using io_len_t = size_t;
#define KSIM_POLL poll
#endif

namespace ksim::net {

namespace {

constexpr uintptr_t kInvalid = ~uintptr_t{0};

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit()
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};
void ensure_init()
{
    static WinsockInit init;
}
#else
void ensure_init() {}
#endif

native_t native(uintptr_t h)
{
    return static_cast<native_t>(h);
}

[[noreturn]] void fail(const std::string &what)
{
    throw std::runtime_error(what);
}

sockaddr_in loopback(uint16_t port)
{
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

void tune(native_t s)
{
    int one = 1;
    // lockstep sends many tiny messages; do not let Nagle delay them
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
               static_cast<socklen_t>(sizeof(one)));
#if defined(__APPLE__)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, static_cast<socklen_t>(sizeof(one)));
#endif
}

} // namespace

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket &&other) noexcept : handle_(other.handle_)
{
    other.handle_ = kInvalid;
}

Socket &Socket::operator=(Socket &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

bool Socket::valid() const
{
    return handle_ != kInvalid;
}

void Socket::close()
{
    if (valid()) {
#if defined(_WIN32)
        closesocket(native(handle_));
#else
        ::close(native(handle_));
#endif
        handle_ = kInvalid;
    }
}

void Socket::send_all(const uint8_t *data, size_t len)
{
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL; // a dead peer must not kill ksim with SIGPIPE
#else
    const int flags = 0;
#endif
    while (len > 0) {
        const auto n = ::send(native(handle_), reinterpret_cast<const char *>(data),
                              static_cast<io_len_t>(len), flags);
        if (n <= 0) {
            fail("connection lost");
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

bool Socket::wait_readable(int timeout_ms)
{
    pollfd p;
    p.fd = native(handle_);
    p.events = POLLIN;
    p.revents = 0;
    const int r = KSIM_POLL(&p, 1, timeout_ms);
    if (r < 0) {
        fail("poll failed");
    }
    return r > 0;
}

bool Socket::recv_exact(uint8_t *data, size_t len, int timeout_ms)
{
    while (len > 0) {
        if (!wait_readable(timeout_ms)) {
            return false;
        }
        const auto n =
            ::recv(native(handle_), reinterpret_cast<char *>(data), static_cast<io_len_t>(len), 0);
        if (n <= 0) {
            fail("connection closed by the stack process");
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

Socket Socket::listen_loopback(uint16_t port)
{
    ensure_init();
    const native_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Socket sock(static_cast<uintptr_t>(s));
#if defined(_WIN32)
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        sock.handle_ = kInvalid;
        fail("cannot create socket");
    }
#if !defined(_WIN32)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, static_cast<socklen_t>(sizeof(one)));
#endif
    const sockaddr_in addr = loopback(port);
    if (::bind(s, reinterpret_cast<const sockaddr *>(&addr), static_cast<socklen_t>(sizeof(addr))) !=
        0) {
        fail("cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)");
    }
    if (::listen(s, 64) != 0) {
        fail("listen failed");
    }
    return sock;
}

uint16_t Socket::local_port() const
{
    sockaddr_in addr;
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    if (::getsockname(native(handle_), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

Socket Socket::accept(int timeout_ms)
{
    if (!wait_readable(timeout_ms)) {
        return Socket{};
    }
    const native_t c = ::accept(native(handle_), nullptr, nullptr);
#if defined(_WIN32)
    if (c == INVALID_SOCKET) {
#else
    if (c < 0) {
#endif
        return Socket{};
    }
    tune(c);
    return Socket(static_cast<uintptr_t>(c));
}

} // namespace ksim::net

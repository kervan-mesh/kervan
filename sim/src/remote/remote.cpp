/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "remote.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>

#include "ksim/remote_protocol.h"

namespace ksim {

namespace {

constexpr int kDefaultSpawnTimeoutMs = 10000;
constexpr int kHelloTimeoutMs = 5000;

std::string node_name(uint32_t id)
{
    return "node " + std::to_string(id);
}

int to_ms(double seconds)
{
    return static_cast<int>(std::llround(seconds * 1000.0));
}

// Read one length-prefixed message; nullopt on timeout.
std::optional<wire::Reader> read_message(net::Socket &sock, int timeout_ms)
{
    uint8_t len_bytes[4];
    if (!sock.recv_exact(len_bytes, 4, timeout_ms)) {
        return std::nullopt;
    }
    const uint32_t len = static_cast<uint32_t>(len_bytes[0]) |
                         static_cast<uint32_t>(len_bytes[1]) << 8 |
                         static_cast<uint32_t>(len_bytes[2]) << 16 |
                         static_cast<uint32_t>(len_bytes[3]) << 24;
    if (len == 0 || len > KSIM_REMOTE_MAX_MESSAGE) {
        throw std::runtime_error("malformed message length " + std::to_string(len));
    }
    std::vector<uint8_t> body(len);
    if (!sock.recv_exact(body.data(), len, timeout_ms)) {
        return std::nullopt;
    }
    return wire::Reader(std::move(body));
}

void send_error(net::Socket &sock, const std::string &msg)
{
    try {
        wire::Writer w(KSIM_MSG_ERROR);
        w.str(msg);
        const auto &bytes = w.finish();
        sock.send_all(bytes.data(), bytes.size());
    } catch (const std::exception &) {
        // the peer is gone anyway
    }
}

// ---- ksim_stack_t forwarding to RemoteNode ----

RemoteNode &self(void *inst)
{
    return *static_cast<RemoteNode *>(inst);
}

void *r_create(const ksim_host_t *, const char *)
{
    return nullptr; // remote nodes are created by the simulator through RemoteNode::create
}
void r_destroy(void *inst)
{
    self(inst).destroy();
}
void r_on_rx(void *inst, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    self(inst).on_rx(buf, len, meta);
}
void r_on_tx_done(void *inst)
{
    self(inst).on_tx_done();
}
void r_on_timer(void *inst, uint32_t timer_id)
{
    self(inst).on_timer(timer_id);
}

const ksim_stack_t kRemoteVtable = {KSIM_ABI_VERSION, "remote",     "1",       r_create,
                                    r_destroy,        r_on_rx,      r_on_tx_done, r_on_timer,
                                    nullptr};

} // namespace

// ---- RemoteNode ----

RemoteNode::RemoteNode(uint32_t node_id, net::Socket sock, Process proc, int timeout_ms,
                       std::string stack_name, std::string stack_version)
    : node_id_(node_id), sock_(std::move(sock)), proc_(std::move(proc)), timeout_ms_(timeout_ms),
      stack_name_(std::move(stack_name)), stack_version_(std::move(stack_version))
{
}

const ksim_stack_t *RemoteNode::vtable()
{
    return &kRemoteVtable;
}

void RemoteNode::fail(const std::string &what)
{
    std::string msg = node_name(node_id_) + ": " + what;
    if (auto code = proc_.exit_code()) {
        msg += " (process exited with code " + std::to_string(*code) + ")";
    }
    sock_.close();
    proc_.kill();
    throw std::runtime_error(msg);
}

void RemoteNode::send(wire::Writer &msg)
{
    const auto &bytes = msg.finish();
    try {
        sock_.send_all(bytes.data(), bytes.size());
    } catch (const std::runtime_error &e) {
        fail(e.what());
    }
}

wire::Reader RemoteNode::receive()
{
    std::optional<wire::Reader> r;
    try {
        r = read_message(sock_, timeout_ms_);
    } catch (const std::runtime_error &e) {
        fail(e.what());
    }
    if (!r) {
        fail("no answer within " + std::to_string(timeout_ms_ / 1000.0).substr(0, 4) + " s");
    }
    return std::move(*r);
}

void RemoteNode::reply(int32_t status, int64_t value)
{
    wire::Writer w(KSIM_MSG_RESULT);
    w.i32(status).i64(value);
    send(w);
}

// Send an event and serve the stack's requests until it says DONE.
bool RemoteNode::exchange(wire::Writer &event)
{
    send(event);
    void *ctx = host_->ctx;
    for (;;) {
        wire::Reader r = receive();
        try {
            const uint8_t type = r.u8();
            switch (type) {
            case KSIM_REQ_RADIO_SEND: {
                const std::vector<uint8_t> frame = r.bytes();
                reply(host_->radio_send(ctx, frame.data(), frame.size()), 0);
                break;
            }
            case KSIM_REQ_TIMER_SET: {
                const uint32_t id = r.u32();
                host_->timer_set(ctx, id, r.u64());
                break;
            }
            case KSIM_REQ_TIMER_CANCEL:
                host_->timer_cancel(ctx, r.u32());
                break;
            case KSIM_REQ_TRACE: {
                const std::string name = r.str();
                const std::string detail = r.str();
                host_->trace(ctx, name.c_str(), detail.c_str());
                break;
            }
            case KSIM_REQ_RANDOM:
                reply(KSIM_OK, host_->random(ctx));
                break;
            case KSIM_REQ_RADIO_GET: {
                int64_t value = 0;
                const int status = host_->radio_get(ctx, r.u32(), &value);
                reply(status, value);
                break;
            }
            case KSIM_REQ_RADIO_SET: {
                const uint32_t param = r.u32();
                reply(host_->radio_set(ctx, param, r.i64()), 0);
                break;
            }
            case KSIM_MSG_DONE:
                return r.u8() != 0;
            default:
                fail("protocol error: unexpected message type " + std::to_string(type));
            }
        } catch (const std::runtime_error &e) {
            const std::string what = e.what();
            if (what.rfind(node_name(node_id_), 0) == 0) {
                throw; // already ours
            }
            fail("protocol error: " + what);
        }
    }
}

bool RemoteNode::create(const ksim_host_t *host, Time now, const std::string &config)
{
    host_ = host;
    wire::Writer w(KSIM_EV_CREATE);
    w.u64(now).u32(node_id_).u32(static_cast<uint32_t>(host->mtu)).str(config);
    return exchange(w);
}

void RemoteNode::on_rx(const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta)
{
    wire::Writer w(KSIM_EV_RX);
    w.u64(host_->now_us(host_->ctx)).f64(meta->rssi_dbm).f64(meta->snr_db).bytes(buf, len);
    exchange(w);
}

void RemoteNode::on_tx_done()
{
    wire::Writer w(KSIM_EV_TX_DONE);
    w.u64(host_->now_us(host_->ctx));
    exchange(w);
}

void RemoteNode::on_timer(uint32_t timer_id)
{
    wire::Writer w(KSIM_EV_TIMER);
    w.u64(host_->now_us(host_->ctx)).u32(timer_id);
    exchange(w);
}

void RemoteNode::destroy()
{
    wire::Writer w(KSIM_EV_DESTROY);
    w.u64(host_->now_us(host_->ctx));
    exchange(w);
    sock_.close();
    proc_.wait_or_kill(2000);
}

// ---- RemoteHub ----

RemoteHub::RemoteHub(uint16_t port, std::string program_dir)
    : listener_(net::Socket::listen_loopback(port)), program_dir_(std::move(program_dir))
{
    port_ = listener_.local_port();
}

std::map<uint32_t, std::unique_ptr<RemoteNode>>
RemoteHub::connect(const std::vector<const NodeSpec *> &nodes, const WaitCallback &on_wait)
{
    using clock = std::chrono::steady_clock;
    struct Pending {
        const NodeSpec *spec = nullptr;
        Process proc;
        std::optional<clock::time_point> deadline;
        int timeout_ms = -1; // for answers during the run; < 0 = unlimited
    };

    std::map<uint32_t, Pending> pending;
    for (const NodeSpec *spec : nodes) {
        const ProcessSpec &ps = *spec->process;
        Pending p;
        p.spec = spec;
        if (!ps.spawn.empty()) {
            p.timeout_ms = ps.timeout_s > 0.0 ? to_ms(ps.timeout_s) : kDefaultSpawnTimeoutMs;
            p.deadline = clock::now() + std::chrono::milliseconds(p.timeout_ms);
            const Process::Env env = {{KSIM_ENV_PORT, std::to_string(port_)},
                                      {KSIM_ENV_NODE_ID, std::to_string(spec->id)}};
            try {
                p.proc = Process::spawn(ps.spawn, env, program_dir_);
            } catch (const std::exception &e) {
                throw std::runtime_error(node_name(spec->id) + ": " + e.what());
            }
        } else {
            if (ps.timeout_s > 0.0) {
                p.timeout_ms = to_ms(ps.timeout_s);
                p.deadline = clock::now() + std::chrono::milliseconds(p.timeout_ms);
            }
            if (on_wait) {
                on_wait(spec->id, port_);
            }
        }
        pending.emplace(spec->id, std::move(p));
    }

    std::map<uint32_t, std::unique_ptr<RemoteNode>> connected;
    while (!pending.empty()) {
        for (auto &[id, p] : pending) {
            if (auto code = p.proc.exit_code()) {
                throw std::runtime_error(node_name(id) + ": process '" + p.spec->process->spawn[0] +
                                         "' exited with code " + std::to_string(*code) +
                                         " before connecting");
            }
            if (p.deadline && clock::now() > *p.deadline) {
                throw std::runtime_error(node_name(id) + ": the stack process did not connect to " +
                                         "127.0.0.1:" + std::to_string(port_) + " in time");
            }
        }

        net::Socket sock = listener_.accept(50);
        if (!sock.valid()) {
            continue;
        }
        std::optional<wire::Reader> hello = read_message(sock, kHelloTimeoutMs);
        if (!hello || hello->u8() != KSIM_MSG_HELLO) {
            send_error(sock, "expected HELLO");
            throw std::runtime_error("a process connected to port " + std::to_string(port_) +
                                     " without sending HELLO");
        }
        const uint16_t protocol = hello->u16();
        const uint16_t abi = hello->u16();
        const uint32_t id = hello->u32();
        const uint8_t mode = hello->u8();
        const std::string name = hello->str();
        const std::string version = hello->str();

        auto it = pending.find(id);
        std::string problem;
        if (protocol != KSIM_REMOTE_PROTOCOL_VERSION) {
            problem = "speaks remote protocol " + std::to_string(protocol) + ", ksim speaks " +
                      std::to_string(KSIM_REMOTE_PROTOCOL_VERSION);
        } else if (abi < KSIM_ABI_VERSION_MIN_REMOTE || abi > KSIM_ABI_VERSION) {
            problem = "was built for stack ABI " + std::to_string(abi) + ", ksim uses " +
                      std::to_string(KSIM_ABI_VERSION);
        } else if (mode != KSIM_REMOTE_MODE_LOCKSTEP) {
            problem = "asks for mode " + std::to_string(mode) + "; only lockstep (0) exists";
        } else if (it == pending.end()) {
            problem = "is not an out-of-process node of this scenario (or connected twice)";
        } else if (name != it->second.spec->stack) {
            problem = "serves stack '" + name + "' but the scenario expects '" +
                      it->second.spec->stack + "'";
        }
        if (!problem.empty()) {
            send_error(sock, problem);
            throw std::runtime_error(node_name(id) + ": the stack process " + problem);
        }

        Pending &p = it->second;
        connected.emplace(id, std::make_unique<RemoteNode>(id, std::move(sock), std::move(p.proc),
                                                           p.timeout_ms, name, version));
        pending.erase(it);
    }
    return connected;
}

} // namespace ksim

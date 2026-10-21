/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <ostream>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ksim/rng.hpp"
#include "payload_scripts.hpp"
#include "remote/remote.hpp"

namespace ksim {

const char *version()
{
    return KSIM_VERSION;
}

namespace {

constexpr uint64_t kTagNode = 0x6E6F6465u;   // per-node random stream
constexpr uint64_t kTagShadow = 0x73686164u; // per-link shadowing

// Trace format version in the meta line: 1 (no "format" key) initial, 2 frame bytes in "tx".
constexpr int kTraceFormat = 2;

// Signal levels are rounded before any decision so libm differences cannot flip an outcome.
double round_db(double v)
{
    return std::round(v * 100.0) / 100.0;
}

bool overlaps(Time s1, Time e1, Time s2, Time e2)
{
    return s1 < e2 && s2 < e1;
}

std::string to_hex(const uint8_t *buf, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string out(2 * len, '0');
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[buf[i] >> 4];
        out[2 * i + 1] = digits[buf[i] & 0xF];
    }
    return out;
}

} // namespace

struct Simulator::Impl {
    struct Node {
        Impl *sim = nullptr;
        NodeSpec spec;
        RadioConfig radio; // current settings (stacks may change them)
        bool rx_on = true;
        Time rx_since = 0;  // receiver on and tuned unchanged since this time
        Time rx_off_at = 0; // when the receiver was last switched off
        ksim_host_t host{};
        const ksim_stack_t *stack = nullptr;
        RemoteNode *remote = nullptr; // set for out-of-process nodes
        std::string stack_name;
        std::string stack_version;
        void *inst = nullptr;
        Rng rng{0, 0};
        std::map<uint32_t, uint64_t> timers; // timer id -> generation of the live expiry
        uint64_t timer_gen = 0;
        bool transmitting = false;
        NodeStats stats;
    };

    struct Transmission {
        uint64_t id = 0;
        size_t sender = 0;
        Time start = 0;
        Time end = 0;
        RadioConfig radio; // sender's settings at the start
        std::vector<uint8_t> data;
    };

    Scenario scn;
    std::map<std::string, const ksim_stack_t *> stacks;
    std::ostream *trace_out = nullptr;
    EventQueue queue;
    std::vector<std::unique_ptr<Node>> nodes;
    std::map<uint32_t, size_t> index_of; // node id -> position in nodes
    std::deque<Transmission> recent;     // every frame that may still overlap a live one
    Time max_airtime = 0;
    uint64_t next_tx_id = 0;
    Summary sum;
    uint64_t hash = 0xCBF29CE484222325ull;
    enum class Phase { Idle, Running, Finished } phase = Phase::Idle;

    // out-of-process nodes
    uint16_t remote_port = 0;
    std::string program_dir;
    std::function<void(uint32_t, uint16_t)> on_wait;
    std::unique_ptr<RemoteHub> hub;
    std::map<uint32_t, std::unique_ptr<RemoteNode>> remotes;

    std::unique_ptr<PayloadScripts> scripts; // Python payload functions, if any node has one
    std::string fatal;                       // set by a host function to end the run with an error

    // Free what was created after an error or an early end; processes die with their connections.
    void abort()
    {
        for (auto &n : nodes) {
            if (n->inst != nullptr && n->remote == nullptr) {
                n->stack->destroy(n->inst);
            }
            n->inst = nullptr;
        }
        remotes.clear();
        phase = Phase::Finished;
    }

    ~Impl()
    {
        if (phase == Phase::Running) {
            abort(); // a live run stopped without finish()
        }
    }

    void emit(const nlohmann::ordered_json &line)
    {
        const std::string text = line.dump();
        for (char c : text) {
            hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
        }
        hash = (hash ^ static_cast<unsigned char>('\n')) * 0x100000001B3ull;
        if (trace_out != nullptr) {
            *trace_out << text << '\n';
        }
    }

    nlohmann::ordered_json event(const char *name, const Node &n) const
    {
        nlohmann::ordered_json j;
        j["t"] = queue.now();
        j["ev"] = name;
        j["node"] = n.spec.id;
        return j;
    }

    double shadowing_db(uint32_t a, uint32_t b) const
    {
        if (scn.path_loss.shadowing_sigma_db == 0.0) {
            return 0.0;
        }
        const uint64_t lo = std::min(a, b);
        const uint64_t hi = std::max(a, b);
        Rng r(scn.seed, mix(kTagShadow, (lo << 32) | hi));
        return r.normal() * scn.path_loss.shadowing_sigma_db;
    }

    double rssi(const NodeSpec &from, const RadioConfig &tx, const NodeSpec &to) const
    {
        const double d = distance_m(from.pos, to.pos);
        const double loss = path_loss_db(scn.path_loss, d, phy_for(tx.type).carrier_hz(tx)) +
                            shadowing_db(from.id, to.id);
        return round_db(tx.tx_power_dbm + tx.antenna_dbi + to.radio.antenna_dbi - loss);
    }

    int start_tx(Node &n, const uint8_t *buf, size_t len)
    {
        const Phy &phy = phy_for(n.radio.type);
        if (buf == nullptr || len > scn.mtu || !phy.frame_len_ok(len)) {
            return KSIM_ERR_INVALID;
        }
        if (n.transmitting) {
            return KSIM_ERR_BUSY;
        }
        Transmission tx;
        tx.id = next_tx_id++;
        tx.sender = index_of.at(n.spec.id);
        tx.start = queue.now();
        tx.end = tx.start + phy.airtime_us(n.radio, len);
        tx.radio = n.radio;
        tx.data.assign(buf, buf + len);

        const Time airtime = tx.end - tx.start;
        max_airtime = std::max(max_airtime, airtime);
        n.transmitting = true;
        n.stats.tx++;
        n.stats.airtime_us += airtime;
        sum.tx++;

        auto j = event("tx", n);
        j["len"] = len;
        j["airtime"] = airtime;
        j["data"] = to_hex(buf, len);
        emit(j);

        recent.push_back(tx);
        queue.schedule(tx.end, [this, tx] { finish_tx(tx); });
        return KSIM_OK;
    }

    void finish_tx(const Transmission &tx)
    {
        Node &sender = *nodes[tx.sender];
        sender.transmitting = false;
        const Phy &phy = phy_for(tx.radio.type);

        for (size_t r = 0; r < nodes.size(); r++) {
            if (r == tx.sender) {
                continue;
            }
            Node &rx = *nodes[r];
            if (rx.radio.type != tx.radio.type || !phy.can_receive(rx.radio, tx.radio)) {
                continue;
            }
            const double level = rssi(sender.spec, tx.radio, rx.spec);
            const double snr = round_db(level - phy.noise_floor_dbm(tx.radio, scn.noise_figure_db));
            // an LNA in front of the receiver lowers the level it needs (signal and noise rise together,
            // so SNR-based PHYs are unaffected)
            if (!phy.decodable(tx.radio, level + rx.radio.lna_db, snr)) {
                continue; // below sensitivity: the receiver never notices
            }

            bool half_duplex = false;
            bool collision = false;
            const double capture = phy.capture_db(tx.radio, scn.capture_threshold_db);
            for (const Transmission &other : recent) {
                if (other.id == tx.id || !overlaps(other.start, other.end, tx.start, tx.end)) {
                    continue;
                }
                if (other.sender == r) {
                    half_duplex = true;
                } else if (other.radio.type == tx.radio.type &&
                           phy.interferes(tx.radio, other.radio) &&
                           rssi(nodes[other.sender]->spec, other.radio, rx.spec) > level - capture) {
                    collision = true;
                }
            }
            const bool listening = rx.rx_on && rx.rx_since <= tx.start;
            if (!half_duplex && !rx.rx_on && rx.rx_off_at <= tx.start) {
                continue; // receiver off for the whole frame: not listening, not a loss
            }

            const char *reason = nullptr;
            if (half_duplex) {
                reason = "half_duplex";
                rx.stats.lost_half_duplex++;
                sum.lost_half_duplex++;
            } else if (!listening) {
                reason = "rx_off";
                rx.stats.lost_rx_off++;
                sum.lost_rx_off++;
            } else if (collision) {
                reason = "collision";
                rx.stats.lost_collision++;
                sum.lost_collision++;
            }
            if (reason != nullptr) {
                auto j = event("lost", rx);
                j["from"] = sender.spec.id;
                j["reason"] = reason;
                emit(j);
                continue;
            }

            rx.stats.rx_ok++;
            sum.rx_ok++;
            auto j = event("rx", rx);
            j["from"] = sender.spec.id;
            j["rssi"] = level;
            j["snr"] = snr;
            emit(j);
            const ksim_rx_meta_t meta{level, snr};
            rx.stack->on_rx(rx.inst, tx.data.data(), tx.data.size(), &meta);
        }

        sender.stack->on_tx_done(sender.inst);

        const Time now = queue.now();
        while (!recent.empty() && recent.front().end + max_airtime < now) {
            recent.pop_front();
        }
    }

    int radio_get(const Node &n, uint32_t param, int64_t *value) const
    {
        if (value == nullptr) {
            return KSIM_ERR_INVALID;
        }
        switch (param) {
        case KSIM_RADIO_PHY:
            *value = static_cast<int64_t>(n.radio.type);
            return KSIM_OK;
        case KSIM_RADIO_RX_ENABLED:
            *value = n.rx_on ? 1 : 0;
            return KSIM_OK;
        case KSIM_RADIO_TX_POWER_CDBM:
            *value = static_cast<int64_t>(std::llround(n.radio.tx_power_dbm * 100.0));
            return KSIM_OK;
        default:
            return phy_for(n.radio.type).get(n.radio, param, value);
        }
    }

    int radio_set(Node &n, uint32_t param, int64_t value)
    {
        if (param == KSIM_RADIO_PHY) {
            return KSIM_ERR_UNSUPPORTED;
        }
        if (n.transmitting) {
            return KSIM_ERR_BUSY;
        }
        if (param == KSIM_RADIO_RX_ENABLED) {
            const bool on = value != 0;
            if (on && !n.rx_on) {
                n.rx_since = queue.now();
            } else if (!on && n.rx_on) {
                n.rx_off_at = queue.now();
            }
            n.rx_on = on;
            return KSIM_OK;
        }

        RadioConfig next = n.radio;
        if (param == KSIM_RADIO_TX_POWER_CDBM) {
            next.tx_power_dbm = static_cast<double>(value) / 100.0;
        } else {
            const int r = phy_for(next.type).set(next, param, value);
            if (r != KSIM_OK) {
                return r;
            }
        }
        if (n.spec.device && !n.spec.device->check(next).empty()) {
            return KSIM_ERR_INVALID;
        }
        n.radio = next;
        n.rx_since = queue.now(); // retuning loses whatever was arriving
        return KSIM_OK;
    }

    void emit_meta()
    {
        sum.versions["ksim"] = version();
        nlohmann::ordered_json j;
        j["t"] = 0;
        j["ev"] = "meta";
        j["ksim"] = version();
        j["abi"] = KSIM_ABI_VERSION;
        j["format"] = kTraceFormat;
        // hardware: boards by name, and every radio module and MCU (also those of boards)
        std::map<std::string, std::string> p, s, d, mod, mcu;
        for (const auto &n : nodes) {
            const Phy &phy = phy_for(n->radio.type);
            p[phy.name()] = std::to_string(phy.version());
            s[n->stack_name] = n->stack_version;
            if (n->spec.device) {
                const DeviceProfile &dev = *n->spec.device;
                if (dev.board) {
                    d[dev.name] = std::to_string(dev.version);
                }
                mod[dev.module] = std::to_string(dev.module_version);
                if (!dev.mcu.empty()) {
                    mcu[dev.mcu] = std::to_string(dev.mcu_version);
                }
            }
        }
        const auto add = [&](const char *key, const char *prefix,
                             const std::map<std::string, std::string> &versions) {
            nlohmann::ordered_json obj = nlohmann::ordered_json::object();
            for (const auto &[k, v] : versions) {
                obj[k] = v;
                sum.versions[prefix + k] = v;
            }
            j[key] = obj;
        };
        add("phy", "phy:", p);
        add("stack", "stack:", s);
        add("device", "device:", d);
        add("module", "module:", mod);
        add("mcu", "mcu:", mcu);
        emit(j);
    }

    // ---- ksim_host_t implementation; ctx is the Node ----

    static Node &node(void *ctx) { return *static_cast<Node *>(ctx); }

    static uint64_t h_now(void *ctx) { return node(ctx).sim->queue.now(); }

    static int h_radio_send(void *ctx, const uint8_t *buf, size_t len)
    {
        return node(ctx).sim->start_tx(node(ctx), buf, len);
    }

    static void h_timer_set(void *ctx, uint32_t timer_id, uint64_t at_us)
    {
        Node *n = &node(ctx);
        const uint64_t gen = ++n->timer_gen;
        n->timers[timer_id] = gen;
        n->sim->queue.schedule(at_us, [n, timer_id, gen] {
            auto it = n->timers.find(timer_id);
            if (it == n->timers.end() || it->second != gen) {
                return; // cancelled or replaced
            }
            n->timers.erase(it);
            n->stack->on_timer(n->inst, timer_id);
        });
    }

    static void h_timer_cancel(void *ctx, uint32_t timer_id) { node(ctx).timers.erase(timer_id); }

    static uint32_t h_random(void *ctx) { return node(ctx).rng.next_u32(); }

    static void h_trace(void *ctx, const char *name, const char *detail)
    {
        Node &n = node(ctx);
        const std::string key = name != nullptr ? name : "";
        n.sim->sum.app_events[key]++;
        auto j = n.sim->event("app", n);
        j["name"] = key;
        j["detail"] = detail != nullptr ? detail : "";
        n.sim->emit(j);
    }

    static int h_radio_get(void *ctx, uint32_t param, int64_t *value)
    {
        return node(ctx).sim->radio_get(node(ctx), param, value);
    }

    static int h_radio_set(void *ctx, uint32_t param, int64_t value)
    {
        return node(ctx).sim->radio_set(node(ctx), param, value);
    }

    static int h_app_payload(void *ctx, uint32_t seq, uint8_t *buf, size_t cap)
    {
        Node &n = node(ctx);
        Impl &s = *n.sim;
        if (!n.spec.payload || !s.fatal.empty()) {
            return -1;
        }
        try {
            const PayloadSpec &p = *n.spec.payload;
            const std::vector<uint8_t> data =
                p.python.empty() ? p.bytes : s.scripts->call(n.spec.id, seq, s.queue.now());
            if (data.size() > cap) {
                throw std::runtime_error("node " + std::to_string(n.spec.id) + ": payload of " +
                                         std::to_string(data.size()) + " bytes, stack '" +
                                         n.stack_name + "' takes at most " + std::to_string(cap));
            }
            if (!data.empty() && buf != nullptr) {
                std::copy(data.begin(), data.end(), buf);
            }
            return static_cast<int>(data.size());
        } catch (const std::exception &e) {
            // a C stack cannot take an exception: stop the run and report after it returns
            s.fatal = e.what();
            s.queue.stop();
            return -1;
        }
    }
};

Simulator::Simulator(Scenario scenario) : impl_(std::make_unique<Impl>())
{
    std::set<uint32_t> ids;
    for (const NodeSpec &spec : scenario.nodes) {
        const std::string where = "node " + std::to_string(spec.id) + ": ";
        if (!ids.insert(spec.id).second) {
            throw std::invalid_argument("duplicate node id " + std::to_string(spec.id));
        }
        std::string err = phy_for(spec.radio.type).validate(spec.radio);
        if (err.empty() && spec.device) {
            err = spec.device->check(spec.radio);
        }
        if (!err.empty()) {
            throw std::invalid_argument(where + err);
        }
        if (spec.process && spec.process->spawn.empty() == !spec.process->wait) {
            throw std::invalid_argument(where + "process needs either spawn or wait");
        }
        if (spec.process && spec.payload) {
            throw std::invalid_argument(where +
                                        "payloads are not supported for out-of-process nodes yet");
        }
    }
    if (scenario.mtu == 0 || scenario.mtu > 255) {
        throw std::invalid_argument("mtu must be 1..255");
    }
    if (scenario.path_loss.ref_distance_m <= 0.0) {
        throw std::invalid_argument("path_loss.ref_distance_m must be > 0");
    }
    impl_->scn = std::move(scenario);
}

Simulator::~Simulator() = default;

void Simulator::register_stack(const ksim_stack_t *stack)
{
    if (stack == nullptr || stack->name == nullptr) {
        throw std::invalid_argument("stack without a name");
    }
    if (stack->abi_version != KSIM_ABI_VERSION) {
        throw std::invalid_argument(std::string("stack '") + stack->name +
                                    "' was built for another ABI version");
    }
    if (!impl_->stacks.emplace(stack->name, stack).second) {
        throw std::invalid_argument(std::string("stack '") + stack->name + "' registered twice");
    }
}

void Simulator::set_trace(std::ostream *out)
{
    impl_->trace_out = out;
}

void Simulator::set_remote_options(uint16_t port, std::string program_dir,
                                   std::function<void(uint32_t, uint16_t)> on_wait)
{
    impl_->remote_port = port;
    impl_->program_dir = std::move(program_dir);
    impl_->on_wait = std::move(on_wait);
}

Summary Simulator::run()
{
    start();
    advance_to(impl_->scn.duration_us);
    return finish();
}

void Simulator::start()
{
    Impl &s = *impl_;
    if (s.phase != Impl::Phase::Idle) {
        throw std::logic_error("Simulator::run/start called twice");
    }
    s.phase = Impl::Phase::Running;

    // Build every node first so a stack may transmit from create().
    std::vector<const NodeSpec *> remote_specs;
    for (const NodeSpec &spec : s.scn.nodes) {
        auto n = std::make_unique<Impl::Node>();
        if (spec.process) {
            n->stack = RemoteNode::vtable(); // name and version come from the process's HELLO
            remote_specs.push_back(&spec);
        } else {
            auto it = s.stacks.find(spec.stack);
            if (it == s.stacks.end()) {
                throw std::runtime_error("node " + std::to_string(spec.id) + ": unknown stack '" +
                                         spec.stack + "'");
            }
            n->stack = it->second;
            n->stack_name = n->stack->name;
            n->stack_version = n->stack->version != nullptr ? n->stack->version : "";
        }
        n->sim = &s;
        n->spec = spec;
        n->radio = spec.radio;
        n->rng = Rng(s.scn.seed, mix(kTagNode, spec.id));
        n->stats.id = spec.id;
        n->host.ctx = n.get();
        n->host.node_id = spec.id;
        n->host.mtu = s.scn.mtu;
        n->host.now_us = Impl::h_now;
        n->host.radio_send = Impl::h_radio_send;
        n->host.timer_set = Impl::h_timer_set;
        n->host.timer_cancel = Impl::h_timer_cancel;
        n->host.random = Impl::h_random;
        n->host.trace = Impl::h_trace;
        n->host.radio_get = Impl::h_radio_get;
        n->host.radio_set = Impl::h_radio_set;
        n->host.app_payload = Impl::h_app_payload;
        s.index_of[spec.id] = s.nodes.size();
        s.nodes.push_back(std::move(n));
    }

    if (!remote_specs.empty()) {
        s.hub = std::make_unique<RemoteHub>(s.remote_port, s.program_dir);
        s.remotes = s.hub->connect(remote_specs, s.on_wait);
        for (auto &n : s.nodes) {
            if (n->spec.process) {
                n->remote = s.remotes.at(n->spec.id).get();
                n->stack_name = n->remote->stack_name();
                n->stack_version = n->remote->stack_version();
            }
        }
    }
    s.emit_meta();

    try {
        std::map<uint32_t, std::string> code;
        for (const NodeSpec &spec : s.scn.nodes) {
            if (spec.payload && !spec.payload->python.empty()) {
                code[spec.id] = spec.payload->python;
            }
        }
        if (!code.empty()) {
            s.scripts = std::make_unique<PayloadScripts>(code, s.scn.seed);
        }

        for (auto &n : s.nodes) {
            if (n->remote != nullptr) {
                n->inst = n->remote->create(&n->host, s.queue.now(), n->spec.config) ? n->remote
                                                                                       : nullptr;
            } else {
                n->inst = n->stack->create(&n->host, n->spec.config.c_str());
            }
            if (n->inst == nullptr) {
                throw std::runtime_error("node " + std::to_string(n->spec.id) + ": stack '" +
                                         n->spec.stack + "' rejected config '" + n->spec.config +
                                         "'");
            }
        }

    } catch (...) {
        s.abort();
        throw;
    }
}

void Simulator::advance_to(Time t)
{
    Impl &s = *impl_;
    if (s.phase != Impl::Phase::Running) {
        throw std::logic_error("Simulator::advance_to needs a started, unfinished run");
    }
    try {
        s.queue.run_until(std::min(t, s.scn.duration_us));
        if (!s.fatal.empty()) {
            throw std::runtime_error(s.fatal);
        }
    } catch (...) {
        s.abort();
        throw;
    }
}

Time Simulator::now() const
{
    return impl_->queue.now();
}

Summary Simulator::finish()
{
    Impl &s = *impl_;
    if (s.phase != Impl::Phase::Running) {
        throw std::logic_error("Simulator::finish needs a started, unfinished run");
    }
    for (auto &n : s.nodes) {
        n->stack->destroy(n->inst);
        n->inst = nullptr;
        s.sum.nodes.push_back(n->stats);
    }
    s.phase = Impl::Phase::Finished;
    s.sum.duration_us = s.queue.now();
    s.sum.trace_hash = s.hash;
    return s.sum;
}

double Simulator::rssi_dbm(uint32_t from_id, uint32_t to_id) const
{
    const Impl &s = *impl_;
    auto find = [&s](uint32_t id) -> const NodeSpec & {
        for (const NodeSpec &spec : s.scn.nodes) {
            if (spec.id == id) {
                return spec;
            }
        }
        throw std::invalid_argument("unknown node id " + std::to_string(id));
    };
    const NodeSpec &a = find(from_id);
    return s.rssi(a, a.radio, find(to_id));
}

} // namespace ksim

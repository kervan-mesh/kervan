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

namespace ksim {

namespace {

constexpr uint64_t kTagNode = 0x6E6F6465u;   // per-node random stream
constexpr uint64_t kTagShadow = 0x73686164u; // per-link shadowing

// Signal levels are rounded before any decision so libm differences cannot flip an outcome.
double round_db(double v)
{
    return std::round(v * 100.0) / 100.0;
}

bool same_channel(const LoraParams &a, const LoraParams &b)
{
    return a.freq_hz == b.freq_hz && a.sf == b.sf && a.bw_hz == b.bw_hz;
}

bool overlaps(Time s1, Time e1, Time s2, Time e2)
{
    return s1 < e2 && s2 < e1;
}

} // namespace

struct Simulator::Impl {
    struct Node {
        Impl *sim = nullptr;
        NodeSpec spec;
        ksim_host_t host{};
        const ksim_stack_t *stack = nullptr;
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
        LoraParams params;
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
    bool ran = false;

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

    double rssi(const Node &from, const Node &to) const
    {
        const double d = distance_m(from.spec.pos, to.spec.pos);
        const double loss = path_loss_db(scn.path_loss, d, from.spec.radio.freq_hz) +
                            shadowing_db(from.spec.id, to.spec.id);
        return round_db(from.spec.radio.tx_power_dbm - loss);
    }

    int start_tx(Node &n, const uint8_t *buf, size_t len)
    {
        if (buf == nullptr || len == 0 || len > scn.mtu) {
            return KSIM_ERR_INVALID;
        }
        if (n.transmitting) {
            return KSIM_ERR_BUSY;
        }
        Transmission tx;
        tx.id = next_tx_id++;
        tx.sender = index_of.at(n.spec.id);
        tx.start = queue.now();
        tx.end = tx.start + lora_airtime_us(n.spec.radio, len);
        tx.params = n.spec.radio;
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
        emit(j);

        recent.push_back(tx);
        queue.schedule(tx.end, [this, tx] { finish_tx(tx); });
        return KSIM_OK;
    }

    void finish_tx(const Transmission &tx)
    {
        Node &sender = *nodes[tx.sender];
        sender.transmitting = false;

        for (size_t r = 0; r < nodes.size(); r++) {
            if (r == tx.sender) {
                continue;
            }
            Node &rx = *nodes[r];
            if (!same_channel(rx.spec.radio, tx.params)) {
                continue;
            }
            const double level = rssi(sender, rx);
            const double snr =
                round_db(level - noise_floor_dbm(tx.params.bw_hz, scn.noise_figure_db));
            if (snr < lora_snr_threshold_db(tx.params.sf)) {
                continue; // below sensitivity: the receiver never notices
            }

            const char *reason = nullptr;
            for (const Transmission &other : recent) {
                if (other.id == tx.id ||
                    !overlaps(other.start, other.end, tx.start, tx.end)) {
                    continue;
                }
                if (other.sender == r) {
                    reason = "half_duplex";
                    break;
                }
                if (same_channel(other.params, tx.params) &&
                    rssi(*nodes[other.sender], rx) > level - scn.capture_threshold_db) {
                    reason = "collision";
                    break;
                }
            }

            if (reason != nullptr) {
                auto j = event("lost", rx);
                j["from"] = sender.spec.id;
                j["reason"] = reason;
                emit(j);
                if (reason[0] == 'h') {
                    rx.stats.lost_half_duplex++;
                    sum.lost_half_duplex++;
                } else {
                    rx.stats.lost_collision++;
                    sum.lost_collision++;
                }
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
};

Simulator::Simulator(Scenario scenario) : impl_(std::make_unique<Impl>())
{
    std::set<uint32_t> ids;
    for (const NodeSpec &spec : scenario.nodes) {
        if (!ids.insert(spec.id).second) {
            throw std::invalid_argument("duplicate node id " + std::to_string(spec.id));
        }
        const std::string err = lora_validate(spec.radio);
        if (!err.empty()) {
            throw std::invalid_argument("node " + std::to_string(spec.id) + ": " + err);
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

Summary Simulator::run()
{
    Impl &s = *impl_;
    if (s.ran) {
        throw std::logic_error("Simulator::run called twice");
    }
    s.ran = true;

    // Build every node first so a stack may transmit from create().
    for (const NodeSpec &spec : s.scn.nodes) {
        auto it = s.stacks.find(spec.stack);
        if (it == s.stacks.end()) {
            throw std::runtime_error("node " + std::to_string(spec.id) + ": unknown stack '" +
                                     spec.stack + "'");
        }
        auto n = std::make_unique<Impl::Node>();
        n->sim = &s;
        n->spec = spec;
        n->stack = it->second;
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
        s.index_of[spec.id] = s.nodes.size();
        s.nodes.push_back(std::move(n));
    }
    for (auto &n : s.nodes) {
        n->inst = n->stack->create(&n->host, n->spec.config.c_str());
        if (n->inst == nullptr) {
            throw std::runtime_error("node " + std::to_string(n->spec.id) + ": stack '" +
                                     n->spec.stack + "' rejected config '" + n->spec.config +
                                     "'");
        }
    }

    s.queue.run_until(s.scn.duration_us);

    for (auto &n : s.nodes) {
        n->stack->destroy(n->inst);
        n->inst = nullptr;
        s.sum.nodes.push_back(n->stats);
    }
    s.sum.duration_us = s.scn.duration_us;
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
    Impl::Node a;
    Impl::Node b;
    a.spec = find(from_id);
    b.spec = find(to_id);
    return s.rssi(a, b);
}

} // namespace ksim

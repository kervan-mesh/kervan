/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "live.hpp"

#include <algorithm>
#include <exception>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <utility>

#include <nlohmann/json.hpp>

#include "firmware.hpp"
#include "report.hpp"

namespace ksim::serve {

namespace {

double clamp_speed(double s)
{
    return std::min(std::max(s, 0.01), 10000.0);
}

} // namespace

// Splits the trace into lines and keeps them for readers on other threads.
class LiveSession::LineSink : public std::streambuf {
public:
    LineSink(std::mutex &m, std::condition_variable &cv, size_t limit)
        : m_(m), cv_(cv), limit_(limit)
    {
    }

    // Under the session mutex.
    size_t size() const { return lines_.size(); }
    bool truncated() const { return truncated_; }
    std::vector<std::string> from(size_t first) const
    {
        return first < lines_.size()
                   ? std::vector<std::string>(lines_.begin() + static_cast<std::ptrdiff_t>(first),
                                              lines_.end())
                   : std::vector<std::string>{};
    }

    // A whole line from outside the simulator (board reports); takes the session mutex.
    void add(std::string line)
    {
        {
            const std::lock_guard<std::mutex> lock(m_);
            keep(std::move(line));
        }
        cv_.notify_all();
    }

protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        for (std::streamsize i = 0; i < n; i++) {
            put(s[i]);
        }
        return n;
    }

    int_type overflow(int_type ch) override
    {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            put(traits_type::to_char_type(ch));
        }
        return traits_type::not_eof(ch);
    }

private:
    void keep(std::string line)
    {
        if (bytes_ + line.size() > limit_) {
            truncated_ = true;
        } else {
            bytes_ += line.size() + 1;
            lines_.push_back(std::move(line));
        }
    }

    void put(char c)
    {
        if (c != '\n') {
            partial_ += c;
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(m_);
            keep(std::move(partial_));
        }
        partial_.clear();
        cv_.notify_all();
    }

    std::mutex &m_;
    std::condition_variable &cv_;
    size_t limit_;
    size_t bytes_ = 0;
    bool truncated_ = false;
    std::string partial_;
    std::vector<std::string> lines_;
};

struct LiveSession::Board {
    uint32_t node = 0;
    std::string port;
    std::unique_ptr<hw::LineIo> io;
    hw::BoardHello hello;
    double offset_us = 0; // virtual time of the board's "start" (its reports count from there)
    bool started = false;
    std::thread reader;
};

LiveSession::LiveSession(Scenario scn, const std::vector<const ksim_stack_t *> &stacks,
                         Options opt)
    : scn_(std::move(scn)), opt_(std::move(opt)), speed_(clamp_speed(opt_.speed))
{
    sink_ = std::make_unique<LineSink>(m_, cv_, opt_.max_trace_bytes);
    trace_ = std::make_unique<std::ostream>(sink_.get());
    sim_ = std::make_unique<Simulator>(scn_);
    for (const ksim_stack_t *s : stacks) {
        sim_->register_stack(s);
    }
    sim_->set_remote_options(0, opt_.program_dir);
    sim_->set_trace(trace_.get());
    sim_->start(); // errors surface here, before the thread exists
    start_wall_ = Clock::now();
    start_boards();
    thread_ = std::thread([this] { loop(); });
}

LiveSession::~LiveSession()
{
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_boards(); // if the thread never ran
}

// ---- real boards ----

// The firmware wiring of a node's MCU + module and pins, "" if unknown.
std::string LiveSession::node_wiring(const NodeSpec &n) const
{
    if (opt_.devices == nullptr || !n.device || n.device->mcu.empty()) {
        return {};
    }
    try {
        return hw::firmware_spec(*opt_.devices, n.device->mcu, n.device->module, n.pins).wiring();
    } catch (const std::exception &) {
        return {};
    }
}

void LiveSession::start_boards()
{
    std::function<std::unique_ptr<hw::LineIo>(const std::string &)> open = opt_.open_port;
    if (!open) {
        open = hw::open_serial;
    }
    for (const NodeSpec &n : scn_.nodes) {
        if (!n.hardware) {
            continue;
        }
        auto b = std::make_unique<Board>();
        b->node = n.id;
        b->port = n.hardware->port;
        const std::string where = "node " + std::to_string(n.id) + " on " + b->port + ": ";
        try {
            const std::string config = hw::config_command(n);
            b->io = open(b->port);
            b->hello = hw::hello(*b->io, opt_.board_timeout_ms);
            if (b->hello.proto != hw::kProtocol) {
                throw std::runtime_error("the board's firmware speaks protocol " +
                                         std::to_string(b->hello.proto) + ", ksim " +
                                         std::to_string(hw::kProtocol) +
                                         ": flash the firmware of this ksim version");
            }
            if (b->hello.radio != "nrf24l01p") {
                std::string msg = "the board found no nRF24 radio: check the wiring";
                const std::string expected = node_wiring(n);
                if (!b->hello.wiring.empty()) {
                    msg += "; its firmware expects " + b->hello.wiring;
                    if (!expected.empty() && expected != b->hello.wiring) {
                        msg += ", but this node is wired " + expected +
                               ": build the firmware for this wiring (Node tab, Hardware)";
                    }
                } else {
                    msg += " (ksim wiring " +
                           (b->hello.board.empty() ? std::string("rpi-pico") : b->hello.board) +
                           " nrf24l01p)";
                }
                throw std::runtime_error(msg);
            }
            hw::command(*b->io, config, "config", opt_.board_timeout_ms);
        } catch (const std::exception &e) {
            std::string msg = e.what();
            if (msg.rfind(b->port + ": ", 0) == 0) {
                msg = msg.substr(b->port.size() + 2); // the serial port named itself
            }
            stop_boards();
            throw std::runtime_error(where + msg);
        }
        boards_.push_back(std::move(b));
    }
    if (boards_.empty()) {
        return;
    }

    speed_ = 1.0; // boards run in real time
    start_wall_ = Clock::now();
    for (const auto &b : boards_) {
        const auto t0 = Clock::now();
        try {
            hw::command(*b->io, R"({"cmd":"start"})", "start", opt_.board_timeout_ms);
        } catch (const std::exception &e) {
            stop_boards();
            throw std::runtime_error("node " + std::to_string(b->node) + " on " + b->port +
                                     ": " + e.what());
        }
        b->started = true;
        const auto t1 = Clock::now();
        // the board started somewhere between sending and the answer: take the middle
        b->offset_us =
            std::chrono::duration<double, std::micro>((t0 - start_wall_) + (t1 - t0) / 2).count();
    }
    for (const auto &b : boards_) {
        nlohmann::ordered_json j;
        j["t"] = 0;
        j["ev"] = "hw";
        j["node"] = b->node;
        j["kind"] = "hello";
        j["port"] = b->port;
        j["board"] = b->hello.board;
        j["id"] = b->hello.id;
        j["fw"] = b->hello.fw;
        j["version"] = b->hello.version;
        sink_->add(j.dump());
    }
    for (size_t i = 0; i < boards_.size(); i++) {
        boards_[i]->reader = std::thread([this, i] { read_board(i); });
    }
}

void LiveSession::read_board(size_t index)
{
    Board &b = *boards_[index];
    while (!readers_stop_) {
        std::optional<std::string> line;
        try {
            line = b.io->read_line(100);
        } catch (const std::exception &e) {
            nlohmann::ordered_json j;
            j["ev"] = "error";
            j["msg"] = e.what();
            const std::lock_guard<std::mutex> lock(reports_m_);
            reports_.push_back({index, Clock::now(), j.dump()}); // gone: report it once
            return;
        }
        if (line && !line->empty()) {
            const std::lock_guard<std::mutex> lock(reports_m_);
            reports_.push_back({index, Clock::now(), std::move(*line)});
        }
    }
}

void LiveSession::stop_boards()
{
    readers_stop_ = true;
    for (const auto &b : boards_) {
        if (b->reader.joinable()) {
            b->reader.join();
        }
    }
    for (const auto &b : boards_) {
        if (b->started && b->io) {
            try {
                b->io->write_line(R"({"cmd":"stop"})");
            } catch (const std::exception &) {
                // unplugged: nothing to stop
            }
            b->started = false;
        }
    }
}

// Board reports become trace lines {"t", "ev": "hw", "node", "kind", ...the report's fields}, t
// in the simulation's time. Reports wait hold_ms so that those of all boards are sorted together.
void LiveSession::add_reports(bool all)
{
    std::vector<Report> ready;
    {
        const std::lock_guard<std::mutex> lock(reports_m_);
        const auto cutoff = Clock::now() - std::chrono::milliseconds(opt_.hold_ms);
        auto keep = std::stable_partition(reports_.begin(), reports_.end(), [&](const Report &r) {
            return all || r.received <= cutoff;
        });
        ready.assign(std::make_move_iterator(reports_.begin()), std::make_move_iterator(keep));
        reports_.erase(reports_.begin(), keep);
    }
    if (ready.empty()) {
        return;
    }

    struct Line {
        Time t;
        nlohmann::ordered_json j;
    };
    std::vector<Line> lines;
    for (const Report &r : ready) {
        const Board &b = *boards_[r.board];
        nlohmann::ordered_json in = nlohmann::ordered_json::parse(r.line, nullptr, false);
        if (!in.is_object() || !in.contains("ev") || !in.at("ev").is_string()) {
            nlohmann::ordered_json bad;
            bad["ev"] = "error";
            bad["msg"] = "unreadable line from the board: " + r.line.substr(0, 80);
            in = bad;
        }
        const std::string kind = in.at("ev").get<std::string>();
        if (kind == "ok" || kind == "pong" || kind == "hello") {
            continue; // answers, not events
        }
        double t = std::chrono::duration<double, std::micro>(r.received - start_wall_).count();
        if (in.contains("t") && in.at("t").is_number_unsigned()) {
            t = static_cast<double>(in.at("t").get<uint64_t>()) + b.offset_us;
        }
        if (t > static_cast<double>(scn_.duration_us)) {
            // boards run on for a moment after the end; compare the same time as simulated
            if (kind != "error") {
                continue;
            }
            t = static_cast<double>(scn_.duration_us);
        }
        nlohmann::ordered_json out;
        out["t"] = 0;
        out["ev"] = "hw";
        out["node"] = b.node;
        out["kind"] = kind;
        for (const auto &[key, value] : in.items()) {
            if (key != "ev" && key != "t") {
                out[key] = value;
            }
        }
        lines.push_back({static_cast<Time>(t > 0 ? t : 0), std::move(out)});
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Line &x, const Line &y) { return x.t < y.t; });
    for (Line &l : lines) {
        last_report_t_ = std::max(last_report_t_, l.t); // stay in order even when late
        l.j["t"] = last_report_t_;
        sink_->add(l.j.dump());
    }
}

// ---- control ----

bool LiveSession::pause()
{
    if (!boards_.empty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_);
    if (state_ == "running") {
        state_ = "paused";
    }
    cv_.notify_all();
    return true;
}

bool LiveSession::resume()
{
    if (!boards_.empty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_);
    if (state_ == "paused") {
        state_ = "running";
        rebase_ = true;
    }
    cv_.notify_all();
    return true;
}

bool LiveSession::set_speed(double speed)
{
    if (!boards_.empty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_);
    speed_ = clamp_speed(speed);
    rebase_ = true;
    cv_.notify_all();
    return true;
}

void LiveSession::stop()
{
    const std::lock_guard<std::mutex> lock(m_);
    stop_ = true;
    cv_.notify_all();
}

void LiveSession::loop()
{
    Clock::time_point w0 = start_wall_;
    Time v0 = 0;
    std::string error;

    while (true) {
        double speed = 1.0;
        {
            std::unique_lock<std::mutex> lock(m_);
            if (stop_) {
                break;
            }
            if (state_ == "paused") {
                cv_.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
            if (rebase_) {
                rebase_ = false;
                w0 = Clock::now();
                v0 = sim_->now(); // only this thread runs the simulator
            }
            speed = speed_;
        }

        const double elapsed_us =
            std::chrono::duration<double, std::micro>(Clock::now() - w0).count();
        const Time target = v0 + static_cast<Time>(elapsed_us * speed);
        try {
            sim_->advance_to(target);
        } catch (const std::exception &e) {
            error = e.what();
            break;
        }
        add_reports(false);
        const Time now = sim_->now();
        {
            const std::lock_guard<std::mutex> lock(m_);
            t_us_ = now;
        }
        cv_.notify_all();
        if (now >= scn_.duration_us) {
            break;
        }
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, std::chrono::milliseconds(opt_.tick_ms), [this] { return stop_; });
    }

    stop_boards();
    add_reports(true);

    std::string result;
    if (error.empty()) {
        try {
            const Summary sum = sim_->finish(); // also after stop(): the time run so far
            nlohmann::json j;
            j["summary"] = summary_json(scn_, sum);
            j["links"] = links_json(scn_, *sim_);
            nlohmann::json boards = nlohmann::json::array();
            for (const auto &b : boards_) {
                boards.push_back({{"node", b->node},
                                  {"port", b->port},
                                  {"board", b->hello.board},
                                  {"id", b->hello.id},
                                  {"fw", b->hello.fw},
                                  {"version", b->hello.version}});
            }
            j["hardware"] = boards;
            {
                const std::lock_guard<std::mutex> lock(m_);
                j["truncated"] = sink_->truncated();
            }
            result = j.dump();
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    {
        const std::lock_guard<std::mutex> lock(m_);
        t_us_ = sim_->now();
        if (error.empty()) {
            state_ = "done";
            result_ = std::move(result);
        } else {
            state_ = "error";
            error_ = error;
        }
    }
    cv_.notify_all();
}

LiveSession::Snapshot LiveSession::poll(size_t from, int timeout_ms) const
{
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return sink_->size() > from || state_ == "done" || state_ == "error";
    });
    Snapshot s;
    s.state = state_;
    s.t_us = t_us_;
    s.duration_us = scn_.duration_us;
    s.speed = speed_;
    s.hardware = !boards_.empty();
    s.error = error_;
    s.from = from;
    s.lines = sink_->from(from);
    s.total_lines = sink_->size();
    s.truncated = sink_->truncated();
    return s;
}

std::string LiveSession::result() const
{
    const std::lock_guard<std::mutex> lock(m_);
    return result_;
}

} // namespace ksim::serve

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "live.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <ostream>
#include <streambuf>
#include <utility>

#include <nlohmann/json.hpp>

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
    void put(char c)
    {
        if (c != '\n') {
            partial_ += c;
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(m_);
            if (bytes_ + partial_.size() > limit_) {
                truncated_ = true;
            } else {
                bytes_ += partial_.size() + 1;
                lines_.push_back(std::move(partial_));
            }
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
    thread_ = std::thread([this] { loop(); });
}

LiveSession::~LiveSession()
{
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LiveSession::pause()
{
    const std::lock_guard<std::mutex> lock(m_);
    if (state_ == "running") {
        state_ = "paused";
    }
    cv_.notify_all();
}

void LiveSession::resume()
{
    const std::lock_guard<std::mutex> lock(m_);
    if (state_ == "paused") {
        state_ = "running";
        rebase_ = true;
    }
    cv_.notify_all();
}

void LiveSession::set_speed(double speed)
{
    const std::lock_guard<std::mutex> lock(m_);
    speed_ = clamp_speed(speed);
    rebase_ = true;
    cv_.notify_all();
}

void LiveSession::stop()
{
    const std::lock_guard<std::mutex> lock(m_);
    stop_ = true;
    cv_.notify_all();
}

void LiveSession::loop()
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point w0 = Clock::now();
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

    std::string result;
    if (error.empty()) {
        try {
            const Summary sum = sim_->finish(); // also after stop(): the time run so far
            nlohmann::json j;
            j["summary"] = summary_json(scn_, sum);
            j["links"] = links_json(scn_, *sim_);
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

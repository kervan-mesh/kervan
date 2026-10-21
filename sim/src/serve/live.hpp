/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// A live run: the simulator paced by the wall clock (speed x real time) on its own thread, with
// the trace available line by line while it grows. Without hardware a live run is the same run as
// `ksim run` (same trace hash); real devices will join live sessions in phase 6b.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ksim/simulator.hpp"

namespace ksim::serve {

class LiveSession {
public:
    struct Options {
        double speed = 1.0;                  // virtual seconds per real second
        size_t max_trace_bytes = 256u << 20; // later lines are dropped (truncated)
        std::string program_dir;             // for out-of-process nodes
        int tick_ms = 20;                    // how often the clock advances the simulator
    };

    struct Snapshot {
        std::string state; // "running", "paused", "done" or "error"
        Time t_us = 0;     // virtual time reached
        Time duration_us = 0;
        double speed = 1.0;
        std::string error;
        size_t from = 0;                // index of lines[0]
        std::vector<std::string> lines; // trace lines (JSON) from `from` on
        size_t total_lines = 0;
        bool truncated = false;
    };

    // Starts every node (throws like Simulator::start) and the thread that paces the run.
    LiveSession(Scenario scn, const std::vector<const ksim_stack_t *> &stacks, Options opt);
    ~LiveSession(); // stops the run and waits for its thread
    LiveSession(const LiveSession &) = delete;
    LiveSession &operator=(const LiveSession &) = delete;

    void pause();
    void resume();
    void set_speed(double speed); // clamped to 0.01..10000
    void stop();                  // ends the run now; the result covers the time run so far

    // Lines from index `from`; waits up to timeout_ms while there are none and the run is live.
    Snapshot poll(size_t from, int timeout_ms) const;
    // {"summary", "links", "truncated"} once the run is done; empty before.
    std::string result() const;

private:
    class LineSink;
    void loop();

    Scenario scn_;
    Options opt_;
    std::unique_ptr<LineSink> sink_;
    std::unique_ptr<std::ostream> trace_;
    std::unique_ptr<Simulator> sim_;

    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::string state_ = "running";
    bool stop_ = false;
    bool rebase_ = false; // the clock must restart from the current time (resume, new speed)
    double speed_;
    Time t_us_ = 0;
    std::string error_;
    std::string result_;
    std::thread thread_;
};

} // namespace ksim::serve

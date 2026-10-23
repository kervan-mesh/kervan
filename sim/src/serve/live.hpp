/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// A live run: the simulator paced by the wall clock (speed x real time) on its own thread, with
// the trace available line by line while it grows. Without hardware a live run is the same run as
// `ksim run` (same trace hash). Nodes with "hardware" also run on real boards (docs/hardware.md):
// their reports join the trace as "hw" lines next to the simulation, and the session then runs in
// real time (no pause, no speed change).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "board.hpp"
#include "ksim/simulator.hpp"

namespace ksim::serve {

class LiveSession {
public:
    struct Options {
        double speed = 1.0;                  // virtual seconds per real second
        size_t max_trace_bytes = 256u << 20; // later lines are dropped (truncated)
        std::string program_dir;             // for out-of-process nodes
        int tick_ms = 20;                    // how often the clock advances the simulator
        // Opens a node's board; empty: the serial port (hw::open_serial).
        std::function<std::unique_ptr<hw::LineIo>(const std::string &port)> open_port;
        int board_timeout_ms = 3000; // for each answer while the boards start
        int hold_ms = 150; // board reports wait this long, so those of all boards come in order
        const DeviceRegistry *devices = nullptr; // to name a node's wiring in error messages
    };

    struct Snapshot {
        std::string state; // "running", "paused", "done" or "error"
        Time t_us = 0;     // virtual time reached
        Time duration_us = 0;
        double speed = 1.0;
        bool hardware = false; // real boards take part (real time only)
        std::string error;
        size_t from = 0;                // index of lines[0]
        std::vector<std::string> lines; // trace lines (JSON) from `from` on
        size_t total_lines = 0;
        bool truncated = false;
    };

    // Starts every node (throws like Simulator::start), then the boards (throws
    // std::runtime_error naming the node and port), then the thread that paces the run.
    LiveSession(Scenario scn, const std::vector<const ksim_stack_t *> &stacks, Options opt);
    ~LiveSession(); // stops the run and waits for its thread
    LiveSession(const LiveSession &) = delete;
    LiveSession &operator=(const LiveSession &) = delete;

    // false with real boards: they run in real time.
    bool pause();
    bool resume();
    bool set_speed(double speed); // clamped to 0.01..10000
    void stop();                  // ends the run now; the result covers the time run so far

    // Lines from index `from`; waits up to timeout_ms while there are none and the run is live.
    Snapshot poll(size_t from, int timeout_ms) const;
    // {"summary", "links", "truncated", "hardware"} once the run is done; empty before.
    std::string result() const;

private:
    using Clock = std::chrono::steady_clock;
    class LineSink;
    struct Board;
    struct Report {
        size_t board;
        Clock::time_point received;
        std::string line;
    };

    void loop();
    void start_boards();
    std::string node_wiring(const NodeSpec &n) const;
    void stop_boards();
    void read_board(size_t index);
    void add_reports(bool all); // board reports into the trace

    Scenario scn_;
    Options opt_;
    std::unique_ptr<LineSink> sink_;
    std::unique_ptr<std::ostream> trace_;
    std::unique_ptr<Simulator> sim_;

    std::vector<std::unique_ptr<Board>> boards_;
    std::atomic<bool> readers_stop_{false};
    std::mutex reports_m_;
    std::vector<Report> reports_;
    Time last_report_t_ = 0;
    Clock::time_point start_wall_;

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

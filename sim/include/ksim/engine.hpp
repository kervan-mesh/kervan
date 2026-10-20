/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace ksim {

using Time = uint64_t; // microseconds of virtual time

// Discrete-event queue. Events at equal times run in scheduling order (deterministic).
class EventQueue {
public:
    using Fn = std::function<void()>;

    // Events in the past run "now".
    void schedule(Time at, Fn fn)
    {
        heap_.push(Event{at < now_ ? now_ : at, next_seq_++, std::move(fn)});
    }

    // Run every event due at or before `end`; afterwards now() == end. Returns events run.
    uint64_t run_until(Time end)
    {
        uint64_t count = 0;
        stopped_ = false;
        while (!stopped_ && !heap_.empty() && heap_.top().at <= end) {
            Event ev = heap_.top();
            heap_.pop();
            now_ = ev.at;
            ev.fn();
            count++;
        }
        if (!stopped_ && end > now_) {
            now_ = end;
        }
        return count;
    }

    // Make run_until return after the current event (e.g. on a fatal error); now() stays put.
    void stop() { stopped_ = true; }

    Time now() const { return now_; }
    bool empty() const { return heap_.empty(); }

private:
    struct Event {
        Time at;
        uint64_t seq;
        Fn fn;
    };
    struct Later {
        bool operator()(const Event &a, const Event &b) const
        {
            return a.at != b.at ? a.at > b.at : a.seq > b.seq;
        }
    };

    std::priority_queue<Event, std::vector<Event>, Later> heap_;
    Time now_ = 0;
    uint64_t next_seq_ = 0;
    bool stopped_ = false;
};

} // namespace ksim

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include <vector>

#include "ksim/engine.hpp"

using ksim::EventQueue;

TEST(EventQueue, RunsInTimeOrder)
{
    EventQueue q;
    std::vector<int> order;
    q.schedule(300, [&] { order.push_back(3); });
    q.schedule(100, [&] { order.push_back(1); });
    q.schedule(200, [&] { order.push_back(2); });
    EXPECT_EQ(q.run_until(1000), 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(q.now(), 1000u);
}

TEST(EventQueue, EqualTimesRunInSchedulingOrder)
{
    EventQueue q;
    std::vector<int> order;
    for (int i = 0; i < 10; i++) {
        q.schedule(50, [&order, i] { order.push_back(i); });
    }
    q.run_until(50);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(EventQueue, StopsAtEndAndKeepsLaterEvents)
{
    EventQueue q;
    int ran = 0;
    q.schedule(100, [&] { ran++; });
    q.schedule(101, [&] { ran++; });
    q.run_until(100);
    EXPECT_EQ(ran, 1);
    EXPECT_FALSE(q.empty());
    q.run_until(200);
    EXPECT_EQ(ran, 2);
}

TEST(EventQueue, EventsCanScheduleEventsAndPastMeansNow)
{
    EventQueue q;
    std::vector<uint64_t> times;
    q.schedule(10, [&] {
        times.push_back(q.now());
        q.schedule(5, [&] { times.push_back(q.now()); }); // in the past -> now
        q.schedule(20, [&] { times.push_back(q.now()); });
    });
    q.run_until(100);
    EXPECT_EQ(times, (std::vector<uint64_t>{10, 10, 20}));
}

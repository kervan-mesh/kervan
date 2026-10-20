/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include "ksim/channel.hpp"

using ksim::PathLoss;

TEST(Channel, Distance)
{
    EXPECT_DOUBLE_EQ(ksim::distance_m({0, 0, 0}, {3, 4, 0}), 5.0);
    EXPECT_DOUBLE_EQ(ksim::distance_m({1, 1, 1}, {1, 1, 3}), 2.0);
}

TEST(Channel, FreeSpaceLoss)
{
    // 868 MHz at 1 m: 20 log10(4 pi f / c) = 31.2 dB; +20 dB per decade
    EXPECT_NEAR(ksim::free_space_loss_db(1.0, 868000000), 31.22, 0.01);
    EXPECT_NEAR(ksim::free_space_loss_db(1000.0, 868000000), 91.22, 0.01);
}

TEST(Channel, LogDistanceSlope)
{
    PathLoss m;
    m.exponent = 3.0;
    m.ref_distance_m = 1.0;
    m.ref_loss_db = 40.0;
    EXPECT_DOUBLE_EQ(ksim::path_loss_db(m, 1.0, 868000000), 40.0);
    EXPECT_DOUBLE_EQ(ksim::path_loss_db(m, 10.0, 868000000), 70.0);
    EXPECT_DOUBLE_EQ(ksim::path_loss_db(m, 1000.0, 868000000), 130.0);
}

TEST(Channel, DefaultReferenceIsFreeSpace)
{
    PathLoss m;
    m.exponent = 2.0; // free space everywhere
    EXPECT_NEAR(ksim::path_loss_db(m, 1000.0, 868000000),
                ksim::free_space_loss_db(1000.0, 868000000), 1e-9);
}

TEST(Channel, BelowReferenceDistanceIsClamped)
{
    PathLoss m;
    m.ref_distance_m = 10.0;
    m.ref_loss_db = 60.0;
    EXPECT_DOUBLE_EQ(ksim::path_loss_db(m, 0.0, 868000000), 60.0);
    EXPECT_DOUBLE_EQ(ksim::path_loss_db(m, 5.0, 868000000), 60.0);
}

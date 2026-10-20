/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/kervan.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_version_number_is_packed(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x000100u, kv_version());
}

static void test_version_string_matches_number(void)
{
    TEST_ASSERT_EQUAL_STRING("0.1.0", kv_version_string());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_version_number_is_packed);
    RUN_TEST(test_version_string_matches_number);
    return UNITY_END();
}

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/kervan.h"

#define KV_STR_(x) #x
#define KV_STR(x) KV_STR_(x)

uint32_t kv_version(void)
{
    return ((uint32_t)KV_VERSION_MAJOR << 16) |
           ((uint32_t)KV_VERSION_MINOR << 8) |
           (uint32_t)KV_VERSION_PATCH;
}

const char *kv_version_string(void)
{
    return KV_STR(KV_VERSION_MAJOR) "." KV_STR(KV_VERSION_MINOR) "." KV_STR(KV_VERSION_PATCH);
}

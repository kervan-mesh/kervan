/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/topic.h"

#include <stddef.h>

uint16_t kv_topic_id(const char *name)
{
    uint32_t h = 2166136261u;

    if (name == NULL) {
        return 0;
    }
    while (*name != '\0') {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

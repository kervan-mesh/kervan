/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_TOPIC_H
#define KERVAN_TOPIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 16-bit topic id of a topic name ("sensors/temp"): FNV-1a 32-bit folded to 16 bits.
 * Names never travel on air; every node derives the same id. If two names of one network collide,
 * pick an explicit id for one of them instead.
 */
uint16_t kv_topic_id(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_TOPIC_H */

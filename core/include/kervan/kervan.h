/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_KERVAN_H
#define KERVAN_KERVAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version, packed as 0x00MMmmpp (major, minor, patch). */
uint32_t kv_version(void);

/* Library version as a human-readable string, e.g. "0.1.0". */
const char *kv_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_KERVAN_H */

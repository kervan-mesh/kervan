/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_STATUS_H
#define KERVAN_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes shared by all core APIs. Errors are negative. */
typedef enum {
    KV_OK = 0,
    KV_ERR_INVALID_ARG = -1,   /* NULL pointer or field out of range */
    KV_ERR_NO_SPACE = -2,      /* output buffer too small */
    KV_ERR_TRUNCATED = -3,     /* input shorter than its headers claim */
    KV_ERR_VERSION = -4,       /* unsupported wire version */
    KV_ERR_MALFORMED = -5,     /* field combination not allowed on the wire */
    KV_ERR_UNSUPPORTED = -6,   /* valid but not implemented yet (e.g. secured frames) */
    KV_ERR_BUSY = -7,          /* resource in use, retry later */
    KV_ERR_TIMEOUT = -8        /* no answer within the allowed time/retries */
} kv_status_t;

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_STATUS_H */

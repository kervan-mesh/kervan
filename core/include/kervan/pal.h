/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_PAL_H
#define KERVAN_PAL_H

#include <stddef.h>
#include <stdint.h>

#include "kervan/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform Abstraction Layer: everything core needs from the outside world.
 * Each node gets its own instance, so one process can host many nodes (simulator).
 *
 * Rules for ports:
 *  - Never call into core from an interrupt; queue the event and call from the main loop.
 *  - radio_send starts a transmission and returns immediately. buf stays valid and unchanged
 *    until the port reports completion with kv_node_on_tx_done().
 */

typedef enum { KV_LOG_ERROR = 0, KV_LOG_WARN, KV_LOG_INFO, KV_LOG_DEBUG } kv_log_level_t;

typedef struct {
    void *ctx;  /* passed back to every callback */
    size_t mtu; /* largest frame the radio can send, header included */

    uint32_t (*now_ms)(void *ctx); /* monotonic milliseconds; wrap-around is fine */
    kv_status_t (*radio_send)(void *ctx, const uint8_t *buf, size_t len);
    uint32_t (*random)(void *ctx);
    void (*log)(void *ctx, kv_log_level_t level, const char *msg); /* optional, may be NULL */
} kv_pal_t;

/* Radio metadata for a received frame, as far as the port knows it. */
typedef struct {
    int16_t rssi; /* dBm */
    int8_t snr;   /* dB */
} kv_rx_meta_t;

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_PAL_H */

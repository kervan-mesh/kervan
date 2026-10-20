/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_STACK_H
#define KSIM_STACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stack adapter interface: how a protocol stack plugs into the simulator.
 * Plain C so stacks built with any compiler (or loaded from a DLL) fit. See docs/simulator.md.
 */

#define KSIM_ABI_VERSION 1u

/* radio_send results */
#define KSIM_OK 0
#define KSIM_ERR_BUSY (-1)    /* already transmitting */
#define KSIM_ERR_INVALID (-2) /* empty frame or longer than mtu */

/* What the simulator offers a stack instance. Valid until destroy. */
typedef struct {
    void *ctx;        /* pass back to every function below */
    uint32_t node_id; /* from the scenario */
    size_t mtu;       /* largest frame radio_send accepts */

    uint64_t (*now_us)(void *ctx);
    /* Start a transmission; on_tx_done follows when the frame has left the antenna. */
    int (*radio_send)(void *ctx, const uint8_t *buf, size_t len);
    /* One pending expiry per timer_id; setting it again replaces the previous one. */
    void (*timer_set)(void *ctx, uint32_t timer_id, uint64_t at_us);
    void (*timer_cancel)(void *ctx, uint32_t timer_id);
    uint32_t (*random)(void *ctx);
    /* Report an application-level event (counted in the summary, written to the trace). */
    void (*trace)(void *ctx, const char *event, const char *detail);
} ksim_host_t;

typedef struct {
    double rssi_dbm;
    double snr_db;
} ksim_rx_meta_t;

/* What a stack offers the simulator. Callbacks are never invoked from inside a host function. */
typedef struct {
    uint32_t abi_version; /* KSIM_ABI_VERSION */
    const char *name;     /* referenced by scenarios, e.g. "kervan" */

    /* config is "key=value;key=value" (possibly empty). Return NULL to reject it. */
    void *(*create)(const ksim_host_t *host, const char *config);
    void (*destroy)(void *inst);
    void (*on_rx)(void *inst, const uint8_t *buf, size_t len, const ksim_rx_meta_t *meta);
    void (*on_tx_done)(void *inst);
    void (*on_timer)(void *inst, uint32_t timer_id);
} ksim_stack_t;

#ifdef __cplusplus
}
#endif

#endif /* KSIM_STACK_H */

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
 *
 * Version history:
 *   1  initial
 *   2  radio_get/radio_set, ksim_stack_t::version
 */

#define KSIM_ABI_VERSION 2u

/* Results */
#define KSIM_OK 0
#define KSIM_ERR_BUSY (-1)        /* already transmitting */
#define KSIM_ERR_INVALID (-2)     /* bad frame length or parameter value */
#define KSIM_ERR_UNSUPPORTED (-3) /* parameter does not exist for this radio, or is read-only */

/* Radio types (KSIM_RADIO_PHY) */
#define KSIM_PHY_LORA 1
#define KSIM_PHY_NRF24 2

/*
 * Radio parameters for radio_get/radio_set. Changing one while transmitting returns
 * KSIM_ERR_BUSY; changing one while a frame is arriving makes the receiver miss that frame.
 * Values are validated against the PHY and the node's device profile.
 */
#define KSIM_RADIO_PHY 1            /* read-only: KSIM_PHY_* */
#define KSIM_RADIO_RX_ENABLED 2     /* 0/1; a frame is heard only if RX was on for all of it */
#define KSIM_RADIO_TX_POWER_CDBM 3  /* transmit power in 0.01 dBm */
#define KSIM_RADIO_FREQ_HZ 10       /* LoRa */
#define KSIM_RADIO_LORA_SF 11       /* LoRa: 7..12 */
#define KSIM_RADIO_LORA_BW_HZ 12    /* LoRa: 125000, 250000, 500000 */
#define KSIM_RADIO_NRF24_CHANNEL 20 /* nRF24: 0..125 */
#define KSIM_RADIO_NRF24_RATE 21    /* nRF24: 250000, 1000000, 2000000 bit/s */

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
    int (*radio_get)(void *ctx, uint32_t param, int64_t *value);
    int (*radio_set)(void *ctx, uint32_t param, int64_t value);
} ksim_host_t;

typedef struct {
    double rssi_dbm;
    double snr_db;
} ksim_rx_meta_t;

/* What a stack offers the simulator. Callbacks are never invoked from inside a host function. */
typedef struct {
    uint32_t abi_version; /* KSIM_ABI_VERSION */
    const char *name;     /* referenced by scenarios, e.g. "kervan" */
    const char *version;  /* model version, recorded in every trace, e.g. "1" */

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

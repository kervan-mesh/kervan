/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The board firmware's logic, independent of the board (docs/hardware-protocol.md): takes
 * command lines from ksim, runs the nRF24 in Enhanced ShockBurst like ksim's nrf24-esb stack
 * (same options, same test payloads) and reports every send and reception as a JSON line.
 * The board's main loop feeds it lines and calls kfw_app_poll as often as it can.
 */

#ifndef KFW_APP_H
#define KFW_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kfw/nrf24.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KFW_FIRMWARE "kervan-nrf24"
#define KFW_VERSION 2
#define KFW_PROTOCOL 1
#define KFW_LINE_MAX 512u /* longest command line */
#define KFW_TX_TIMEOUT_US 100000u

typedef struct {
    void (*write_line)(void *ctx, const char *line); /* one JSON object, without newline */
    void (*activity)(void *ctx); /* a frame went out or came in (LED); may be NULL */
    void *ctx;
    const char *board;    /* catalog name, e.g. "rpi-pico" */
    const char *board_id; /* unique id of this board */
    const char *wiring;   /* the pins it was built for, e.g. "SPI0 SCK=GP18 ..."; may be NULL */
} kfw_app_host_t;

/* What "config" sets; the meaning of each field is that of the nrf24-esb stack option. */
typedef struct {
    uint32_t node;
    kfw_nrf24_config_t radio;
    bool listen;
    bool has_dst;
    uint64_t send_to;
    uint32_t every_ms;
    uint32_t jitter_ms;
    uint8_t len;
    bool fixed; /* payload set: send it instead of test data */
    uint8_t payload[KFW_NRF24_MAX_PAYLOAD];
    uint8_t payload_len;
    bool no_ack;
    uint8_t ack_payload_len;
} kfw_app_config_t;

typedef struct {
    kfw_app_host_t host;
    kfw_nrf24_t radio;
    bool has_radio;
    bool configured;
    bool running;
    kfw_app_config_t cfg;

    uint64_t epoch_us; /* times in events count from "start" */
    uint64_t next_send_us;
    uint32_t seq;
    uint32_t rng;

    bool in_flight;
    uint64_t tx_start_us;
    uint8_t tx_data[KFW_NRF24_MAX_PAYLOAD];
    uint8_t tx_len;
} kfw_app_t;

/* Probes the radio; io may be NULL for a board without one. */
void kfw_app_init(kfw_app_t *a, const kfw_app_host_t *host, const kfw_nrf24_io_t *io);
/* One command line (without the newline). */
void kfw_app_line(kfw_app_t *a, const char *line, size_t len, uint64_t now_us);
void kfw_app_poll(kfw_app_t *a, uint64_t now_us);
/* The host went away (USB closed): stop sending. */
void kfw_app_disconnected(kfw_app_t *a);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A register-level model of the nRF24L01+ for tests on a PC: SPI commands, registers, FIFOs and
 * Enhanced ShockBurst between chips that share a fake_air_t (auto-ACK, retransmissions, ACK
 * payloads, MAX_RT). Timing is not modelled: a transmission completes the moment CE goes high.
 * The air's deliver() decides which attempts arrive.
 */

#ifndef KFW_FAKE_NRF24_H
#define KFW_FAKE_NRF24_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kfw/nrf24.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_NRF24_MAX_CHIPS 8u

typedef struct fake_nrf24 fake_nrf24_t;

typedef struct {
    fake_nrf24_t *chips[FAKE_NRF24_MAX_CHIPS];
    size_t n;
    /* Does attempt `attempt` (0 = first) of a frame from `from` reach `to`? NULL: always. */
    bool (*deliver)(void *ctx, const fake_nrf24_t *from, const fake_nrf24_t *to, unsigned attempt);
    void *ctx;
} fake_air_t;

typedef struct {
    uint8_t pipe;
    uint8_t len;
    uint8_t data[KFW_NRF24_MAX_PAYLOAD];
    bool no_ack;
} fake_nrf24_payload_t;

struct fake_nrf24 {
    bool present; /* false: MISO floats high, as without a module */
    bool stuck;   /* true: never finishes sending (no TX_DS, no MAX_RT) */
    uint8_t reg[0x20];
    uint8_t addr[6][5]; /* RX_ADDR_P0..P5 (P2..P5: first byte only) */
    uint8_t tx_addr[5];
    fake_nrf24_payload_t tx[3];
    uint8_t tx_n;
    fake_nrf24_payload_t rx[3];
    uint8_t rx_n;
    fake_nrf24_payload_t ack[3]; /* ACK payloads waiting, by pipe */
    uint8_t ack_n;
    bool ce;
    fake_air_t *air;
    unsigned sent;     /* frames put on air, retransmissions included */
    uint32_t delay_us; /* sum of delays the driver asked for */
};

void fake_air_init(fake_air_t *air);
/* Reset values of the datasheet; joins the air if one is given. */
void fake_nrf24_init(fake_nrf24_t *chip, fake_air_t *air);
kfw_nrf24_io_t fake_nrf24_io(fake_nrf24_t *chip);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * nRF24L01+ driver (written from the Nordic datasheet, v1.0). Portable C99: the board supplies
 * SPI, the CE pin and a delay through kfw_nrf24_io_t, so the driver also runs on a PC against a
 * register model (firmware/testing). Uses the chip's Enhanced ShockBurst: auto-ACK, automatic
 * retransmission, dynamic payload length, ACK payloads (always enabled: the sender cannot know
 * whether the receiver adds one). Polls the STATUS register; the IRQ pin is
 * not needed.
 */

#ifndef KFW_NRF24_H
#define KFW_NRF24_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFW_NRF24_MAX_PAYLOAD 32u
#define KFW_NRF24_MAX_PIPES 5u /* receive pipes 1..5; pipe 0 receives ACKs while sending */

/* Registers and commands (datasheet section 9 and 8.3.1). */
enum {
    KFW_NRF24_CONFIG = 0x00,
    KFW_NRF24_EN_AA = 0x01,
    KFW_NRF24_EN_RXADDR = 0x02,
    KFW_NRF24_SETUP_AW = 0x03,
    KFW_NRF24_SETUP_RETR = 0x04,
    KFW_NRF24_RF_CH = 0x05,
    KFW_NRF24_RF_SETUP = 0x06,
    KFW_NRF24_STATUS = 0x07,
    KFW_NRF24_OBSERVE_TX = 0x08,
    KFW_NRF24_RPD = 0x09,
    KFW_NRF24_RX_ADDR_P0 = 0x0A,
    KFW_NRF24_RX_ADDR_P1 = 0x0B,
    KFW_NRF24_TX_ADDR = 0x10,
    KFW_NRF24_FIFO_STATUS = 0x17,
    KFW_NRF24_DYNPD = 0x1C,
    KFW_NRF24_FEATURE = 0x1D
};

enum {
    KFW_NRF24_R_REGISTER = 0x00,
    KFW_NRF24_W_REGISTER = 0x20,
    KFW_NRF24_R_RX_PL_WID = 0x60,
    KFW_NRF24_R_RX_PAYLOAD = 0x61,
    KFW_NRF24_W_TX_PAYLOAD = 0xA0,
    KFW_NRF24_W_ACK_PAYLOAD = 0xA8,
    KFW_NRF24_W_TX_PAYLOAD_NOACK = 0xB0,
    KFW_NRF24_FLUSH_TX = 0xE1,
    KFW_NRF24_FLUSH_RX = 0xE2,
    KFW_NRF24_NOP = 0xFF
};

/* STATUS bits */
#define KFW_NRF24_RX_DR 0x40u
#define KFW_NRF24_TX_DS 0x20u
#define KFW_NRF24_MAX_RT 0x10u

typedef struct {
    /* One SPI transaction with CSN held low: send tx[0..n), store what comes back in rx (may be
     * NULL). */
    void (*spi)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n);
    void (*ce)(void *ctx, bool high);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
} kfw_nrf24_io_t;

typedef struct {
    uint8_t channel;    /* 0..125 */
    uint32_t data_rate; /* 250000, 1000000, 2000000 */
    uint8_t crc_bytes;  /* 1 or 2 */
    uint8_t pa_level;   /* 0..3 = -18, -12, -6, 0 dBm at the chip */
    uint8_t addr_width; /* 3..5 */
    uint64_t addr;      /* own address: pipe 1; pipes 2..5 change the lowest byte (+1..) */
    uint8_t pipes;      /* receive pipes, 1..5 */
    uint16_t ard_us;    /* auto retransmit delay, 250..4000 in steps of 250 */
    uint8_t arc;        /* auto retransmit count, 0..15 */
} kfw_nrf24_config_t;

typedef struct {
    kfw_nrf24_io_t io;
    kfw_nrf24_config_t cfg;
    bool rx_mode;
} kfw_nrf24_t;

/* Result of kfw_nrf24_tx_poll. */
typedef enum { KFW_NRF24_TX_PENDING, KFW_NRF24_TX_ACKED, KFW_NRF24_TX_FAILED } kfw_nrf24_tx_t;

/* Checks that a chip answers (a register keeps what was written), then powers down. */
bool kfw_nrf24_probe(kfw_nrf24_t *r, const kfw_nrf24_io_t *io);

/* false for values out of range. Leaves the radio powered up in standby. */
bool kfw_nrf24_configure(kfw_nrf24_t *r, const kfw_nrf24_config_t *cfg);

/* Receive on pipes 1..pipes (PRX). */
void kfw_nrf24_listen(kfw_nrf24_t *r);
/* Standby: neither sending nor receiving. */
void kfw_nrf24_standby(kfw_nrf24_t *r);
void kfw_nrf24_power_down(kfw_nrf24_t *r);

/* Start sending a payload to dst (PTX). Leaves receive mode. */
void kfw_nrf24_send(kfw_nrf24_t *r, uint64_t dst, const uint8_t *payload, uint8_t len, bool no_ack);
/* ACKED: the ACK arrived (ack_len > 0: it carried a payload, stored in ack). FAILED: no ACK
 * after every retransmission (the TX FIFO is flushed). retries: retransmissions made. */
kfw_nrf24_tx_t kfw_nrf24_tx_poll(kfw_nrf24_t *r, uint8_t *retries, uint8_t *ack, uint8_t *ack_len);

/* One received payload: true and pipe (1..5), len, data; rpd: a signal above -64 dBm was present.
 */
bool kfw_nrf24_rx_poll(kfw_nrf24_t *r, uint8_t *pipe, uint8_t *data, uint8_t *len, bool *rpd);
/* Payload sent with the next ACK on a pipe (1..5). */
void kfw_nrf24_ack_payload(kfw_nrf24_t *r, uint8_t pipe, const uint8_t *data, uint8_t len);

/* Register access, also for tests. */
uint8_t kfw_nrf24_read(kfw_nrf24_t *r, uint8_t reg);
void kfw_nrf24_write(kfw_nrf24_t *r, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif

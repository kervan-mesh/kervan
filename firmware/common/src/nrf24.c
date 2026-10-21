/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kfw/nrf24.h"

#include <string.h>

/* CONFIG bits */
#define EN_CRC 0x08u
#define CRCO 0x04u
#define PWR_UP 0x02u
#define PRIM_RX 0x01u
/* RF_SETUP bits */
#define RF_DR_LOW 0x20u
#define RF_DR_HIGH 0x08u
/* FEATURE bits */
#define EN_DPL 0x04u
#define EN_ACK_PAY 0x02u
#define EN_DYN_ACK 0x01u

#define POWER_UP_US 5000u /* datasheet: 1.5 ms (Tpd2stby); generous for slow crystals */
#define SETTLE_US 130u
#define CE_PULSE_US 15u

static void command(kfw_nrf24_t *r, uint8_t cmd, const uint8_t *data, uint8_t *out, size_t n)
{
    uint8_t tx[1u + 5u + KFW_NRF24_MAX_PAYLOAD];
    uint8_t rx[sizeof(tx)];

    tx[0] = cmd;
    if (data != NULL) {
        memcpy(&tx[1], data, n);
    } else {
        memset(&tx[1], KFW_NRF24_NOP, n);
    }
    r->io.spi(r->io.ctx, tx, rx, n + 1u);
    if (out != NULL) {
        memcpy(out, &rx[1], n);
    }
}

uint8_t kfw_nrf24_read(kfw_nrf24_t *r, uint8_t reg)
{
    uint8_t v = 0;
    command(r, (uint8_t)(KFW_NRF24_R_REGISTER | reg), NULL, &v, 1);
    return v;
}

void kfw_nrf24_write(kfw_nrf24_t *r, uint8_t reg, uint8_t value)
{
    command(r, (uint8_t)(KFW_NRF24_W_REGISTER | reg), &value, NULL, 1);
}

static void write_addr(kfw_nrf24_t *r, uint8_t reg, uint64_t addr, uint8_t width)
{
    uint8_t b[5];
    uint8_t i;
    for (i = 0; i < width; i++) {
        b[i] = (uint8_t)(addr >> (8u * i)); /* least significant byte first */
    }
    command(r, (uint8_t)(KFW_NRF24_W_REGISTER | reg), b, NULL, width);
}

static uint8_t status(kfw_nrf24_t *r)
{
    uint8_t tx = KFW_NRF24_NOP;
    uint8_t st = 0;
    r->io.spi(r->io.ctx, &tx, &st, 1);
    return st;
}

static void simple(kfw_nrf24_t *r, uint8_t cmd)
{
    r->io.spi(r->io.ctx, &cmd, NULL, 1);
}

static uint8_t config_base(const kfw_nrf24_config_t *c)
{
    return (uint8_t)(EN_CRC | (c->crc_bytes == 2u ? CRCO : 0u) | PWR_UP);
}

/* Pipe mask 0..pipes; pipe 0 (ACK reception) is added only while sending. */
static uint8_t pipes_mask(const kfw_nrf24_config_t *c)
{
    return (uint8_t)((1u << (c->pipes + 1u)) - 1u);
}

bool kfw_nrf24_probe(kfw_nrf24_t *r, const kfw_nrf24_io_t *io)
{
    bool ok;

    memset(r, 0, sizeof(*r));
    r->io = *io;
    r->io.ce(r->io.ctx, false);
    kfw_nrf24_write(r, KFW_NRF24_SETUP_AW, 0x01); /* 3 bytes */
    ok = kfw_nrf24_read(r, KFW_NRF24_SETUP_AW) == 0x01u;
    kfw_nrf24_write(r, KFW_NRF24_SETUP_AW, 0x03); /* 5 bytes */
    ok = ok && kfw_nrf24_read(r, KFW_NRF24_SETUP_AW) == 0x03u;
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, 0x00);
    return ok;
}

bool kfw_nrf24_configure(kfw_nrf24_t *r, const kfw_nrf24_config_t *c)
{
    uint8_t rate;
    uint8_t k;

    if (c->channel > 125u || (c->crc_bytes != 1u && c->crc_bytes != 2u) || c->pa_level > 3u ||
        c->addr_width < 3u || c->addr_width > 5u || c->pipes < 1u ||
        c->pipes > KFW_NRF24_MAX_PIPES || c->ard_us < 250u || c->ard_us > 4000u ||
        c->ard_us % 250u != 0u || c->arc > 15u) {
        return false;
    }
    switch (c->data_rate) {
    case 250000:
        rate = RF_DR_LOW;
        break;
    case 1000000:
        rate = 0;
        break;
    case 2000000:
        rate = RF_DR_HIGH;
        break;
    default:
        return false;
    }

    r->cfg = *c;
    r->rx_mode = false;
    r->io.ce(r->io.ctx, false);
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, (uint8_t)(config_base(c) & ~PWR_UP));
    kfw_nrf24_write(r, KFW_NRF24_SETUP_AW, (uint8_t)(c->addr_width - 2u));
    kfw_nrf24_write(r, KFW_NRF24_SETUP_RETR, (uint8_t)(((c->ard_us / 250u - 1u) << 4) | c->arc));
    kfw_nrf24_write(r, KFW_NRF24_RF_CH, c->channel);
    kfw_nrf24_write(r, KFW_NRF24_RF_SETUP, (uint8_t)(rate | (c->pa_level << 1)));
    write_addr(r, KFW_NRF24_RX_ADDR_P1, c->addr, c->addr_width);
    for (k = 1; k < c->pipes; k++) {
        /* pipes 2..5 share pipe 1's upper bytes */
        kfw_nrf24_write(r, (uint8_t)(KFW_NRF24_RX_ADDR_P1 + k), (uint8_t)((c->addr + k) & 0xFFu));
    }
    kfw_nrf24_write(r, KFW_NRF24_EN_AA, pipes_mask(c));
    kfw_nrf24_write(r, KFW_NRF24_EN_RXADDR, (uint8_t)(pipes_mask(c) & ~1u));
    kfw_nrf24_write(r, KFW_NRF24_DYNPD, pipes_mask(c));
    kfw_nrf24_write(r, KFW_NRF24_FEATURE, EN_DPL | EN_ACK_PAY | EN_DYN_ACK);
    simple(r, KFW_NRF24_FLUSH_TX);
    simple(r, KFW_NRF24_FLUSH_RX);
    kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_RX_DR | KFW_NRF24_TX_DS | KFW_NRF24_MAX_RT);
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, config_base(c));
    r->io.delay_us(r->io.ctx, POWER_UP_US);
    return true;
}

void kfw_nrf24_listen(kfw_nrf24_t *r)
{
    r->io.ce(r->io.ctx, false);
    kfw_nrf24_write(r, KFW_NRF24_EN_RXADDR, (uint8_t)(pipes_mask(&r->cfg) & ~1u));
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, (uint8_t)(config_base(&r->cfg) | PRIM_RX));
    r->io.ce(r->io.ctx, true);
    r->io.delay_us(r->io.ctx, SETTLE_US);
    r->rx_mode = true;
}

void kfw_nrf24_standby(kfw_nrf24_t *r)
{
    r->io.ce(r->io.ctx, false);
    r->rx_mode = false;
}

void kfw_nrf24_power_down(kfw_nrf24_t *r)
{
    r->io.ce(r->io.ctx, false);
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, 0x00);
    r->rx_mode = false;
}

void kfw_nrf24_send(kfw_nrf24_t *r, uint64_t dst, const uint8_t *payload, uint8_t len, bool no_ack)
{
    if (len > KFW_NRF24_MAX_PAYLOAD) {
        len = KFW_NRF24_MAX_PAYLOAD;
    }
    r->io.ce(r->io.ctx, false);
    r->rx_mode = false;
    kfw_nrf24_write(r, KFW_NRF24_CONFIG, config_base(&r->cfg));
    write_addr(r, KFW_NRF24_TX_ADDR, dst, r->cfg.addr_width);
    write_addr(r, KFW_NRF24_RX_ADDR_P0, dst, r->cfg.addr_width); /* the ACK comes on dst */
    kfw_nrf24_write(r, KFW_NRF24_EN_RXADDR, pipes_mask(&r->cfg));
    simple(r, KFW_NRF24_FLUSH_TX);
    kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_TX_DS | KFW_NRF24_MAX_RT);
    command(r, no_ack ? KFW_NRF24_W_TX_PAYLOAD_NOACK : KFW_NRF24_W_TX_PAYLOAD, payload, NULL, len);
    /* standby-I -> TX after 130 us; CE stays high until the transaction ends */
    r->io.ce(r->io.ctx, true);
    r->io.delay_us(r->io.ctx, CE_PULSE_US);
}

/* Read one payload from the RX FIFO; false (and the FIFO flushed) for a corrupt length. */
static bool read_payload(kfw_nrf24_t *r, uint8_t *data, uint8_t *len)
{
    uint8_t width = 0;
    command(r, KFW_NRF24_R_RX_PL_WID, NULL, &width, 1);
    if (width > KFW_NRF24_MAX_PAYLOAD) {
        simple(r, KFW_NRF24_FLUSH_RX);
        return false;
    }
    command(r, KFW_NRF24_R_RX_PAYLOAD, NULL, data, width);
    *len = width;
    return true;
}

kfw_nrf24_tx_t kfw_nrf24_tx_poll(kfw_nrf24_t *r, uint8_t *retries, uint8_t *ack, uint8_t *ack_len)
{
    const uint8_t st = status(r);

    *ack_len = 0;
    if ((st & (KFW_NRF24_TX_DS | KFW_NRF24_MAX_RT)) == 0u) {
        return KFW_NRF24_TX_PENDING;
    }
    r->io.ce(r->io.ctx, false);
    *retries = (uint8_t)(kfw_nrf24_read(r, KFW_NRF24_OBSERVE_TX) & 0x0Fu);
    if ((st & KFW_NRF24_MAX_RT) != 0u) {
        simple(r, KFW_NRF24_FLUSH_TX);
        kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_MAX_RT);
        kfw_nrf24_write(r, KFW_NRF24_EN_RXADDR, (uint8_t)(pipes_mask(&r->cfg) & ~1u));
        return KFW_NRF24_TX_FAILED;
    }
    if ((st & KFW_NRF24_RX_DR) != 0u && ((st >> 1) & 7u) == 0u) {
        if (!read_payload(r, ack, ack_len)) {
            *ack_len = 0;
        }
        kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_RX_DR);
    }
    kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_TX_DS);
    kfw_nrf24_write(r, KFW_NRF24_EN_RXADDR, (uint8_t)(pipes_mask(&r->cfg) & ~1u));
    return KFW_NRF24_TX_ACKED;
}

bool kfw_nrf24_rx_poll(kfw_nrf24_t *r, uint8_t *pipe, uint8_t *data, uint8_t *len, bool *rpd)
{
    const uint8_t st = status(r);
    const uint8_t p = (uint8_t)((st >> 1) & 7u);
    bool ok;

    if (p == 7u) {
        return false; /* RX FIFO empty */
    }
    *rpd = (kfw_nrf24_read(r, KFW_NRF24_RPD) & 1u) != 0u;
    ok = read_payload(r, data, len);
    kfw_nrf24_write(r, KFW_NRF24_STATUS, KFW_NRF24_RX_DR);
    *pipe = p;
    return ok && p >= 1u;
}

void kfw_nrf24_ack_payload(kfw_nrf24_t *r, uint8_t pipe, const uint8_t *data, uint8_t len)
{
    if (len > KFW_NRF24_MAX_PAYLOAD || pipe > 5u) {
        return;
    }
    command(r, (uint8_t)(KFW_NRF24_W_ACK_PAYLOAD | pipe), data, NULL, len);
}

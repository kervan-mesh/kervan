/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_nrf24.h"

#include <string.h>

#define PWR_UP 0x02u
#define PRIM_RX 0x01u

void fake_air_init(fake_air_t *air)
{
    memset(air, 0, sizeof(*air));
}

void fake_nrf24_init(fake_nrf24_t *c, fake_air_t *air)
{
    static const uint8_t p0[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    static const uint8_t p1[5] = {0xC2, 0xC2, 0xC2, 0xC2, 0xC2};
    uint8_t k;

    memset(c, 0, sizeof(*c));
    c->present = true;
    c->reg[KFW_NRF24_CONFIG] = 0x08;
    c->reg[KFW_NRF24_EN_AA] = 0x3F;
    c->reg[KFW_NRF24_EN_RXADDR] = 0x03;
    c->reg[KFW_NRF24_SETUP_AW] = 0x03;
    c->reg[KFW_NRF24_SETUP_RETR] = 0x03;
    c->reg[KFW_NRF24_RF_CH] = 0x02;
    c->reg[KFW_NRF24_RF_SETUP] = 0x0E;
    c->reg[KFW_NRF24_STATUS] = 0x0E;
    memcpy(c->addr[0], p0, 5);
    memcpy(c->addr[1], p1, 5);
    for (k = 2; k < 6; k++) {
        c->addr[k][0] = (uint8_t)(0xC1u + k);
    }
    memcpy(c->tx_addr, p0, 5);
    if (air != NULL && air->n < FAKE_NRF24_MAX_CHIPS) {
        c->air = air;
        air->chips[air->n++] = c;
    }
}

static unsigned aw(const fake_nrf24_t *c)
{
    return (unsigned)(c->reg[KFW_NRF24_SETUP_AW] & 3u) + 2u;
}

static uint8_t status(const fake_nrf24_t *c)
{
    const unsigned pipe = c->rx_n > 0u ? c->rx[0].pipe : 7u;
    return (uint8_t)((c->reg[KFW_NRF24_STATUS] & 0x70u) | (pipe << 1) | (c->tx_n == 3u ? 1u : 0u));
}

static uint8_t fifo_status(const fake_nrf24_t *c)
{
    return (uint8_t)((c->rx_n == 0u ? 0x01u : 0u) | (c->rx_n == 3u ? 0x02u : 0u) |
                     (c->tx_n == 0u ? 0x10u : 0u) | (c->tx_n == 3u ? 0x20u : 0u));
}

static bool receiving(const fake_nrf24_t *c)
{
    return c->ce && (c->reg[KFW_NRF24_CONFIG] & (PWR_UP | PRIM_RX)) == (PWR_UP | PRIM_RX);
}

/* The pipe of `c` listening on address a (aw bytes), or -1. */
static int pipe_for(const fake_nrf24_t *c, const uint8_t *a)
{
    const unsigned w = aw(c);
    int p;
    for (p = 0; p < 6; p++) {
        uint8_t full[5];
        if ((c->reg[KFW_NRF24_EN_RXADDR] & (1u << p)) == 0u) {
            continue;
        }
        memcpy(full, c->addr[p < 2 ? p : 1], 5);
        if (p >= 2) {
            full[0] = c->addr[p][0];
        }
        if (memcmp(full, a, w) == 0) {
            return p;
        }
    }
    return -1;
}

static void push(fake_nrf24_payload_t *fifo, uint8_t *n, const fake_nrf24_payload_t *p)
{
    if (*n < 3u) {
        fifo[(*n)++] = *p;
    }
}

static void pop(fake_nrf24_payload_t *fifo, uint8_t *n)
{
    if (*n > 0u) {
        memmove(&fifo[0], &fifo[1], sizeof(fifo[0]) * (size_t)(*n - 1u));
        (*n)--;
    }
}

/* One attempt; true if a receiver acknowledged (or no ACK was asked for). A receiver keeps a
 * frame once: retransmissions of it carry the same PID and are dropped. */
static bool attempt(fake_nrf24_t *c, const fake_nrf24_payload_t *f, unsigned n, bool *delivered)
{
    fake_air_t *air = c->air;
    bool acked = false;
    size_t i;

    c->sent++;
    if (air == NULL) {
        return f->no_ack;
    }
    for (i = 0; i < air->n; i++) {
        fake_nrf24_t *to = air->chips[i];
        int p;
        if (to == c || !receiving(to) || to->reg[KFW_NRF24_RF_CH] != c->reg[KFW_NRF24_RF_CH] ||
            (to->reg[KFW_NRF24_RF_SETUP] & 0x28u) != (c->reg[KFW_NRF24_RF_SETUP] & 0x28u) ||
            aw(to) != aw(c)) {
            continue;
        }
        p = pipe_for(to, c->tx_addr);
        if (p < 0 || (air->deliver != NULL && !air->deliver(air->ctx, c, to, n))) {
            continue;
        }
        if (!delivered[i]) {
            fake_nrf24_payload_t got = *f;
            got.pipe = (uint8_t)p;
            got.no_ack = false;
            push(to->rx, &to->rx_n, &got);
            to->reg[KFW_NRF24_STATUS] |= KFW_NRF24_RX_DR;
            delivered[i] = true;
        }
        if (!f->no_ack && (to->reg[KFW_NRF24_EN_AA] & (1u << p)) != 0u) {
            uint8_t k;
            acked = true;
            for (k = 0; k < to->ack_n; k++) {
                if (to->ack[k].pipe == (uint8_t)p) {
                    fake_nrf24_payload_t ap = to->ack[k];
                    ap.pipe = 0;
                    push(c->rx, &c->rx_n, &ap);
                    c->reg[KFW_NRF24_STATUS] |= KFW_NRF24_RX_DR;
                    memmove(&to->ack[k], &to->ack[k + 1u],
                            sizeof(to->ack[0]) * (size_t)(to->ack_n - k - 1u));
                    to->ack_n--;
                    break;
                }
            }
        }
    }
    return f->no_ack || acked;
}

static void try_transmit(fake_nrf24_t *c)
{
    while (!c->stuck && c->ce && (c->reg[KFW_NRF24_CONFIG] & (PWR_UP | PRIM_RX)) == PWR_UP &&
           c->tx_n > 0u && (c->reg[KFW_NRF24_STATUS] & KFW_NRF24_MAX_RT) == 0u) {
        const fake_nrf24_payload_t f = c->tx[0];
        const unsigned arc = c->reg[KFW_NRF24_SETUP_RETR] & 0x0Fu;
        const unsigned tries = f.no_ack ? 1u : arc + 1u;
        bool delivered[FAKE_NRF24_MAX_CHIPS] = {false};
        unsigned n;
        bool ok = false;

        for (n = 0; n < tries && !ok; n++) {
            ok = attempt(c, &f, n, delivered);
        }
        if (ok) {
            pop(c->tx, &c->tx_n);
            c->reg[KFW_NRF24_OBSERVE_TX] =
                (uint8_t)((c->reg[KFW_NRF24_OBSERVE_TX] & 0xF0u) | (n - 1u));
            c->reg[KFW_NRF24_STATUS] |= KFW_NRF24_TX_DS;
        } else {
            unsigned plos = (c->reg[KFW_NRF24_OBSERVE_TX] >> 4) + 1u;
            c->reg[KFW_NRF24_OBSERVE_TX] = (uint8_t)(((plos > 15u ? 15u : plos) << 4) | arc);
            c->reg[KFW_NRF24_STATUS] |= KFW_NRF24_MAX_RT; /* the payload stays in the FIFO */
        }
    }
}

static void spi(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n)
{
    fake_nrf24_t *c = (fake_nrf24_t *)ctx;
    const uint8_t cmd = tx[0];
    uint8_t out[1u + 5u + KFW_NRF24_MAX_PAYLOAD];
    size_t len = n > 0u ? n - 1u : 0u;
    size_t i;

    if (n == 0u || n > sizeof(out)) {
        return;
    }
    memset(out, 0, sizeof(out));
    out[0] = status(c);

    if (cmd < 0x20u) { /* R_REGISTER */
        const uint8_t r = cmd & 0x1Fu;
        for (i = 0; i < len; i++) {
            if (r == KFW_NRF24_RX_ADDR_P0 || r == KFW_NRF24_RX_ADDR_P1) {
                out[1u + i] = i < 5u ? c->addr[r - KFW_NRF24_RX_ADDR_P0][i] : 0u;
            } else if (r > KFW_NRF24_RX_ADDR_P1 && r <= 0x0Fu) {
                out[1u + i] = i == 0u ? c->addr[r - KFW_NRF24_RX_ADDR_P0][0] : 0u;
            } else if (r == KFW_NRF24_TX_ADDR) {
                out[1u + i] = i < 5u ? c->tx_addr[i] : 0u;
            } else if (r == KFW_NRF24_STATUS) {
                out[1u + i] = status(c);
            } else if (r == KFW_NRF24_FIFO_STATUS) {
                out[1u + i] = fifo_status(c);
            } else {
                out[1u + i] = c->reg[r];
            }
        }
    } else if (cmd < 0x40u) { /* W_REGISTER */
        const uint8_t r = cmd & 0x1Fu;
        if (r == KFW_NRF24_RX_ADDR_P0 || r == KFW_NRF24_RX_ADDR_P1) {
            memcpy(c->addr[r - KFW_NRF24_RX_ADDR_P0], &tx[1], len < 5u ? len : 5u);
        } else if (r > KFW_NRF24_RX_ADDR_P1 && r <= 0x0Fu) {
            c->addr[r - KFW_NRF24_RX_ADDR_P0][0] = len > 0u ? tx[1] : 0u;
        } else if (r == KFW_NRF24_TX_ADDR) {
            memcpy(c->tx_addr, &tx[1], len < 5u ? len : 5u);
        } else if (r == KFW_NRF24_STATUS) {
            c->reg[r] = (uint8_t)(c->reg[r] & ~(len > 0u ? tx[1] & 0x70u : 0u));
        } else if (len > 0u) {
            c->reg[r] = tx[1];
        }
        try_transmit(c);
    } else if (cmd == KFW_NRF24_R_RX_PL_WID) {
        if (len > 0u) {
            out[1] = c->rx_n > 0u ? c->rx[0].len : 0u;
        }
    } else if (cmd == KFW_NRF24_R_RX_PAYLOAD) {
        if (c->rx_n > 0u) {
            memcpy(&out[1], c->rx[0].data, len < c->rx[0].len ? len : c->rx[0].len);
            pop(c->rx, &c->rx_n);
        }
    } else if (cmd == KFW_NRF24_W_TX_PAYLOAD || cmd == KFW_NRF24_W_TX_PAYLOAD_NOACK) {
        fake_nrf24_payload_t p;
        memset(&p, 0, sizeof(p));
        p.len = (uint8_t)(len < KFW_NRF24_MAX_PAYLOAD ? len : KFW_NRF24_MAX_PAYLOAD);
        memcpy(p.data, &tx[1], p.len);
        p.no_ack = cmd == KFW_NRF24_W_TX_PAYLOAD_NOACK;
        push(c->tx, &c->tx_n, &p);
        try_transmit(c);
    } else if ((cmd & 0xF8u) == KFW_NRF24_W_ACK_PAYLOAD && (cmd & 7u) <= 5u) {
        fake_nrf24_payload_t p;
        memset(&p, 0, sizeof(p));
        p.pipe = (uint8_t)(cmd & 7u);
        p.len = (uint8_t)(len < KFW_NRF24_MAX_PAYLOAD ? len : KFW_NRF24_MAX_PAYLOAD);
        memcpy(p.data, &tx[1], p.len);
        push(c->ack, &c->ack_n, &p);
    } else if (cmd == KFW_NRF24_FLUSH_TX) {
        c->tx_n = 0;
        c->ack_n = 0; /* ACK payloads share the TX FIFO */
    } else if (cmd == KFW_NRF24_FLUSH_RX) {
        c->rx_n = 0;
    }

    if (rx != NULL) {
        if (c->present) {
            memcpy(rx, out, n);
        } else {
            memset(rx, 0xFF, n);
        }
    }
}

static void ce(void *ctx, bool high)
{
    fake_nrf24_t *c = (fake_nrf24_t *)ctx;
    c->ce = high;
    try_transmit(c);
}

static void delay(void *ctx, uint32_t us)
{
    ((fake_nrf24_t *)ctx)->delay_us += us;
}

kfw_nrf24_io_t fake_nrf24_io(fake_nrf24_t *chip)
{
    kfw_nrf24_io_t io;
    io.spi = spi;
    io.ce = ce;
    io.delay_us = delay;
    io.ctx = chip;
    return io;
}

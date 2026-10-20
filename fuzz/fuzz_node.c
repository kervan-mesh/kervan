/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Drives one node with an arbitrary sequence of events (received frames, transmission
 * completions, time steps, sends) and lets the sanitizers catch memory and UB errors.
 *
 * Input format, repeated:  op:1 [args]
 *   op % 4 == 0: receive  len:1 bytes[len]
 *   op % 4 == 1: tx done
 *   op % 4 == 2: advance  steps:1      (steps * 97 ms, then tick)
 *   op % 4 == 3: send     dst_lo:1 len:1 bytes[len]  (bit 7 of op = ack_req)
 */

#include "kervan/node.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint32_t clock_ms;

static uint32_t fz_now(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static kv_status_t fz_send(void *ctx, const uint8_t *buf, size_t len)
{
    volatile uint8_t sink = 0;
    size_t i;

    (void)ctx;
    for (i = 0; i < len; i++) {
        sink ^= buf[i]; /* touch every byte so ASan checks the range */
    }
    (void)sink;
    return KV_OK;
}

static uint32_t fz_random(void *ctx)
{
    (void)ctx;
    return 0xC0FFEEu;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    kv_pal_t pal;
    kv_node_config_t cfg;
    kv_node_t node;
    size_t pos = 0;
    uint32_t at;

    memset(&pal, 0, sizeof(pal));
    pal.mtu = 64;
    pal.now_ms = fz_now;
    pal.radio_send = fz_send;
    pal.random = fz_random;
    kv_node_config_default(&cfg);
    cfg.addr = 0x0002;
    cfg.ack_timeout_ms = 500;
    clock_ms = 0xFFFFF000u; /* exercise 32-bit wrap-around */
    if (kv_node_init(&node, &pal, &cfg) != KV_OK) {
        return 0;
    }

    while (pos < size) {
        uint8_t op = data[pos++];
        size_t len;

        switch (op % 4u) {
        case 0:
            if (pos >= size) {
                return 0;
            }
            len = data[pos++];
            if (len > size - pos) {
                len = size - pos;
            }
            kv_node_on_rx(&node, &data[pos], len, NULL);
            pos += len;
            break;
        case 1:
            kv_node_on_tx_done(&node);
            break;
        case 2:
            if (pos >= size) {
                return 0;
            }
            clock_ms += (uint32_t)data[pos++] * 97u;
            kv_node_tick(&node);
            break;
        default:
            if (size - pos < 2u) {
                return 0;
            }
            {
                uint16_t dst = data[pos++];
                len = data[pos++];
                if (len > size - pos) {
                    len = size - pos;
                }
                (void)kv_node_send(&node, dst, &data[pos], len, (op & 0x80u) != 0u, NULL);
                pos += len;
            }
            break;
        }
        (void)kv_node_next_deadline(&node, &at);
    }
    return 0;
}

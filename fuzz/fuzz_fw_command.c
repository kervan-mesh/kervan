/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* Board firmware: arbitrary command lines from USB, then a few polls of the running app. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_nrf24.h"
#include "kfw/app.h"
#include "kfw/json.h"

static void sink(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
}

static bool member(void *ctx, const char *key, size_t key_len, const kfw_json_value_t *v)
{
    (void)ctx;
    (void)key;
    (void)key_len;
    (void)v;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static fake_air_t air;
    static fake_nrf24_t chip, peer;
    static kfw_app_t app;
    kfw_app_host_t host;
    kfw_nrf24_io_t io;
    uint64_t now = 0;
    size_t start = 0;
    size_t i;

    (void)kfw_json_parse((const char *)data, size, member, NULL);

    fake_air_init(&air);
    fake_nrf24_init(&chip, &air);
    fake_nrf24_init(&peer, &air);
    memset(&host, 0, sizeof(host));
    host.write_line = sink;
    host.board = "fuzz";
    host.board_id = "0";
    io = fake_nrf24_io(&chip);
    kfw_app_init(&app, &host, &io);

    /* one command per line, polls in between */
    for (i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            kfw_app_line(&app, (const char *)&data[start], i - start, now);
            kfw_app_poll(&app, now);
            now += 50000u;
            kfw_app_poll(&app, now);
            start = i + 1u;
        }
    }
    return 0;
}

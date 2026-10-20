/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Property: any input the decoder accepts must re-encode to the same bytes
 * (except the reserved bit, which is ignored on receive and sent as 0).
 */

#include "kervan/frame.h"

#include <stdlib.h>
#include <string.h>

#define RSV_BIT 0x01u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t out[4096];
    kv_frame_t f;
    size_t len = 0;

    if (size > sizeof(out) || kv_frame_decode(data, size, &f) != KV_OK) {
        return 0;
    }
    if (kv_frame_encode(&f, out, sizeof(out), &len) != KV_OK || len != size) {
        abort();
    }
    if (out[0] != data[0] || out[1] != (data[1] & (uint8_t)~RSV_BIT) ||
        memcmp(&out[2], &data[2], size - 2u) != 0) {
        abort();
    }
    return 0;
}

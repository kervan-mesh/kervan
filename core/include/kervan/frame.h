/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_FRAME_H
#define KERVAN_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kervan/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire format: see docs/frame-format.md. */

#define KV_FRAME_VERSION 0u
#define KV_FRAME_HEADER_LEN 8u     /* fixed header */
#define KV_FRAME_FRAG_HEADER_LEN 1u /* extra byte when frag is set */

#define KV_ADDR_BROADCAST 0xFFFFu

#define KV_HOP_LIMIT_MAX 7u     /* hop_limit + hop_count never exceed this */
#define KV_HOP_LIMIT_DEFAULT 3u

#define KV_FRAG_MAX 16u /* fragments per message */

typedef enum {
    KV_FRAME_DATA = 0x0,
    KV_FRAME_ACK = 0x1,
    KV_FRAME_CTRL = 0x2,
    KV_FRAME_EXT = 0xF
} kv_frame_type_t;

typedef struct {
    uint8_t type; /* kv_frame_type_t, 0..15 */
    bool ack_req;
    bool frag; /* frag_index / frag_last are valid */
    bool sec;
    uint8_t hop_limit;
    uint8_t hop_count;
    uint8_t net_id;
    uint16_t dst;
    uint16_t src;
    uint8_t seq;
    uint8_t frag_index; /* 0..frag_last */
    uint8_t frag_last;  /* fragment count - 1, 0..15 */
} kv_frame_header_t;

typedef struct {
    kv_frame_header_t hdr;
    const uint8_t *payload; /* after decode: points into the input buffer */
    size_t payload_len;
} kv_frame_t;

/* Number of header bytes this header occupies on the wire. */
size_t kv_frame_header_len(const kv_frame_header_t *hdr);

/*
 * Serialize frame into buf (capacity cap). On success *out_len holds the frame length.
 * Rejects invalid field combinations instead of silently fixing them.
 */
kv_status_t kv_frame_encode(const kv_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len);

/*
 * Parse len bytes from buf into frame. Zero-copy: frame->payload points into buf,
 * so buf must outlive the use of frame.
 */
kv_status_t kv_frame_decode(const uint8_t *buf, size_t len, kv_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_FRAME_H */

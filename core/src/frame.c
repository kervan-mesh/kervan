/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kervan/frame.h"

#include <string.h>

/* Byte 0: ver:2 | type:4 | ack_req:1 | frag:1 */
#define B0_VER_SHIFT 6u
#define B0_TYPE_SHIFT 2u
#define B0_TYPE_MASK 0x0Fu
#define B0_ACK_REQ 0x02u
#define B0_FRAG 0x01u

/* Byte 1: hop_limit:3 | hop_count:3 | sec:1 | rsv:1 */
#define B1_HOP_LIMIT_SHIFT 5u
#define B1_HOP_COUNT_SHIFT 2u
#define B1_HOP_MASK 0x07u
#define B1_SEC 0x02u

/* Fragment byte: index:4 | last:4 */
#define FRAG_INDEX_SHIFT 4u
#define FRAG_NIBBLE_MASK 0x0Fu

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Rules shared by encode and decode; see docs/frame-format.md. */
static kv_status_t check_header(const kv_frame_header_t *hdr)
{
    if (hdr->hop_limit + hdr->hop_count > KV_HOP_LIMIT_MAX) {
        return KV_ERR_MALFORMED;
    }
    if (hdr->src == KV_ADDR_BROADCAST) {
        return KV_ERR_MALFORMED;
    }
    if (hdr->ack_req && hdr->dst == KV_ADDR_BROADCAST) {
        return KV_ERR_MALFORMED;
    }
    if (hdr->frag && hdr->frag_index > hdr->frag_last) {
        return KV_ERR_MALFORMED;
    }
    if (hdr->sec) {
        return KV_ERR_UNSUPPORTED;
    }
    return KV_OK;
}

size_t kv_frame_header_len(const kv_frame_header_t *hdr)
{
    return KV_FRAME_HEADER_LEN + (hdr->frag ? KV_FRAME_FRAG_HEADER_LEN : 0u);
}

kv_status_t kv_frame_encode(const kv_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len)
{
    const kv_frame_header_t *hdr;
    size_t hdr_len;
    kv_status_t st;

    if (frame == NULL || buf == NULL || out_len == NULL ||
        (frame->payload == NULL && frame->payload_len > 0u)) {
        return KV_ERR_INVALID_ARG;
    }
    hdr = &frame->hdr;
    if (hdr->type > B0_TYPE_MASK || hdr->hop_limit > KV_HOP_LIMIT_MAX ||
        hdr->hop_count > KV_HOP_LIMIT_MAX ||
        (hdr->frag && hdr->frag_last >= KV_FRAG_MAX)) {
        return KV_ERR_INVALID_ARG;
    }
    st = check_header(hdr);
    if (st != KV_OK) {
        return st;
    }

    hdr_len = kv_frame_header_len(hdr);
    if (frame->payload_len > cap || cap - frame->payload_len < hdr_len) {
        return KV_ERR_NO_SPACE;
    }

    buf[0] = (uint8_t)((KV_FRAME_VERSION << B0_VER_SHIFT) | ((unsigned)hdr->type << B0_TYPE_SHIFT) |
                       (hdr->ack_req ? B0_ACK_REQ : 0u) | (hdr->frag ? B0_FRAG : 0u));
    buf[1] = (uint8_t)(((unsigned)hdr->hop_limit << B1_HOP_LIMIT_SHIFT) |
                       ((unsigned)hdr->hop_count << B1_HOP_COUNT_SHIFT));
    buf[2] = hdr->net_id;
    put_u16le(&buf[3], hdr->dst);
    put_u16le(&buf[5], hdr->src);
    buf[7] = hdr->seq;
    if (hdr->frag) {
        buf[8] = (uint8_t)(((unsigned)hdr->frag_index << FRAG_INDEX_SHIFT) | hdr->frag_last);
    }
    if (frame->payload_len > 0u) {
        memcpy(&buf[hdr_len], frame->payload, frame->payload_len);
    }

    *out_len = hdr_len + frame->payload_len;
    return KV_OK;
}

kv_status_t kv_frame_decode(const uint8_t *buf, size_t len, kv_frame_t *frame)
{
    kv_frame_header_t hdr;
    size_t hdr_len;
    kv_status_t st;

    if (buf == NULL || frame == NULL) {
        return KV_ERR_INVALID_ARG;
    }
    if (len < KV_FRAME_HEADER_LEN) {
        return KV_ERR_TRUNCATED;
    }
    if ((unsigned)(buf[0] >> B0_VER_SHIFT) != KV_FRAME_VERSION) {
        return KV_ERR_VERSION;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.type = (uint8_t)((buf[0] >> B0_TYPE_SHIFT) & B0_TYPE_MASK);
    hdr.ack_req = (buf[0] & B0_ACK_REQ) != 0u;
    hdr.frag = (buf[0] & B0_FRAG) != 0u;
    hdr.hop_limit = (uint8_t)((buf[1] >> B1_HOP_LIMIT_SHIFT) & B1_HOP_MASK);
    hdr.hop_count = (uint8_t)((buf[1] >> B1_HOP_COUNT_SHIFT) & B1_HOP_MASK);
    hdr.sec = (buf[1] & B1_SEC) != 0u;
    /* rsv bit is ignored on receive */
    hdr.net_id = buf[2];
    hdr.dst = get_u16le(&buf[3]);
    hdr.src = get_u16le(&buf[5]);
    hdr.seq = buf[7];

    hdr_len = kv_frame_header_len(&hdr);
    if (len < hdr_len) {
        return KV_ERR_TRUNCATED;
    }
    if (hdr.frag) {
        hdr.frag_index = (uint8_t)((buf[8] >> FRAG_INDEX_SHIFT) & FRAG_NIBBLE_MASK);
        hdr.frag_last = (uint8_t)(buf[8] & FRAG_NIBBLE_MASK);
    }

    st = check_header(&hdr);
    if (st != KV_OK) {
        return st;
    }

    frame->hdr = hdr;
    frame->payload = &buf[hdr_len];
    frame->payload_len = len - hdr_len;
    return KV_OK;
}

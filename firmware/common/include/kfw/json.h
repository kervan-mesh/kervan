/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The JSON the board protocol needs (docs/hardware-protocol.md), nothing more: reading flat
 * objects whose values are integers, booleans or strings without escapes, and writing them.
 * No allocation.
 */

#ifndef KFW_JSON_H
#define KFW_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { KFW_JSON_INT, KFW_JSON_BOOL, KFW_JSON_STRING } kfw_json_type_t;

typedef struct {
    kfw_json_type_t type;
    int64_t i;
    bool b;
    const char *s; /* not terminated: s[0..len) */
    size_t len;
} kfw_json_value_t;

/* Called for every member; returning false stops parsing (kfw_json_parse then fails). The key
 * is not terminated. */
typedef bool (*kfw_json_member_fn)(void *ctx, const char *key, size_t key_len,
                                   const kfw_json_value_t *value);

/* Parse one flat object from text[0..len). false for anything else, including nested values,
 * fractions, escapes and trailing characters other than whitespace. */
bool kfw_json_parse(const char *text, size_t len, kfw_json_member_fn fn, void *ctx);

/* key[0..key_len) equals the C string name. */
bool kfw_json_key_is(const char *key, size_t key_len, const char *name);

/* Writing one object into a fixed buffer; on overflow the result is marked failed. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool failed;
    bool first;
} kfw_json_out_t;

void kfw_json_begin(kfw_json_out_t *o, char *buf, size_t cap);
void kfw_json_int(kfw_json_out_t *o, const char *key, int64_t v);
void kfw_json_uint(kfw_json_out_t *o, const char *key, uint64_t v);
void kfw_json_bool(kfw_json_out_t *o, const char *key, bool v);
void kfw_json_str(kfw_json_out_t *o, const char *key, const char *v); /* v: no quotes/escapes */
void kfw_json_hex(kfw_json_out_t *o, const char *key, const uint8_t *data, size_t len);
/* Closes the object and terminates the string; false if it did not fit. */
bool kfw_json_end(kfw_json_out_t *o);

/* "0102ff" -> bytes; false for odd length, non-hex digits or more than cap bytes. */
bool kfw_hex_decode(const char *s, size_t len, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif

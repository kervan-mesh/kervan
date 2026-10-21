/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kfw/json.h"

#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static bool eat(cursor_t *c, char ch)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return true;
    }
    return false;
}

static bool literal(cursor_t *c, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(c->end - c->p) >= n && memcmp(c->p, word, n) == 0) {
        c->p += n;
        return true;
    }
    return false;
}

/* A string without escapes; s and len point into the text. */
static bool string(cursor_t *c, const char **s, size_t *len)
{
    const char *start;
    if (!eat(c, '"')) {
        return false;
    }
    start = c->p;
    while (c->p < c->end && *c->p != '"') {
        const unsigned char ch = (unsigned char)*c->p;
        if (ch == '\\' || ch < 0x20u) {
            return false;
        }
        c->p++;
    }
    if (c->p >= c->end) {
        return false;
    }
    *s = start;
    *len = (size_t)(c->p - start);
    c->p++; /* closing quote */
    return true;
}

static bool integer(cursor_t *c, int64_t *out)
{
    bool neg = false;
    uint64_t v = 0;
    const uint64_t limit = (uint64_t)INT64_MAX;
    const char *digits;

    if (c->p < c->end && *c->p == '-') {
        neg = true;
        c->p++;
    }
    digits = c->p;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        const uint64_t d = (uint64_t)(*c->p - '0');
        if (v > (limit - d) / 10u) {
            return false; /* would not fit into int64 */
        }
        v = v * 10u + d;
        c->p++;
    }
    if (c->p == digits || (c->p - digits > 1 && *digits == '0')) {
        return false; /* no digits, or a leading zero */
    }
    if (c->p < c->end && (*c->p == '.' || *c->p == 'e' || *c->p == 'E')) {
        return false;
    }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

static bool value(cursor_t *c, kfw_json_value_t *v)
{
    memset(v, 0, sizeof(*v));
    skip_ws(c);
    if (c->p >= c->end) {
        return false;
    }
    if (*c->p == '"') {
        v->type = KFW_JSON_STRING;
        return string(c, &v->s, &v->len);
    }
    if (literal(c, "true")) {
        v->type = KFW_JSON_BOOL;
        v->b = true;
        return true;
    }
    if (literal(c, "false")) {
        v->type = KFW_JSON_BOOL;
        return true;
    }
    v->type = KFW_JSON_INT;
    return integer(c, &v->i);
}

bool kfw_json_parse(const char *text, size_t len, kfw_json_member_fn fn, void *ctx)
{
    cursor_t c;
    c.p = text;
    c.end = text + len;

    if (!eat(&c, '{')) {
        return false;
    }
    if (!eat(&c, '}')) {
        do {
            const char *key;
            size_t key_len;
            kfw_json_value_t v;
            if (!string(&c, &key, &key_len) || !eat(&c, ':') || !value(&c, &v) ||
                !fn(ctx, key, key_len, &v)) {
                return false;
            }
        } while (eat(&c, ','));
        if (!eat(&c, '}')) {
            return false;
        }
    }
    skip_ws(&c);
    return c.p == c.end;
}

bool kfw_json_key_is(const char *key, size_t key_len, const char *name)
{
    return strlen(name) == key_len && memcmp(key, name, key_len) == 0;
}

/* ---- writing ---- */

static void put(kfw_json_out_t *o, const char *s, size_t n)
{
    if (o->failed || o->len + n >= o->cap) { /* keep room for the terminator */
        o->failed = true;
        return;
    }
    memcpy(&o->buf[o->len], s, n);
    o->len += n;
}

static void puts_(kfw_json_out_t *o, const char *s)
{
    put(o, s, strlen(s));
}

static void key(kfw_json_out_t *o, const char *k)
{
    if (!o->first) {
        put(o, ",", 1);
    }
    o->first = false;
    put(o, "\"", 1);
    puts_(o, k);
    put(o, "\":", 2);
}

static void put_uint(kfw_json_out_t *o, uint64_t v)
{
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        put(o, &digits[--n], 1);
    }
}

void kfw_json_begin(kfw_json_out_t *o, char *buf, size_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->failed = cap == 0u;
    o->first = true;
    put(o, "{", 1);
}

void kfw_json_int(kfw_json_out_t *o, const char *k, int64_t v)
{
    key(o, k);
    if (v < 0) {
        put(o, "-", 1);
        put_uint(o, (uint64_t)(-(v + 1)) + 1u);
    } else {
        put_uint(o, (uint64_t)v);
    }
}

void kfw_json_uint(kfw_json_out_t *o, const char *k, uint64_t v)
{
    key(o, k);
    put_uint(o, v);
}

void kfw_json_bool(kfw_json_out_t *o, const char *k, bool v)
{
    key(o, k);
    puts_(o, v ? "true" : "false");
}

void kfw_json_str(kfw_json_out_t *o, const char *k, const char *v)
{
    key(o, k);
    put(o, "\"", 1);
    puts_(o, v);
    put(o, "\"", 1);
}

void kfw_json_hex(kfw_json_out_t *o, const char *k, const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t i;
    key(o, k);
    put(o, "\"", 1);
    for (i = 0; i < len; i++) {
        const char pair[2] = {digits[data[i] >> 4], digits[data[i] & 0x0Fu]};
        put(o, pair, 2);
    }
    put(o, "\"", 1);
}

bool kfw_json_end(kfw_json_out_t *o)
{
    put(o, "}", 1);
    if (o->cap > 0u) {
        o->buf[o->failed ? 0 : o->len] = '\0';
    }
    return !o->failed;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool kfw_hex_decode(const char *s, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i;
    if (len % 2u != 0u || len / 2u > cap) {
        return false;
    }
    for (i = 0; i < len; i += 2u) {
        const int hi = hex_digit(s[i]);
        const int lo = hex_digit(s[i + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2u] = (uint8_t)(hi * 16 + lo);
    }
    *out_len = len / 2u;
    return true;
}

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_FRAME_FIELDS_H
#define KSIM_FRAME_FIELDS_H

/*
 * Helper for ksim_stack_t::describe_frame: writes the JSON array of fields into the caller's
 * buffer without allocating.
 *
 *   ksim_fields_t f;
 *   ksim_fields_begin(&f, out, cap);
 *   ksim_field(&f, "src", "3", 5, 2);       // name, value, first byte, byte count
 *   ksim_field(&f, "note", "derived", -1, 0); // no bytes
 *   return ksim_fields_end(&f);             // length, or 0 if it did not fit
 */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    int ok;
    int count;
} ksim_fields_t;

static inline void ksim_fields_put_(ksim_fields_t *f, char c)
{
    if (f->len + 1u < f->cap) {
        f->out[f->len++] = c;
    } else {
        f->ok = 0;
    }
}

static inline void ksim_fields_str_(ksim_fields_t *f, const char *s)
{
    ksim_fields_put_(f, '"');
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            ksim_fields_put_(f, '\\');
            ksim_fields_put_(f, (char)c);
        } else if (c >= 0x20u) {
            ksim_fields_put_(f, (char)c);
        }
    }
    ksim_fields_put_(f, '"');
}

static inline void ksim_fields_raw_(ksim_fields_t *f, const char *s)
{
    for (; *s != '\0'; s++) {
        ksim_fields_put_(f, *s);
    }
}

static inline void ksim_fields_begin(ksim_fields_t *f, char *out, size_t cap)
{
    f->out = out;
    f->cap = cap;
    f->len = 0;
    f->ok = out != NULL && cap > 0u;
    f->count = 0;
    ksim_fields_put_(f, '[');
}

/* offset < 0: the field covers no particular bytes. */
static inline void ksim_field(ksim_fields_t *f, const char *name, const char *value, int offset,
                              int len)
{
    char num[48];
    if (f->count++ > 0) {
        ksim_fields_put_(f, ',');
    }
    ksim_fields_raw_(f, "{\"name\":");
    ksim_fields_str_(f, name);
    ksim_fields_raw_(f, ",\"value\":");
    ksim_fields_str_(f, value);
    if (offset >= 0) {
        snprintf(num, sizeof(num), ",\"offset\":%d,\"len\":%d", offset, len);
        ksim_fields_raw_(f, num);
    }
    ksim_fields_put_(f, '}');
}

static inline size_t ksim_fields_end(ksim_fields_t *f)
{
    ksim_fields_put_(f, ']');
    if (!f->ok) {
        if (f->cap > 0u && f->out != NULL) {
            f->out[0] = '\0';
        }
        return 0;
    }
    f->out[f->len] = '\0';
    return f->len;
}

#ifdef __cplusplus
}
#endif

#endif /* KSIM_FRAME_FIELDS_H */

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 MAGR contributors
//
// This file is part of MAGR (Merkle Array Graph Runtime).
//
// MAGR is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, version 3.
//
// MAGR is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
//
// Note for AGPL section 13: MAGR is designed to be used over a
// network. If you run a modified version as a service, section 13
// requires you to offer those users the corresponding source of
// your modified version. See docs/licensing.md.

/* magr/canonical.c -- implementation of the canonical encoding. */

#include "magr/canonical.h"

#include "magr/sha256.h"

#include <stdlib.h>
#include <string.h>

/* Canonical quiet NaN, IEEE-754 binary64. */
#define MAGR_CANONICAL_NAN_BITS 0x7FF8000000000000ULL

void magr_can_init(magr_can *w) {
    if (w == NULL) return;
    w->data = (uint8_t *)malloc(MAGR_CANONICAL_INIT_CAPACITY);
    w->len = 0;
    w->cap = (w->data != NULL) ? MAGR_CANONICAL_INIT_CAPACITY : 0;
    w->oom = (w->data == NULL);
}

void magr_can_free(magr_can *w) {
    if (w == NULL) return;
    free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    w->oom = 0;
}

int magr_can_ok(const magr_can *w) { return w != NULL && !w->oom; }

static int can_reserve(magr_can *w, size_t extra) {
    size_t need;
    uint8_t *p;
    if (w == NULL || w->oom) return -1;
    need = w->len + extra;
    if (need < w->len) { w->oom = 1; return -1; } /* overflow */
    if (need <= w->cap) return 0;
    {
        size_t cap = (w->cap == 0) ? MAGR_CANONICAL_INIT_CAPACITY : w->cap;
        while (cap < need) {
            if (cap > (size_t)-1 / 2) { w->oom = 1; return -1; }
            cap *= 2;
        }
        p = (uint8_t *)realloc(w->data, cap);
        if (p == NULL) { w->oom = 1; return -1; }
        w->data = p;
        w->cap = cap;
    }
    return 0;
}

int magr_can_bytes(magr_can *w, const void *data, size_t len) {
    if (w == NULL || w->oom) return -1;
    if (len == 0) return 0;
    if (data == NULL) { w->oom = 1; return -1; }
    if (can_reserve(w, len) != 0) return -1;
    memcpy(w->data + w->len, data, len);
    w->len += len;
    return 0;
}

int magr_can_bytes_framed(magr_can *w, const void *data, size_t len) {
    if (magr_can_u64(w, (uint64_t)len) != 0) return -1;
    return magr_can_bytes(w, data, len);
}

int magr_can_u8(magr_can *w, uint8_t v) { return magr_can_bytes(w, &v, 1); }

int magr_can_u16(magr_can *w, uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v);
    return magr_can_bytes(w, b, 2);
}

int magr_can_u32(magr_can *w, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)(v);
    return magr_can_bytes(w, b, 4);
}

int magr_can_u64(magr_can *w, uint64_t v) {
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (56 - 8 * i));
    return magr_can_bytes(w, b, 8);
}

int magr_can_i64(magr_can *w, int64_t v) {
    /* Two's complement, big-endian. Casting a negative int64 to uint64 is
     * well-defined modular arithmetic in C, so this needs no branch. */
    return magr_can_u64(w, (uint64_t)v);
}

static uint64_t f64_bits(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    return bits;
}

int magr_can_f64(magr_can *w, double v) {
    uint64_t bits = f64_bits(v);
    /* -0.0 has bits 0x8000000000000000; normalise it to +0.0. */
    if (bits == 0x8000000000000000ULL) bits = 0;
    /* Exponent all ones and non-zero mantissa == NaN. */
    else if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
             (bits & 0x000FFFFFFFFFFFFFULL) != 0)
        bits = MAGR_CANONICAL_NAN_BITS;
    return magr_can_u64(w, bits);
}

int magr_can_f32(magr_can *w, float v) {
    uint32_t bits;
    uint8_t b[4];
    memcpy(&bits, &v, 4);
    if (bits == 0x80000000u) bits = 0;
    else if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
        bits = 0x7FC00000u;
    b[0] = (uint8_t)(bits >> 24);
    b[1] = (uint8_t)(bits >> 16);
    b[2] = (uint8_t)(bits >> 8);
    b[3] = (uint8_t)(bits);
    return magr_can_bytes(w, b, 4);
}

int magr_can_str(magr_can *w, const char *s) {
    if (s == NULL) return magr_can_strn(w, "", 0);
    return magr_can_strn(w, s, strlen(s));
}

int magr_can_strn(magr_can *w, const char *s, size_t n) {
    if (magr_can_u64(w, (uint64_t)n) != 0) return -1;
    if (n == 0) return 0;
    if (s == NULL) { w->oom = 1; return -1; }
    return magr_can_bytes(w, s, n);
}

int magr_can_domain(magr_can *w, const char tag[5], uint64_t version) {
    if (w == NULL || tag == NULL) return -1;
    if (magr_can_bytes(w, tag, 4) != 0) return -1;
    return magr_can_u64(w, version);
}

int magr_can_shape(magr_can *w, uint32_t rank, const int64_t *dims, const char *dtype_name) {
    uint32_t i;
    if (magr_can_u8(w, (uint8_t)rank) != 0) return -1;
    if (magr_can_str(w, dtype_name == NULL ? "invalid" : dtype_name) != 0) return -1;
    if (rank > 0 && dims == NULL) { w->oom = 1; return -1; }
    for (i = 0; i < rank; ++i) {
        if (magr_can_i64(w, dims[i]) != 0) return -1;
    }
    return 0;
}

int magr_can_digest(magr_digest out, const char tag[5], uint64_t version,
                    const void *payload, size_t len) {
    magr_can w;
    int rc;
    if (out == NULL) return -1;
    magr_can_init(&w);
    if (magr_can_domain(&w, tag, version) != 0 ||
        magr_can_bytes_framed(&w, payload, len) != 0) {
        magr_can_free(&w);
        return -1;
    }
    magr_sha256(w.data, w.len, out);
    rc = magr_can_ok(&w) ? 0 : -1;
    magr_can_free(&w);
    return rc;
}

int magr_can_digest_self(magr_digest out, magr_can *w) {
    if (out == NULL || w == NULL || w->oom) return -1;
    magr_sha256(w->data, w->len, out);
    return 0;
}
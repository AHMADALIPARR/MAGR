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

/*
 * magr_uiua.c -- lowering for the documented Uiua subset.
 *
 * STATUS: source complete, not compiled, not executed. See STATUS.md.
 */

#include "magr_uiua.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The IR-construction helpers are shared with the Klong adapter. They are
 * duplicated rather than factored into a common library on purpose: the two
 * adapters are independently versioned and a shared private helper library
 * would be a third thing to keep in sync. The duplication is ~90 lines and the
 * alternative is a dependency between two components that should not have one.
 *
 * See klong-adapter/src/magr_klong.c for the commentary on why each builder
 * writes indices out explicitly.
 */

static uint32_t add_value(magr_ir *ir, const magr_ir_value *v) {
    if (ir->value_count >= MAGR_IR_MAX_OPS) return UINT32_MAX;
    ir->values[ir->value_count] = *v;
    ir->values[ir->value_count].producer = UINT32_MAX;
    return ir->value_count++;
}

static int32_t add_node(magr_ir *ir, magr_op_kind kind) {
    magr_ir_node *n;
    if (ir->node_count >= MAGR_IR_MAX_OPS) return -1;
    n = &ir->nodes[ir->node_count];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->result = -1;
    n->state = MAGR_NODE_UNRESOLVED;
    n->status = MAGR_STATUS_OK;
    return (int32_t)ir->node_count++;
}

static uint32_t bind_input(magr_ir *ir, const magr_ir_value *src) {
    uint32_t idx = add_value(ir, src);
    int32_t node;

    if (idx == UINT32_MAX) return UINT32_MAX;
    node = add_node(ir, MAGR_OP_INPUT);
    if (node < 0) return UINT32_MAX;
    ir->nodes[node].result = (int32_t)idx;
    ir->values[idx].producer = (uint32_t)node;
    return idx;
}

static uint32_t alloc_result(magr_ir *ir, int32_t node, magr_dtype dtype, uint32_t rank,
                             const int64_t *dims) {
    uint32_t idx;
    uint32_t i;
    size_t esize = magr_dtype_size(dtype);
    int64_t total = 1;
    int64_t acc;

    idx = add_value(ir, NULL);
    if (idx == UINT32_MAX) return UINT32_MAX;
    for (i = 0; i < rank; ++i) total *= dims[i];

    acc = (int64_t)esize;
    for (i = 0; i < rank; ++i) {
        ir->values[idx].strides[i] = acc;
        if (dims[i] != 0) acc *= dims[i];
    }

    ir->values[idx].dtype = (uint32_t)dtype;
    ir->values[idx].rank = rank;
    for (i = 0; i < rank; ++i) ir->values[idx].dims[i] = dims[i];
    ir->values[idx].capacity_bytes = total * (int64_t)esize;
    ir->values[idx].data = malloc((size_t)ir->values[idx].capacity_bytes);
    if (ir->values[idx].data == NULL) return UINT32_MAX;
    memset(ir->values[idx].data, 0, (size_t)ir->values[idx].capacity_bytes);
    ir->values[idx].ownership = (uint32_t)MAGR_OWNERSHIP_OWNED;
    ir->values[idx].flags = 0;
    ir->values[idx].producer = (uint32_t)node;
    ir->nodes[node].result = (int32_t)idx;
    return idx;
}

static magr_status require_vector(const magr_ir_value *v) {
    if (v == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((magr_dtype)v->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    if (v->rank != 1) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (v->data == NULL) return MAGR_STATUS_BOUNDS;
    return MAGR_STATUS_OK;
}

magr_status magr_uiua_check_column_major(const magr_ir_value *v) {
    size_t esize;
    if (v == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((magr_dtype)v->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    if (v->rank != 2) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (v->data == NULL) return MAGR_STATUS_BOUNDS;
    esize = magr_dtype_size(MAGR_DTYPE_F64);
    if (v->strides[0] != (int64_t)esize) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (v->strides[1] % (int64_t)esize != 0) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (v->strides[1] / (int64_t)esize < v->dims[0]) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

magr_status magr_uiua_lower_dot(const magr_ir_value *x, const magr_ir_value *y, magr_ir *ir) {
    magr_status st;
    uint32_t vx, vy;
    int32_t node;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_vector(x);
    if (st != MAGR_STATUS_OK) return st;
    st = require_vector(y);
    if (st != MAGR_STATUS_OK) return st;
    if (x->dims[0] != y->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    vx = bind_input(ir, x);
    vy = bind_input(ir, y);
    if (vx == UINT32_MAX || vy == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_DOT);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = vx;
    ir->nodes[node].operands[1] = vy;
    ir->nodes[node].operand_count = 2;

    if (alloc_result(ir, node, MAGR_DTYPE_F64, 0, NULL) == UINT32_MAX)
        return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_uiua_lower_axpy(const magr_ir_value *x, const magr_ir_value *y, double alpha,
                                 magr_ir *ir) {
    magr_status st;
    uint32_t vx, vy;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_vector(x);
    if (st != MAGR_STATUS_OK) return st;
    st = require_vector(y);
    if (st != MAGR_STATUS_OK) return st;
    if (x->dims[0] != y->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    if ((y->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    vx = bind_input(ir, x);
    vy = bind_input(ir, y);
    if (vx == UINT32_MAX || vy == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_AXPY);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = vx;
    ir->nodes[node].operands[1] = vy;
    ir->nodes[node].operand_count = 2;
    ir->nodes[node].scalars[0].literal = alpha;
    ir->nodes[node].scalars[0].is_literal = 1;
    ir->nodes[node].scalars[0].producer_node = -1;
    ir->nodes[node].scalar_count = 1;
    ir->nodes[node].result = (int32_t)vy;
    ir->values[vy].flags |= MAGR_ARRAY_F_MAY_ALIAS;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_uiua_lower_norm(const magr_ir_value *x, magr_ir *ir) {
    magr_status st;
    uint32_t vx;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_vector(x);
    if (st != MAGR_STATUS_OK) return st;

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    vx = bind_input(ir, x);
    if (vx == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_NRM2);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = vx;
    ir->nodes[node].operand_count = 1;

    if (alloc_result(ir, node, MAGR_DTYPE_F64, 0, NULL) == UINT32_MAX)
        return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_uiua_lower_matmul(const magr_ir_value *a, const magr_ir_value *b, magr_ir *ir) {
    magr_status st;
    uint32_t va, vb;
    int32_t node;
    int64_t dims[2];

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = magr_uiua_check_column_major(a);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_uiua_check_column_major(b);
    if (st != MAGR_STATUS_OK) return st;
    if (a->dims[1] != b->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    va = bind_input(ir, a);
    vb = bind_input(ir, b);
    if (va == UINT32_MAX || vb == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_GEMM);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = va;
    ir->nodes[node].operands[1] = vb;
    ir->nodes[node].operand_count = 2;
    ir->nodes[node].flags[0] = 0;
    ir->nodes[node].flags[1] = 0;
    ir->nodes[node].flag_count = 2;
    ir->nodes[node].scalars[0].literal = 1.0;
    ir->nodes[node].scalars[0].is_literal = 1;
    ir->nodes[node].scalars[0].producer_node = -1;
    ir->nodes[node].scalars[1].literal = 0.0;
    ir->nodes[node].scalars[1].is_literal = 1;
    ir->nodes[node].scalars[1].producer_node = -1;
    ir->nodes[node].scalar_count = 2;

    dims[0] = a->dims[0];
    dims[1] = b->dims[1];
    if (alloc_result(ir, node, MAGR_DTYPE_F64, 2, dims) == UINT32_MAX)
        return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_uiua_lower_gemv(const magr_ir_value *a, const magr_ir_value *x,
                                 const magr_ir_value *y, magr_trans trans, magr_ir *ir) {
    magr_status st;
    uint32_t va, vx, vy;
    int32_t node;
    int64_t dims[1];

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = magr_uiua_check_column_major(a);
    if (st != MAGR_STATUS_OK) return st;
    st = require_vector(x);
    if (st != MAGR_STATUS_OK) return st;
    st = require_vector(y);
    if (st != MAGR_STATUS_OK) return st;
    if ((y->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    {
        int64_t m = (trans == MAGR_TRANS_N) ? a->dims[0] : a->dims[1];
        int64_t n = (trans == MAGR_TRANS_N) ? a->dims[1] : a->dims[0];
        if (x->dims[0] != m || y->dims[0] != n) return MAGR_STATUS_SHAPE_MISMATCH;
    }

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    va = bind_input(ir, a);
    vx = bind_input(ir, x);
    vy = bind_input(ir, y);
    if (va == UINT32_MAX || vx == UINT32_MAX || vy == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_GEMV);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = va;
    ir->nodes[node].operands[1] = vx;
    ir->nodes[node].operands[2] = vy;
    ir->nodes[node].operand_count = 3;
    ir->nodes[node].flags[0] = (int64_t)trans;
    ir->nodes[node].flag_count = 1;
    ir->nodes[node].scalars[0].literal = 1.0;
    ir->nodes[node].scalars[0].is_literal = 1;
    ir->nodes[node].scalars[0].producer_node = -1;
    ir->nodes[node].scalars[1].literal = 0.0;
    ir->nodes[node].scalars[1].is_literal = 1;
    ir->nodes[node].scalars[1].producer_node = -1;
    ir->nodes[node].scalar_count = 2;

    dims[0] = y->dims[0];
    ir->nodes[node].result = (int32_t)vy;
    ir->values[vy].flags |= MAGR_ARRAY_F_MAY_ALIAS;
    ir->values[vy].rank = 1;
    ir->values[vy].dims[0] = dims[0];

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Classification and diagnostics                                      */
/* ------------------------------------------------------------------ */

/*
 * Glyph strings are UTF-8. The classifier below matches on the byte sequences
 * for the handful of glyphs in the supported subset. It exists to give a
 * human-readable diagnostic, never to build a program -- glyph layout changes
 * between Uiua releases, and a parser that has to track them would be the
 * weakest link in the adapter.
 */
static const char *normalise(const char *in, char *out, size_t cap) {
    size_t j = 0;
    if (in == NULL) { if (cap) out[0] = '\0'; return out; }
    while (*in != '\0' && j + 1 < cap) {
        if (!isspace((unsigned char)*in)) out[j++] = *in;
        in++;
    }
    out[j] = '\0';
    return out;
}

magr_status magr_uiua_classify(const char *glyphs, magr_op_kind *kind_out) {
    char buf[128];

    if (glyphs == NULL || kind_out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    normalise(glyphs, buf, sizeof(buf));

    /* UTF-8: C3 97 = U+00D7 multiply, C2 AC = U+00AC negate,
     *        E2 88 9A = U+221A sqrt, E2 8A 86 = U+220F product.
     *         C2 B1 = U+00B1 plus-minus, C3 D7 = U+00D7. */
    if (strstr(buf, "/+") != NULL && strstr(buf, "\xC3\x97") != NULL) {
        /* /+× is either a dot or a sum-of-scaled; the caller disambiguates by
         * operand shape, so report the dot form and let shape inference decide. */
        *kind_out = MAGR_OP_FORTRAN_DOT;
        return MAGR_STATUS_OK;
    }
    if (strstr(buf, "\xE2\x88\x9A") != NULL) { *kind_out = MAGR_OP_FORTRAN_NRM2; return MAGR_STATUS_OK; }
    if (strstr(buf, "\xC2\xAC") != NULL) { *kind_out = MAGR_OP_ELEMENTWISE_NEG; return MAGR_STATUS_OK; }
    if (strstr(buf, "/+") != NULL) { *kind_out = MAGR_OP_REDUCE_SUM; return MAGR_STATUS_OK; }
    if (strstr(buf, "\xC3\x97") != NULL) { *kind_out = MAGR_OP_FORTRAN_GEMM; return MAGR_STATUS_OK; }
    if (strstr(buf, "+") != NULL) { *kind_out = MAGR_OP_FORTRAN_AXPY; return MAGR_STATUS_OK; }
    return MAGR_STATUS_UNSUPPORTED;
}

const char *magr_uiua_unsupported_reason(const char *glyphs) {
    char buf[128];
    normalise(glyphs, buf, sizeof(buf));

    if (buf[0] == '\0') return "empty Uiua glyph sequence";
    if (strstr(buf, "\xE2\xA6\x9E") != NULL)
        return "`\xE2\xA6\x9E` (table/reshape) is not lowered; v1 reshape operates on "
               "contiguous views only.";
    if (strstr(buf, "\xE2\xA6\x8F") != NULL)
        return "`\xE2\xA6\x8F` (slice) is declared in the IR but not implemented.";
    if (strstr(buf, "\xE2\x8D\x9C") != NULL)
        return "`\xE2\x8D\x9C` (modifier) takes and returns functions; the v1 IR has no "
               "higher-order node, so modifiers must be inlined by the caller.";
    if (strstr(buf, "\xC3\xB7") != NULL)
        return "elementwise divide is implemented but its div-by-zero semantics are a "
               "language decision that should be made explicitly, so it is not lowered by default.";
    return "this glyph sequence is outside MAGR's documented Uiua subset. Supported: "
           "/+x (dot), +x (axpy), norm, matmul, gemv, /+ (sum), and negation.";
}
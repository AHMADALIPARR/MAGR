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
 * magr_klong.c -- lowering for the documented Klong subset.
 *
 * STATUS: source complete, not compiled, not executed. No C toolchain was
 * available when this was written. See STATUS.md.
 */

#include "magr_klong.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * IR construction is spelled out rather than wrapped in an over-clever builder.
 * The IR struct is a flat array with explicit indices, and every builder has to
 * know exactly which index it is about to fill. A builder that hides that
 * behind an append() call is a builder that makes the value/node distinction
 * invisible -- which is precisely the confusion that made an earlier revision's
 * tests pass while conflating node ids with value ids.
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

/* Binds an input value: a value with a buffer and no producer. */
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

/* Allocates the output value for a node and returns its index. */
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

    /* Dense column-major strides. A 2-D result is given the layout the Fortran
     * layer needs, because every GEMM-shaped node writes into a fresh buffer
     * and there is no reason to make the executor re-derive it. */
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
    /* Zero-initialised: an unwritten element must be a defined value, not
     * whatever was in the heap. */
    memset(ir->values[idx].data, 0, (size_t)ir->values[idx].capacity_bytes);
    ir->values[idx].ownership = (uint32_t)MAGR_OWNERSHIP_OWNED;
    ir->values[idx].flags = 0;
    ir->values[idx].producer = (uint32_t)node;
    ir->nodes[node].result = (int32_t)idx;
    return idx;
}

static magr_status require_f64_vector(const magr_ir_value *v) {
    if (v == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((magr_dtype)v->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    if (v->rank != 1) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (v->data == NULL) return MAGR_STATUS_BOUNDS;
    return MAGR_STATUS_OK;
}

static magr_status require_f64_matrix(const magr_ir_value *v) {
    size_t esize;
    if (v == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((magr_dtype)v->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    if (v->rank != 2) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (v->data == NULL) return MAGR_STATUS_BOUNDS;
    esize = magr_dtype_size(MAGR_DTYPE_F64);
    /* Refuse row-major rather than inserting a silent transposing copy: the
     * copy would change what the caller pays for and would hide a mis-lowering
     * of the source program. */
    if (v->strides[0] != (int64_t)esize || v->strides[1] % (int64_t)esize != 0 ||
        v->strides[1] / (int64_t)esize < v->dims[0])
        return MAGR_STATUS_STRIDE_UNSUPPORTED;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

magr_status magr_klong_lower_dot(const magr_ir_value *x, const magr_ir_value *y, magr_ir *ir) {
    magr_status st;
    uint32_t vx, vy, vr;
    int32_t node;
    int64_t scalar_dim[1];

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_f64_vector(x);
    if (st != MAGR_STATUS_OK) return st;
    st = require_f64_vector(y);
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

    /* DDOT returns a scalar. Representing it as a rank-0 value keeps one
     * uniform representation for "a number that came out of a reduction". */
    scalar_dim[0] = 1;
    vr = alloc_result(ir, node, MAGR_DTYPE_F64, 0, scalar_dim);
    if (vr == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_klong_lower_axpy(const magr_ir_value *x, const magr_ir_value *y, double alpha,
                                  magr_ir *ir) {
    magr_status st;
    uint32_t vx, vy;
    int32_t node;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_f64_vector(x);
    if (st != MAGR_STATUS_OK) return st;
    st = require_f64_vector(y);
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

    /* AXPY writes into y, so the result IS y. Aliasing it is declared, not
     * assumed: the scheduler is entitled to serialise around it. */
    ir->nodes[node].result = (int32_t)vy;
    ir->values[vy].flags |= MAGR_ARRAY_F_MAY_ALIAS;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_klong_lower_nrm2(const magr_ir_value *x, magr_ir *ir) {
    magr_status st;
    uint32_t vx, vr;
    int32_t node;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_f64_vector(x);
    if (st != MAGR_STATUS_OK) return st;

    st = magr_ir_init(ir);
    if (st != MAGR_STATUS_OK) return st;

    vx = bind_input(ir, x);
    if (vx == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    node = add_node(ir, MAGR_OP_FORTRAN_NRM2);
    if (node < 0) return MAGR_STATUS_INTERNAL;
    ir->nodes[node].operands[0] = vx;
    ir->nodes[node].operand_count = 1;

    vr = alloc_result(ir, node, MAGR_DTYPE_F64, 0, NULL);
    if (vr == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

magr_status magr_klong_lower_matmul(const magr_ir_value *a, const magr_ir_value *b, magr_ir *ir) {
    magr_status st;
    uint32_t va, vb, vr;
    int32_t node;
    int64_t dims[2];

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = require_f64_matrix(a);
    if (st != MAGR_STATUS_OK) return st;
    st = require_f64_matrix(b);
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
    ir->nodes[node].flags[0] = 0; /* TRANSA = N */
    ir->nodes[node].flags[1] = 0; /* TRANSB = N */
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
    vr = alloc_result(ir, node, MAGR_DTYPE_F64, 2, dims);
    if (vr == UINT32_MAX) return MAGR_STATUS_INTERNAL;

    ir->output_node = (uint32_t)node;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Classification and diagnostics                                      */
/* ------------------------------------------------------------------ */

static void strip_ws(const char *in, char *out, size_t cap) {
    size_t j = 0;
    if (in == NULL) { if (cap > 0) out[0] = '\0'; return; }
    while (*in != '\0' && j + 1 < cap) {
        if (!isspace((unsigned char)*in)) out[j++] = *in;
        in++;
    }
    out[j] = '\0';
}

magr_status magr_klong_classify(const char *text, magr_op_kind *kind_out) {
    char buf[128];

    if (text == NULL || kind_out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    strip_ws(text, buf, sizeof(buf));

    if (strcmp(buf, "+/x*y") == 0 || strcmp(buf, "+/y*x") == 0) {
        *kind_out = MAGR_OP_FORTRAN_DOT;
        return MAGR_STATUS_OK;
    }
    if (strcmp(buf, "a*x+y") == 0 || strcmp(buf, "x*a+y") == 0) {
        *kind_out = MAGR_OP_FORTRAN_AXPY;
        return MAGR_STATUS_OK;
    }
    if (strcmp(buf, ".nrm2x") == 0 || strcmp(buf, "x.nrm2") == 0) {
        *kind_out = MAGR_OP_FORTRAN_NRM2;
        return MAGR_STATUS_OK;
    }
    if (strcmp(buf, "x@y") == 0) { *kind_out = MAGR_OP_FORTRAN_GEMM; return MAGR_STATUS_OK; }
    if (strcmp(buf, "+/x") == 0) { *kind_out = MAGR_OP_REDUCE_SUM; return MAGR_STATUS_OK; }
    if (strcmp(buf, "\\x") == 0) { *kind_out = MAGR_OP_ELEMENTWISE_NEG; return MAGR_STATUS_OK; }
    return MAGR_STATUS_UNSUPPORTED;
}

const char *magr_klong_unsupported_reason(const char *text) {
    char buf[128];
    strip_ws(text, buf, sizeof(buf));

    if (buf[0] == '\0') return "empty Klong fragment";
    if (strstr(buf, "x@y") != NULL)
        return "`x@y` needs column-major f64 operands; a row-major operand is refused "
               "rather than silently copied. Lower it as an explicit transpose instead.";
    if (strstr(buf, "x y") != NULL)
        return "`x y` is concatenation, which has no BLAS analogue and is not implemented. "
               "MAGR will not pretend it is a dot product.";
    if (strstr(buf, "#") != NULL)
        return "`#x` (length) is metadata that the v1 IR has no node for.";
    if (strstr(buf, ",") != NULL)
        return "overlappy (`,`) control flow has no IR node; unroll the expression.";
    if (strstr(buf, "[") != NULL)
        return "Klong slicing is not lowered yet; view.slice is declared but unimplemented.";
    return "this Klong form is outside MAGR's documented subset. Supported forms are "
           "+/x*y, a*x+y, .nrm2 x, x@y (column-major), +/x, and \\x.";
}
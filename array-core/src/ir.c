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

/* magr/ir.c -- Array IR op metadata, structural validation, and shape inference. */

#include "magr/ir.h"

#include "magr/array_core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Op metadata                                                         */
/* ------------------------------------------------------------------ */

typedef struct op_info {
    magr_op_kind kind;
    const char *name;
    uint32_t arity; /* array operands */
    magr_functor_id functor;
    int pure;
    int fusable;
} op_info;

static const op_info kOps[] = {
    {MAGR_OP_UNSUPPORTED, "unsupported", 0, MAGR_FUNCTOR_INVALID, 0, 0},

    {MAGR_OP_FORTRAN_DOT, "fortran.dot", 2, MAGR_FUNCTOR_F_DOT_F64, 1, 0},
    {MAGR_OP_FORTRAN_AXPY, "fortran.axpy", 2, MAGR_FUNCTOR_F_AXPY_F64, 1, 0},
    {MAGR_OP_FORTRAN_NRM2, "fortran.nrm2", 1, MAGR_FUNCTOR_F_NRM2_F64, 1, 0},
    {MAGR_OP_FORTRAN_GEMV, "fortran.gemv", 3, MAGR_FUNCTOR_F_GEMV_F64, 1, 0},
    {MAGR_OP_FORTRAN_GEMM, "fortran.gemm", 3, MAGR_FUNCTOR_F_GEMM_F64, 1, 0},
    {MAGR_OP_FORTRAN_GER, "fortran.ger", 3, MAGR_FUNCTOR_F_GER_F64, 1, 0},
    {MAGR_OP_FORTRAN_TRSV, "fortran.trsv", 2, MAGR_FUNCTOR_F_TRSV_F64, 1, 0},
    {MAGR_OP_FORTRAN_TRSM, "fortran.trsm", 2, MAGR_FUNCTOR_F_TRSM_F64, 1, 0},

    {MAGR_OP_VIEW_RESHAPE, "view.reshape", 1, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_VIEW_SLICE, "view.slice", 1, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_VIEW_TRANSPOSE, "view.transpose", 1, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_VIEW_BROADCAST, "view.broadcast", 2, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_VIEW_FLATTEN, "view.flatten", 1, MAGR_FUNCTOR_INVALID, 1, 1},

    {MAGR_OP_ELEMENTWISE_ADD, "elementwise.add", 2, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_ELEMENTWISE_SUB, "elementwise.sub", 2, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_ELEMENTWISE_MUL, "elementwise.mul", 2, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_ELEMENTWISE_DIV, "elementwise.div", 2, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_ELEMENTWISE_NEG, "elementwise.neg", 1, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_ELEMENTWISE_FMA, "elementwise.fma", 3, MAGR_FUNCTOR_INVALID, 1, 1},

    {MAGR_OP_REDUCE_SUM, "reduce.sum", 1, MAGR_FUNCTOR_INVALID, 1, 0},
    {MAGR_OP_REDUCE_MAX, "reduce.max", 1, MAGR_FUNCTOR_INVALID, 1, 0},
    {MAGR_OP_REDUCE_MIN, "reduce.min", 1, MAGR_FUNCTOR_INVALID, 1, 0},

    {MAGR_OP_CONST, "const", 0, MAGR_FUNCTOR_INVALID, 1, 1},
    {MAGR_OP_INPUT, "input", 0, MAGR_FUNCTOR_INVALID, 1, 0},
    {MAGR_OP_OUTPUT, "output", 1, MAGR_FUNCTOR_INVALID, 1, 0},
};

static const op_info *op_find(magr_op_kind kind) {
    size_t i;
    for (i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (kOps[i].kind == kind) return &kOps[i];
    }
    return NULL;
}

const char *magr_op_kind_name(magr_op_kind kind) {
    const op_info *o = op_find(kind);
    return o != NULL ? o->name : "unsupported";
}

magr_op_kind magr_op_kind_from_name(const char *name) {
    size_t i;
    if (name == NULL) return MAGR_OP_UNSUPPORTED;
    for (i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (strcmp(kOps[i].name, name) == 0) return kOps[i].kind;
    }
    return MAGR_OP_UNSUPPORTED;
}

uint32_t magr_op_kind_arity(magr_op_kind kind) {
    const op_info *o = op_find(kind);
    return o != NULL ? o->arity : 0;
}

magr_functor_id magr_op_functor(magr_op_kind kind) {
    const op_info *o = op_find(kind);
    return o != NULL ? o->functor : MAGR_FUNCTOR_INVALID;
}

int magr_op_is_pure(magr_op_kind kind) {
    const op_info *o = op_find(kind);
    return o != NULL ? o->pure : 0;
}

int magr_op_is_fusable(magr_op_kind kind) {
    const op_info *o = op_find(kind);
    return o != NULL ? o->fusable : 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

magr_status magr_ir_init(magr_ir *ir) {
    uint32_t i;
    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    memset(ir, 0, sizeof(*ir));
    ir->struct_size = (uint32_t)sizeof(*ir);
    ir->abi_version = (uint32_t)MAGR_ABI_VERSION;
    ir->output_node = 0;
    for (i = 0; i < MAGR_IR_MAX_OPS; ++i) {
        ir->nodes[i].result = -1;
    }
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Structural validation                                               */
/* ------------------------------------------------------------------ */

magr_status magr_ir_validate_program(const magr_ir *ir) {
    uint32_t i, j;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (ir->struct_size < offsetof(magr_ir, output_node) + sizeof(ir->output_node))
        return MAGR_STATUS_ABI_MISMATCH;
    if (((ir->abi_version >> 16) & 0xFFFFu) != ((uint32_t)MAGR_ABI_VERSION >> 16))
        return MAGR_STATUS_ABI_MISMATCH;
    if (ir->node_count == 0 || ir->node_count > MAGR_IR_MAX_OPS)
        return MAGR_STATUS_INVALID_ARGUMENT;
    if (ir->output_node >= ir->node_count) return MAGR_STATUS_INVALID_ARGUMENT;

    for (i = 0; i < ir->node_count; ++i) {
        const magr_ir_node *n = &ir->nodes[i];
        uint32_t arity;

        if (op_find(n->kind) == NULL || n->kind == MAGR_OP_UNSUPPORTED)
            return MAGR_STATUS_UNSUPPORTED;

        arity = magr_op_kind_arity(n->kind);
        if (n->operand_count != arity) return MAGR_STATUS_INVALID_ARGUMENT;
        if (n->operand_count > MAGR_IR_MAX_OPERANDS) return MAGR_STATUS_INVALID_ARGUMENT;
        if (n->scalar_count > MAGR_IR_MAX_SCALARS) return MAGR_STATUS_INVALID_ARGUMENT;

        for (j = 0; j < n->operand_count; ++j) {
            uint32_t idx = n->operands[j];
            if (idx >= ir->value_count) return MAGR_STATUS_BOUNDS;
            /* Topological invariant: a value produced by node p must come from
             * a node with p < i. Inputs have producer == -1 and are exempt.
             * This makes cycle detection a range check rather than a traversal,
             * and the JSON loader is responsible for maintaining the order. */
            {
                uint32_t producer = ir->values[idx].producer;
                if (producer != UINT32_MAX && producer >= i) return MAGR_STATUS_INVALID_ARGUMENT;
            }
        }
        for (j = 0; j < n->scalar_count; ++j) {
            if (!n->scalars[j].is_literal) {
                int32_t p = n->scalars[j].producer_node;
                if (p < 0 || (uint32_t)p >= i) return MAGR_STATUS_INVALID_ARGUMENT;
            }
        }
        if (n->result < 0 || (uint32_t)n->result >= ir->value_count) return MAGR_STATUS_BOUNDS;
    }
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Shape inference                                                     */
/* ------------------------------------------------------------------ */

static void set_result(magr_ir *ir, uint32_t node, magr_dtype dtype, const int64_t *dims,
                       uint32_t rank) {
    magr_ir_value *v = &ir->values[ir->nodes[node].result];
    uint32_t i;
    v->dtype = (uint32_t)dtype;
    v->rank = rank;
    for (i = 0; i < rank; ++i) v->dims[i] = dims[i];
    for (i = 0; i < rank; ++i) v->strides[i] = 0;
    v->producer = node;
    ir->nodes[node].dtype = (uint32_t)dtype;
    ir->nodes[node].rank = rank;
    for (i = 0; i < rank; ++i) ir->nodes[node].dims[i] = dims[i];
    ir->nodes[node].state = MAGR_NODE_VALIDATED;
}

static magr_dtype value_dtype(const magr_ir_value *v) { return (magr_dtype)v->dtype; }

/* Broadcasting rules, restricted to what v1 supports: ranks must match exactly
 * and each dimension must be equal or 1. Rank-different broadcasting (the
 * numpy-style leading-alignment rule) is REFUSED. It is a real and useful
 * feature, but Uiua and Klong disagree about its edge cases, and a front end
 * that wants it must insert explicit reshape nodes. Guessing here would mean
 * silently changing the meaning of a program. */
static int broadcast_shape(const int64_t *a, uint32_t ra, const int64_t *b, uint32_t rb,
                           int64_t *out, uint32_t *rout) {
    uint32_t i;
    if (ra != rb) return 0;
    for (i = 0; i < ra; ++i) {
        if (a[i] == b[i]) out[i] = a[i];
        else if (a[i] == 1) out[i] = b[i];
        else if (b[i] == 1) out[i] = a[i];
        else return 0;
    }
    *rout = ra;
    return 1;
}

magr_status magr_ir_infer_shapes_explained(magr_ir *ir, char *err, size_t err_len) {
    uint32_t i, j;
    magr_status st;

#define MAGR_IR_FAIL(code, ...)                                                      \
    do {                                                                             \
        if (err != NULL && err_len > 0) {                                            \
            snprintf(err, err_len, __VA_ARGS__);                                     \
        }                                                                            \
        return (code);                                                               \
    } while (0)
#define MAGR_IR_TRY(expr, node)                                                    \
    do {                                                                           \
        st = (expr);                                                               \
        if (st != MAGR_STATUS_OK)                                                 \
            MAGR_IR_FAIL(st, "node %u (%s): %s", (node),                           \
                         magr_op_kind_name(ir->nodes[node].kind),                  \
                         magr_status_string(st));                                  \
    } while (0)

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    MAGR_IR_TRY(magr_ir_validate_program(ir), 0);

    for (i = 0; i < ir->node_count; ++i) {
        magr_ir_node *n = &ir->nodes[i];
        const magr_ir_value *a0 = n->operand_count > 0 ? &ir->values[n->operands[0]] : NULL;
        const magr_ir_value *a1 = n->operand_count > 1 ? &ir->values[n->operands[1]] : NULL;
        const magr_ir_value *a2 = n->operand_count > 2 ? &ir->values[n->operands[2]] : NULL;
        int64_t d[MAGR_IR_MAX_DIMS];

        switch (n->kind) {
        case MAGR_OP_INPUT:
        case MAGR_OP_CONST:
            /* Already fully specified by the loader. */
            break;

        case MAGR_OP_OUTPUT:
            /* The output node only re-labels a shape; there is nothing to
             * infer, and the loader has already checked that the operand
             * exists. */
            set_result(ir, i, value_dtype(a0), a0->dims, a0->rank);
            break;

        case MAGR_OP_FORTRAN_DOT: {
            if (a1->dims[0] != a0->dims[0])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: dot length mismatch %lld vs %lld", i,
                             (long long)a0->dims[0], (long long)a1->dims[0]);
            if (value_dtype(a0) != MAGR_DTYPE_F64 || value_dtype(a1) != MAGR_DTYPE_F64)
                MAGR_IR_FAIL(MAGR_STATUS_UNSUPPORTED,
                             "node %u: dot requires f64", i);
            d[0] = 1;
            set_result(ir, i, MAGR_DTYPE_F64, d, 0); /* scalar-shaped */
            break;
        }

        case MAGR_OP_FORTRAN_NRM2: {
            if (a0->rank != 1)
                MAGR_IR_FAIL(MAGR_STATUS_RANK_UNSUPPORTED,
                             "node %u: nrm2 expects a vector, got rank %u", i, a0->rank);
            if (value_dtype(a0) != MAGR_DTYPE_F64)
                MAGR_IR_FAIL(MAGR_STATUS_UNSUPPORTED,
                             "node %u: nrm2 requires f64", i);
            set_result(ir, i, MAGR_DTYPE_F64, d, 0);
            break;
        }

        case MAGR_OP_FORTRAN_AXPY: {
            if (a0->dims[0] != a1->dims[0])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: axpy length mismatch", i);
            set_result(ir, i, value_dtype(a1), a1->dims, a1->rank);
            break;
        }

        case MAGR_OP_FORTRAN_GEMV: {
            /* Logical M,N are derived from the matrix and TRANS exactly as the
             * Fortran routine's own documentation does. */
            int trans = (int)n->flags[0];
            int64_t m = (trans == 0) ? a0->dims[0] : a0->dims[1];
            int64_t nn = (trans == 0) ? a0->dims[1] : a0->dims[0];
            int64_t xn = (trans == 0) ? m : nn;
            int64_t yn = (trans == 0) ? nn : m;
            if (a1->rank != 1 || a2->rank != 1)
                MAGR_IR_FAIL(MAGR_STATUS_RANK_UNSUPPORTED,
                             "node %u: gemv operands must be vectors", i);
            if (a1->dims[0] != xn)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: gemv x has length %lld, expected %lld", i,
                             (long long)a1->dims[0], (long long)xn);
            if (a2->dims[0] != yn)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: gemv y has length %lld, expected %lld", i,
                             (long long)a2->dims[0], (long long)yn);
            d[0] = yn;
            set_result(ir, i, MAGR_DTYPE_F64, d, 1);
            break;
        }

        case MAGR_OP_FORTRAN_GEMM: {
            int ta = (int)n->flags[0], tb = (int)n->flags[1];
            int64_t am = (ta == 0) ? a0->dims[0] : a0->dims[1];
            int64_t ak = (ta == 0) ? a0->dims[1] : a0->dims[0];
            int64_t bk = (tb == 0) ? a1->dims[0] : a1->dims[1];
            int64_t bn = (tb == 0) ? a1->dims[1] : a1->dims[0];
            if (ak != bk)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: gemm inner dims disagree: %lld vs %lld", i,
                             (long long)ak, (long long)bk);
            if (a2->dims[0] != am || a2->dims[1] != bn)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: gemm C is (%lld,%lld), expected (%lld,%lld)", i,
                             (long long)a2->dims[0], (long long)a2->dims[1],
                             (long long)am, (long long)bn);
            d[0] = am; d[1] = bn;
            set_result(ir, i, MAGR_DTYPE_F64, d, 2);
            break;
        }

        case MAGR_OP_FORTRAN_GER: {
            if (a0->dims[0] != a2->dims[0])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: ger x length != rows", i);
            if (a1->dims[0] != a2->dims[1])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: ger y length != cols", i);
            set_result(ir, i, value_dtype(a2), a2->dims, a2->rank);
            break;
        }

        case MAGR_OP_FORTRAN_TRSV: {
            if (a0->dims[0] != a0->dims[1])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: trsv A must be square, got (%lld,%lld)", i,
                             (long long)a0->dims[0], (long long)a0->dims[1]);
            if (a1->dims[0] != a0->dims[0])
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: trsv x length != n", i);
            set_result(ir, i, MAGR_DTYPE_F64, a1->dims, a1->rank);
            break;
        }

        case MAGR_OP_FORTRAN_TRSM: {
            int side = (int)n->flags[0];
            int64_t k = (side == 0) ? a2->dims[0] : a2->dims[1];
            if (a0->dims[0] != a0->dims[1] || a0->dims[0] != k)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: trsm A must be %lld x %lld", i, (long long)k,
                             (long long)k);
            set_result(ir, i, value_dtype(a1), a1->dims, a1->rank);
            break;
        }

        case MAGR_OP_VIEW_TRANSPOSE: {
            if (a0->rank != 2)
                MAGR_IR_FAIL(MAGR_STATUS_RANK_UNSUPPORTED,
                             "node %u: transpose expects rank 2, got %u", i, a0->rank);
            d[0] = a0->dims[1];
            d[1] = a0->dims[0];
            set_result(ir, i, value_dtype(a0), d, 2);
            break;
        }

        case MAGR_OP_VIEW_FLATTEN: {
            int64_t total = 1;
            uint32_t k;
            for (k = 0; k < a0->rank; ++k) total *= a0->dims[k];
            d[0] = total;
            set_result(ir, i, value_dtype(a0), d, 1);
            break;
        }

        case MAGR_OP_VIEW_RESHAPE: {
            uint32_t k = n->scalar_count;
            if (k != (uint32_t)a0->rank)
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: reshape needs one dim per input axis", i);
            for (j = 0; j < k; ++j) d[j] = (int64_t)n->scalars[j].literal;
            set_result(ir, i, value_dtype(a0), d, k);
            break;
        }

        case MAGR_OP_VIEW_SLICE:
        case MAGR_OP_VIEW_BROADCAST: {
            uint32_t rout = 0;
            if (!broadcast_shape(a0->dims, a0->rank, a1->dims, a1->rank, d, &rout))
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: shapes (%lld..) and (%lld..) do not broadcast "
                             "under MAGR v1's equal-rank rule",
                             i, (long long)a0->dims[0], (long long)a1->dims[0]);
            set_result(ir, i, value_dtype(a0), d, rout);
            break;
        }

        case MAGR_OP_ELEMENTWISE_NEG:
            set_result(ir, i, value_dtype(a0), a0->dims, a0->rank);
            break;

        case MAGR_OP_ELEMENTWISE_ADD:
        case MAGR_OP_ELEMENTWISE_SUB:
        case MAGR_OP_ELEMENTWISE_MUL:
        case MAGR_OP_ELEMENTWISE_DIV:
        case MAGR_OP_ELEMENTWISE_FMA: {
            uint32_t rout = 0;
            const magr_ir_value *last = a2 != NULL ? a2 : a1;
            if (!broadcast_shape(a0->dims, a0->rank, a1->dims, a1->rank, d, &rout))
                MAGR_IR_FAIL(MAGR_STATUS_SHAPE_MISMATCH,
                             "node %u: elementwise shapes do not broadcast", i);
            set_result(ir, i, value_dtype(last), d, rout);
            break;
        }

        case MAGR_OP_REDUCE_SUM:
        case MAGR_OP_REDUCE_MAX:
        case MAGR_OP_REDUCE_MIN:
            /* Full reduction to a scalar. Partial reduction over a subset of
             * axes is not in v1; a front end must express it as a slice plus a
             * transpose plus a full reduce, or wait for v2. */
            set_result(ir, i, value_dtype(a0), d, 0);
            break;

        default:
            MAGR_IR_FAIL(MAGR_STATUS_UNSUPPORTED,
                         "node %u: no shape rule for %s", i, magr_op_kind_name(n->kind));
        }
    }
    return MAGR_STATUS_OK;
#undef MAGR_IR_FAIL
#undef MAGR_IR_TRY
}

magr_status magr_ir_infer_shapes(magr_ir *ir) {
    return magr_ir_infer_shapes_explained(ir, NULL, 0);
}
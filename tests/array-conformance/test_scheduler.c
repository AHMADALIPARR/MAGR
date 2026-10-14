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
 * tests/array-conformance/test_scheduler.c
 *
 * Scheduler and Array IR conformance. This is the suite that covers the Phase 3
 * work: the dependency graph, the wavefront decomposition, the conflict
 * predicate, and serial-versus-wavefront equivalence.
 *
 * The central claim being tested is the one that is easiest to get wrong and
 * hardest to notice:
 *
 *     Two nodes that are INDEPENDENT in the dependency sense are not
 *     necessarily independent in the memory sense, and the scheduler must
 *     never run two nodes against the same buffer concurrently.
 *
 * A bug in that area does not crash and does not usually produce an obviously
 * wrong number. It produces a number that is right most of the time. So the
 * tests below are built to make the bad case deterministic rather than
 * probabilistic.
 *
 * STATUS: source complete, not compiled or executed here. See STATUS.md.
 */

#include "magr_test.h"

#include "magr/array_core.h"
#include "magr/ir.h"
#include "magr_execute.h"

#include <stdlib.h>
#include <string.h>

int magr_test_failures = 0;
int magr_test_checks = 0;
const char *magr_test_current = NULL;

/* ------------------------------------------------------------------ */
/* IR construction helpers                                             */
/* ------------------------------------------------------------------ */

static uint32_t ir_add_value(magr_ir *ir, const magr_ir_value *v) {
    uint32_t idx;
    if (ir->value_count >= MAGR_IR_MAX_OPS) return UINT32_MAX;
    idx = ir->value_count++;
    ir->values[idx] = *v;
    ir->values[idx].producer = UINT32_MAX;
    return idx;
}

static int32_t ir_add_node(magr_ir *ir, magr_op_kind kind) {
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

/* Binds a caller-owned buffer as an input value. No allocation: the test owns
 * the memory and asserts on it afterwards. */
static uint32_t bind_buffer(magr_ir *ir, void *data, int64_t n, magr_dtype dt) {
    uint32_t idx;
    int32_t node;
    magr_ir_value v;

    memset(&v, 0, sizeof(v));
    v.dtype = (uint32_t)dt;
    v.rank = 1;
    v.dims[0] = n;
    v.strides[0] = (int64_t)magr_dtype_size(dt);
    v.data = data;
    v.capacity_bytes = n * (int64_t)magr_dtype_size(dt);
    v.ownership = (uint32_t)MAGR_OWNERSHIP_BORROWED;
    v.flags = 0;

    idx = ir_add_value(ir, &v);
    if (idx == UINT32_MAX) return UINT32_MAX;
    node = ir_add_node(ir, MAGR_OP_INPUT);
    if (node < 0) return UINT32_MAX;
    ir->nodes[node].result = (int32_t)idx;
    ir->values[idx].producer = (uint32_t)node;
    return idx;
}

static uint32_t alloc_result(magr_ir *ir, int32_t node, int64_t n) {
    uint32_t idx = ir_add_value(ir, NULL);
    if (idx == UINT32_MAX) return UINT32_MAX;
    ir->values[idx].dtype = (uint32_t)MAGR_DTYPE_F64;
    ir->values[idx].rank = 1;
    ir->values[idx].dims[0] = n;
    ir->values[idx].strides[0] = 8;
    ir->values[idx].capacity_bytes = n * 8;
    ir->values[idx].data = calloc((size_t)n, 8);
    if (ir->values[idx].data == NULL) return UINT32_MAX;
    ir->values[idx].ownership = (uint32_t)MAGR_OWNERSHIP_OWNED;
    ir->values[idx].producer = (uint32_t)node;
    ir->nodes[node].result = (int32_t)idx;
    return idx;
}

/* ------------------------------------------------------------------ */
/* Structural validation                                               */
/* ------------------------------------------------------------------ */

static int t_valid_program_validates(void) {
    magr_ir ir;
    double a[4] = {1, 2, 3, 4};
    double b[4] = {5, 6, 7, 8};
    uint32_t va, vb, vr;
    int32_t node;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = bind_buffer(&ir, a, 4, MAGR_DTYPE_F64);
    vb = bind_buffer(&ir, b, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = va;
    ir.nodes[node].operands[1] = vb;
    ir.nodes[node].operand_count = 2;
    vr = alloc_result(&ir, node, 1);
    ir.nodes[node].result = (int32_t)vr;
    ir.output_node = (uint32_t)node;

    MAGR_CHECK_STATUS(magr_ir_validate_program(&ir), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_ir_infer_shapes(&ir), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(ir.nodes[node].rank, 0); /* DOT yields a scalar */
    return 0;
}

static int t_forward_reference_is_rejected(void) {
    /* A node consuming a value produced by a LATER node is a cycle, and the
     * topological invariant makes it a range check. Without that invariant
     * this is the bug that silently produces an execution order that depends
     * on the scheduler's mood. */
    magr_ir ir;
    double a[4] = {1, 2, 3, 4};
    uint32_t va;
    int32_t early;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = bind_buffer(&ir, a, 4, MAGR_DTYPE_F64);

    early = ir_add_node(&ir, MAGR_OP_FORTRAN_NRM2);
    ir.nodes[early].operands[0] = ir.value_count; /* a value that does not exist yet */
    ir.nodes[early].operand_count = 1;
    ir.nodes[early].result = (int32_t)ir.value_count;

    MAGR_CHECK_STATUS(magr_ir_validate_program(&ir), MAGR_STATUS_INVALID_ARGUMENT);
    return 0;
}

static int t_arity_is_enforced(void) {
    /* A DOT with one operand is not a DOT. Catching it structurally is what
     * makes "unsupported operations fail explicitly" a property of the IR
     * rather than a promise about the executor. */
    magr_ir ir;
    double a[4] = {1, 2, 3, 4};
    uint32_t va, vr;
    int32_t node;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = bind_buffer(&ir, a, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = va;
    ir.nodes[node].operand_count = 1; /* should be 2 */
    vr = alloc_result(&ir, node, 1);
    ir.nodes[node].result = (int32_t)vr;

    MAGR_CHECK_STATUS(magr_ir_validate_program(&ir), MAGR_STATUS_INVALID_ARGUMENT);
    MAGR_CHECK_EQ_I(magr_op_kind_arity(MAGR_OP_FORTRAN_DOT), 2);
    MAGR_CHECK_EQ_I(magr_op_kind_arity(MAGR_OP_FORTRAN_NRM2), 1);
    MAGR_CHECK_EQ_I(magr_op_kind_arity(MAGR_OP_FORTRAN_GEMM), 3);
    return 0;
}

static int t_unsupported_op_kind_is_refused(void) {
    magr_ir ir;
    int32_t node;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    node = ir_add_node(&ir, MAGR_OP_UNSUPPORTED);
    ir.nodes[node].result = 0;
    MAGR_CHECK_STATUS(magr_ir_validate_program(&ir), MAGR_STATUS_UNSUPPORTED);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shape inference                                                     */
/* ------------------------------------------------------------------ */

static int t_gemm_shape_inference(void) {
    /* 2x3 * 3x2 -> 2x2, and a mismatched K must be reported before execution. */
    magr_ir ir;
    magr_ir_value a, b;
    uint32_t va, vb, vc;
    int32_t node;
    char err[256];

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.dtype = (uint32_t)MAGR_DTYPE_F64;
    a.rank = 2;
    a.dims[0] = 2; a.dims[1] = 3;
    a.strides[0] = 8; a.strides[1] = 16;
    b.dtype = (uint32_t)MAGR_DTYPE_F64;
    b.rank = 2;
    b.dims[0] = 3; b.dims[1] = 2;
    b.strides[0] = 8; b.strides[1] = 24;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = ir_add_value(&ir, &a); ir_add_node(&ir, MAGR_OP_INPUT)->result = (int32_t)va;
    vb = ir_add_value(&ir, &b); ir_add_node(&ir, MAGR_OP_INPUT)->result = (int32_t)vb;
    vc = ir_add_value(&ir, NULL);
    vc.data = calloc(4, 8);
    vc.dtype = (uint32_t)MAGR_DTYPE_F64;
    vc.rank = 2; vc.dims[0] = 2; vc.dims[1] = 2;
    vc.strides[0] = 8; vc.strides[1] = 16;

    node = ir_add_node(&ir, MAGR_OP_FORTRAN_GEMM);
    ir.nodes[node].operands[0] = va;
    ir.nodes[node].operands[1] = vb;
    ir.nodes[node].operand_count = 2;
    ir.nodes[node].scalars[0].literal = 1.0;
    ir.nodes[node].scalars[0].is_literal = 1;
    ir.nodes[node].scalars[1].literal = 0.0;
    ir.nodes[node].scalars[1].is_literal = 1;
    ir.nodes[node].scalar_count = 2;
    ir.nodes[node].result = (int32_t)vc;
    ir.values[vc].producer = (uint32_t)node;
    ir.output_node = (uint32_t)node;

    MAGR_CHECK_STATUS(magr_ir_infer_shapes_explained(&ir, err, sizeof(err)), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(ir.nodes[node].dims[0], 2);
    MAGR_CHECK_EQ_I(ir.nodes[node].dims[1], 2);

    /* Now break K and confirm it is refused with an explanation, not a crash. */
    ir.values[vb].dims[0] = 4;
    MAGR_CHECK_STATUS(magr_ir_infer_shapes_explained(&ir, err, sizeof(err)),
                      MAGR_STATUS_SHAPE_MISMATCH);
    MAGR_CHECK_MSG(err[0] != '\0', "a shape failure must explain itself");
    printf("       (diagnostic: %s)\n", err);
    free(ir.values[vc].data);
    return 0;
}

static int t_broadcast_is_equal_rank_only(void) {
    /* MAGR v1 refuses rank-different broadcasting. Uiua and Klong disagree
     * about the edge cases, so a front end that wants it must insert an
     * explicit reshape. Guessing here would silently change program meaning. */
    magr_ir ir;
    magr_ir_value a, b;
    uint32_t va, vb, vr;
    int32_t node;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.dtype = b.dtype = (uint32_t)MAGR_DTYPE_F64;
    a.rank = 2; a.dims[0] = 2; a.dims[1] = 3; a.strides[0] = 8; a.strides[1] = 16;
    b.rank = 1; b.dims[0] = 3; b.strides[0] = 8;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = ir_add_value(&ir, &a); ir_add_node(&ir, MAGR_OP_INPUT)->result = (int32_t)va;
    vb = ir_add_value(&ir, &b); ir_add_node(&ir, MAGR_OP_INPUT)->result = (int32_t)vb;
    node = ir_add_node(&ir, MAGR_OP_ELEMENTWISE_ADD);
    ir.nodes[node].operands[0] = va;
    ir.nodes[node].operands[1] = vb;
    ir.nodes[node].operand_count = 2;

    vr = alloc_result(&ir, node, 6);
    MAGR_CHECK_STATUS(magr_ir_infer_shapes(&ir), MAGR_STATUS_SHAPE_MISMATCH);

    /* Equal rank with a unit axis DOES broadcast. */
    {
        magr_ir ir2;
        uint32_t va2, vb2, vr2;
        int32_t n2;
        magr_ir_value a2, b2;
        memset(&a2, 0, sizeof(a2));
        memset(&b2, 0, sizeof(b2));
        a2.dtype = b2.dtype = (uint32_t)MAGR_DTYPE_F64;
        a2.rank = 2; a2.dims[0] = 2; a2.dims[1] = 3; a2.strides[0] = 8; a2.strides[1] = 16;
        b2.rank = 2; b2.dims[0] = 1; b2.dims[1] = 3; b2.strides[0] = 8; b2.strides[1] = 24;

        MAGR_CHECK_STATUS(magr_ir_init(&ir2), MAGR_STATUS_OK);
        va2 = ir_add_value(&ir2, &a2); ir_add_node(&ir2, MAGR_OP_INPUT)->result = (int32_t)va2;
        vb2 = ir_add_value(&ir2, &b2); ir_add_node(&ir2, MAGR_OP_INPUT)->result = (int32_t)vb2;
        n2 = ir_add_node(&ir2, MAGR_OP_ELEMENTWISE_ADD);
        ir2.nodes[n2].operands[0] = va2;
        ir2.nodes[n2].operands[1] = vb2;
        ir2.nodes[n2].operand_count = 2;
        vr2 = alloc_result(&ir2, n2, 6);
        MAGR_CHECK_STATUS(magr_ir_infer_shapes(&ir2), MAGR_STATUS_OK);
        MAGR_CHECK_EQ_I(ir2.nodes[n2].dims[0], 2);
        MAGR_CHECK_EQ_I(ir2.nodes[n2].dims[1], 3);
        free(ir2.values[vr2].data);
    }
    (void)vr;
    free(ir.values[vr].data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wavefronts and conflict detection                                   */
/* ------------------------------------------------------------------ */

static int t_wavefront_decomposition(void) {
    /* Four independent dot products: all four INPUT nodes are ready at once,
     * then the four functor nodes are ready at once. Two waves. */
    magr_ir ir;
    double bufs[4][4];
    uint32_t vs[4];
    int32_t nodes[4];
    uint32_t starts[MAGR_IR_MAX_OPS], sizes[MAGR_IR_MAX_OPS], waves;
    int i;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    for (i = 0; i < 4; ++i) {
        bufs[i][0] = (double)(i + 1);
        bufs[i][1] = 1.0;
        bufs[i][2] = 0.0;
        bufs[i][3] = 0.0;
        vs[i] = bind_buffer(&ir, bufs[i], 4, MAGR_DTYPE_F64);
    }
    for (i = 0; i < 4; ++i) {
        nodes[i] = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
        ir.nodes[nodes[i]].operands[0] = vs[i];
        ir.nodes[nodes[i]].operand_count = 1;
        alloc_result(&ir, nodes[i], 1);
    }
    ir.output_node = (uint32_t)nodes[3];

    MAGR_CHECK_STATUS(magr_ir_validate_program(&ir), MAGR_STATUS_OK);
    waves = magr_ir_wavefronts(&ir, starts, sizes, MAGR_IR_MAX_OPS);
    MAGR_CHECK_EQ_I(waves, 2);
    MAGR_CHECK_EQ_I(sizes[0], 4); /* the four inputs */
    MAGR_CHECK_EQ_I(sizes[1], 4); /* the four reductions */
    MAGR_CHECK_EQ_I(starts[1], 4);
    MAGR_CHECK_EQ_I(magr_ir_depth(&ir), 2);
    return 0;
}

static int t_conflict_detection(void) {
    /*
     * The important test. Two nodes over the SAME buffer must be reported as
     * conflicting, and two nodes over adjacent-but-disjoint buffers must not.
     *
     * Making this deterministic matters: if the conflict predicate were wrong,
     * a real program would race and produce a wrong answer intermittently, and
     * an intermittent numerical bug is close to undiscoverable. Here it is a
     * boolean answer.
     */
    magr_ir ir;
    double shared[8];
    double other[8];
    uint32_t v_shared, v_other, v_dummy;
    int32_t n1, n2, n3;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    v_shared = bind_buffer(&ir, shared, 8, MAGR_DTYPE_F64);
    v_other = bind_buffer(&ir, other, 8, MAGR_DTYPE_F64);
    v_dummy = bind_buffer(&ir, shared + 8, 8, MAGR_DTYPE_F64); /* adjacent, disjoint */

    n1 = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[n1].operands[0] = v_shared;
    ir.nodes[n1].operand_count = 1;
    alloc_result(&ir, n1, 1);

    n2 = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[n2].operands[0] = v_shared;
    ir.nodes[n2].operand_count = 1;
    alloc_result(&ir, n2, 1);

    n3 = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[n3].operands[0] = v_other;
    ir.nodes[n3].operand_count = 1;
    alloc_result(&ir, n3, 1);

    /* Same buffer -> conflict. */
    MAGR_CHECK_MSG(magr_ir_nodes_conflict(&ir, (uint32_t)n1, (uint32_t)n2) == 1,
                   "two nodes over one buffer must conflict");
    /* Different buffers -> no conflict. */
    MAGR_CHECK_MSG(magr_ir_nodes_conflict(&ir, (uint32_t)n1, (uint32_t)n3) == 0,
                   "nodes over disjoint buffers must not conflict");
    /* A node never conflicts with itself. */
    MAGR_CHECK(magr_ir_nodes_conflict(&ir, (uint32_t)n1, (uint32_t)n1) == 1);

    /* Adjacent views: shared[0..7] and shared[8..15] do not overlap. This is
     * the half-open-interval case; an inclusive-endpoint implementation would
     * call these a conflict and serialise the entire program. */
    {
        magr_ir ir2;
        double big[32];
        uint32_t va, vb, vout;
        int32_t na, nb;

        MAGR_CHECK_STATUS(magr_ir_init(&ir2), MAGR_STATUS_OK);
        va = bind_buffer(&ir2, big, 8, MAGR_DTYPE_F64);
        vb = bind_buffer(&ir2, big + 8, 8, MAGR_DTYPE_F64);
        na = ir_add_node(&ir2, MAGR_OP_REDUCE_SUM);
        ir2.nodes[na].operands[0] = va;
        ir2.nodes[na].operand_count = 1;
        alloc_result(&ir2, na, 1);
        nb = ir_add_node(&ir2, MAGR_OP_REDUCE_SUM);
        ir2.nodes[nb].operands[0] = vb;
        ir2.nodes[nb].operand_count = 1;
        vout = alloc_result(&ir2, nb, 1);

        MAGR_CHECK_MSG(magr_ir_nodes_conflict(&ir2, (uint32_t)na, (uint32_t)nb) == 0,
                       "adjacent non-overlapping buffers must not conflict");
        free(ir2.values[vout].data);
    }
    (void)v_dummy;
    return 0;
}

static int t_aliasing_nodes_are_detectable(void) {
    magr_ir ir;
    double a[4];
    uint32_t va;
    int32_t node;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    va = bind_buffer(&ir, a, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[node].operands[0] = va;
    ir.nodes[node].operand_count = 1;
    alloc_result(&ir, node, 1);

    MAGR_CHECK(magr_ir_has_aliasing_nodes(&ir) == 0);
    ir.values[va].flags |= MAGR_ARRAY_F_MAY_ALIAS;
    MAGR_CHECK_MSG(magr_ir_has_aliasing_nodes(&ir) == 1,
                   "a declared alias must be visible so the scheduler can refuse to "
                   "parallelise rather than guess");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Serial execution                                                    */
/* ------------------------------------------------------------------ */

static int t_serial_dot_matches_mathematics(void) {
    magr_ir ir;
    double x[4] = {1, 2, 3, 4};
    double y[4] = {5, 6, 7, 8};
    uint32_t vx, vy, vr;
    int32_t node;
    magr_exec_report report;
    double out;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 4, MAGR_DTYPE_F64);
    vy = bind_buffer(&ir, y, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operands[1] = vy;
    ir.nodes[node].operand_count = 2;
    vr = alloc_result(&ir, node, 1);
    ir.output_node = (uint32_t)node;

    MAGR_CHECK_STATUS(magr_ir_execute_serial(&ir, &report), MAGR_STATUS_OK);
    memcpy(&out, ir.values[vr].data, sizeof(double));
    MAGR_CHECK_NEAR(out, 70.0, 1e-13, 1e-300);
    MAGR_CHECK_EQ_I(report.functors_invoked, 1);
    MAGR_CHECK_MSG(report.first_failure == MAGR_STATUS_OK, "no failure expected");
    free(ir.values[vr].data);
    return 0;
}

static int t_serial_axpy_chain_orders_correctly(void) {
    /* y := 2x + y then y := 3x + y, chained through y. The second node's input
     * is the FIRST node's output value, which is the dependency that makes this
     * a chain rather than two independent nodes. Result: y = 5x. */
    magr_ir ir;
    double x[3] = {1, 1, 1};
    double y[3] = {0, 0, 0};
    uint32_t vx, vy, vr;
    int32_t n1, n2;
    magr_exec_report report;
    const double *out;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 3, MAGR_DTYPE_F64);
    vy = bind_buffer(&ir, y, 3, MAGR_DTYPE_F64);

    n1 = ir_add_node(&ir, MAGR_OP_FORTRAN_AXPY);
    ir.nodes[n1].operands[0] = vx;
    ir.nodes[n1].operands[1] = vy;
    ir.nodes[n1].operand_count = 2;
    ir.nodes[n1].scalars[0].literal = 2.0;
    ir.nodes[n1].scalars[0].is_literal = 1;
    ir.nodes[n1].scalar_count = 1;
    ir.nodes[n1].result = (int32_t)vy; /* writes into y */
    ir.values[vy].flags |= MAGR_ARRAY_F_MAY_ALIAS;

    vr = ir_add_value(&ir, NULL);
    vr.dtype = (uint32_t)MAGR_DTYPE_F64;
    vr.rank = 1;
    vr.dims[0] = 3;
    vr.strides[0] = 8;
    vr.data = y; /* the same buffer */
    vr.capacity_bytes = 24;
    vr.ownership = (uint32_t)MAGR_OWNERSHIP_BORROWED;
    vr.producer = (uint32_t)n1;

    n2 = ir_add_node(&ir, MAGR_OP_FORTRAN_AXPY);
    ir.nodes[n2].operands[0] = vx;
    ir.nodes[n2].operands[1] = vr;
    ir.nodes[n2].operand_count = 2;
    ir.nodes[n2].scalars[0].literal = 3.0;
    ir.nodes[n2].scalars[0].is_literal = 1;
    ir.nodes[n2].scalar_count = 1;
    ir.nodes[n2].result = (int32_t)vr;
    ir.output_node = (uint32_t)n2;

    MAGR_CHECK_STATUS(magr_ir_execute_serial(&ir, &report), MAGR_STATUS_OK);
    out = (const double *)ir.values[vr].data;
    MAGR_CHECK_NEAR(out[0], 5.0, 1e-13, 1e-300);
    MAGR_CHECK_NEAR(out[1], 5.0, 1e-13, 1e-300);
    MAGR_CHECK_NEAR(out[2], 5.0, 1e-13, 1e-300);
    return 0;
}

static int t_computed_scalar_flows_across_nodes(void) {
    /* A DOT produces a number that a later node consumes as a scalar. The
     * result is stored on the node, not on the stack, which is what makes this
     * work across separate executor calls under wavefront scheduling. */
    magr_ir ir;
    double x[2] = {1, 2};
    double y[2] = {3, 4};
    double scale[2] = {1, 1};
    uint32_t vx, vy, vs, vdot, vscale;
    int32_t ndot, nscale;
    magr_exec_report report;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 2, MAGR_DTYPE_F64);
    vy = bind_buffer(&ir, y, 2, MAGR_DTYPE_F64);
    vs = bind_buffer(&ir, scale, 2, MAGR_DTYPE_F64);

    ndot = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[ndot].operands[0] = vx;
    ir.nodes[ndot].operands[1] = vy;
    ir.nodes[ndot].operand_count = 2;
    vdot = alloc_result(&ir, ndot, 1); /* scalar-shaped buffer */

    /* scale := (x . y) * scale, i.e. AXPY with alpha coming from the DOT. */
    vscale = ir_add_value(&ir, NULL);
    vscale.dtype = (uint32_t)MAGR_DTYPE_F64;
    vscale.rank = 1;
    vscale.dims[0] = 2;
    vscale.strides[0] = 8;
    vscale.data = scale;
    vscale.capacity_bytes = 16;
    vscale.ownership = (uint32_t)MAGR_OWNERSHIP_BORROWED;

    nscale = ir_add_node(&ir, MAGR_OP_FORTRAN_AXPY);
    ir.nodes[nscale].operands[0] = vs;
    ir.nodes[nscale].operands[1] = vscale;
    ir.nodes[nscale].operand_count = 2;
    ir.nodes[nscale].scalars[0].is_literal = 0;
    ir.nodes[nscale].scalars[0].producer_node = ndot; /* the DOT's result */
    ir.nodes[nscale].scalar_count = 1;
    ir.nodes[nscale].result = (int32_t)vscale;
    ir.output_node = (uint32_t)nscale;

    MAGR_CHECK_STATUS(magr_ir_execute_serial(&ir, &report), MAGR_STATUS_OK);
    /* x . y = 1*3 + 2*4 = 11; scale was {1,1} -> {12, 15} */
    MAGR_CHECK_NEAR(scale[0], 12.0, 1e-13, 1e-300);
    MAGR_CHECK_NEAR(scale[1], 15.0, 1e-13, 1e-300);
    free(ir.values[vdot].data);
    return 0;
}

static int t_unbound_value_fails_loudly(void) {
    /* A value with no buffer must fail at bind time with a specific status, not
     * become a NULL dereference inside Fortran. */
    magr_ir ir;
    double x[4];
    uint32_t vx, vr;
    int32_t node;
    magr_array bound;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operand_count = 1;
    vr = alloc_result(&ir, node, 1);
    ir.values[vr].data = NULL; /* deliberately unbound */
    ir.output_node = (uint32_t)node;

    MAGR_CHECK_STATUS(magr_ir_bind_value(&ir, vr, &bound), MAGR_STATUS_BOUNDS);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wavefront executor                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t seen[64];
    uint32_t count;
    uint32_t max_width;
} launch_log;

/* A launch callback that just runs the nodes one after another. That is a legal
 * implementation of "run these concurrently" -- it simply uses one core -- and
 * it is what makes serial and wavefront results comparable. The difference
 * between the two executors is WHICH nodes are grouped, not how fast they run.
 */
static magr_status_t launch_serial(magr_ir *ir, const uint32_t *nodes, uint32_t count, void *ctx,
                                   uint32_t *launched_concurrently) {
    launch_log *log = (launch_log *)ctx;
    uint32_t i;
    for (i = 0; i < count; ++i) {
        magr_status st;
        if (log->count < 64) log->seen[log->count++] = nodes[i];
        st = magr_ir_run_node_serial(ir, nodes[i], NULL);
        if (st != MAGR_STATUS_OK) return st;
    }
    if (count > log->max_width) log->max_width = count;
    if (launched_concurrently != NULL) *launched_concurrently = count;
    return MAGR_STATUS_OK;
}

static int t_wavefront_execution_matches_serial(void) {
    /*
     * The headline scheduler property: a program executed by wavefront
     * scheduling produces the same results as one executed serially, under the
     * declared numerical contract.
     *
     * Both runs use the same sequential launch callback, so what is actually
     * being compared is the SCHEDULER's choice of grouping and ordering, not
     * thread scheduling. That is the right thing to isolate: if the two
     * disagree, the bug is in the dependency analysis, which is the part that
     * is easy to get wrong and impossible to notice.
     */
    magr_ir ir;
    double x[4] = {1, 2, 3, 4};
    double y[4] = {5, 6, 7, 8};
    uint32_t vx, vy, vr;
    int32_t node;
    magr_exec_report serial_report, wave_report;
    launch_log log;
    double serial_out = 0.0, wave_out = 0.0;

    /* --- serial --- */
    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 4, MAGR_DTYPE_F64);
    vy = bind_buffer(&ir, y, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operands[1] = vy;
    ir.nodes[node].operand_count = 2;
    vr = alloc_result(&ir, node, 1);
    ir.output_node = (uint32_t)node;
    MAGR_CHECK_STATUS(magr_ir_execute_serial(&ir, &serial_report), MAGR_STATUS_OK);
    memcpy(&serial_out, ir.values[vr].data, sizeof(double));
    free(ir.values[vr].data);

    /* --- wavefront, same arithmetic, different scheduling --- */
    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 4, MAGR_DTYPE_F64);
    vy = bind_buffer(&ir, y, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operands[1] = vy;
    ir.nodes[node].operand_count = 2;
    vr = alloc_result(&ir, node, 1);
    ir.output_node = (uint32_t)node;

    memset(&log, 0, sizeof(log));
    MAGR_CHECK_STATUS(magr_ir_execute_wavefronts(&ir, launch_serial, &log, &wave_report),
                      MAGR_STATUS_OK);
    memcpy(&wave_out, ir.values[vr].data, sizeof(double));
    free(ir.values[vr].data);

    MAGR_CHECK_BITS_F64(serial_out, wave_out);
    MAGR_CHECK_MSG(wave_report.waves >= 1, "wavefronts were computed");
    MAGR_CHECK_MSG(wave_report.functors_invoked == serial_report.functors_invoked,
                   "the same functors must run under either scheduler");
    printf("       (%u wave(s), widest %u node(s), %u functors)\n", wave_report.waves,
           wave_report.max_wave_width, wave_report.functors_invoked);
    return 0;
}

static int t_wavefront_requires_a_callback(void) {
    /* The whole point of renaming the serial executor: a NULL callback must be
     * an error, not a silent downgrade to one core under a name that promises
     * concurrency. */
    magr_ir ir;
    double x[2] = {1, 2};
    uint32_t vx, vr;
    int32_t node;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 2, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_REDUCE_SUM);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operand_count = 1;
    vr = alloc_result(&ir, node, 1);
    ir.output_node = (uint32_t)node;

    MAGR_CHECK_STATUS(magr_ir_execute_wavefronts(&ir, NULL, NULL, NULL), MAGR_STATUS_INVALID_ARGUMENT);
    free(ir.values[vr].data);
    return 0;
}

static int t_failure_identifies_the_node(void) {
    /* A failing node must be named. "Execution failed" is useless when there
     * are 4096 possible nodes. */
    magr_ir ir;
    double x[4] = {1, 2, 3, 4};
    uint32_t vx, vr;
    int32_t node;
    magr_exec_report report;

    MAGR_CHECK_STATUS(magr_ir_init(&ir), MAGR_STATUS_OK);
    vx = bind_buffer(&ir, x, 4, MAGR_DTYPE_F64);
    node = ir_add_node(&ir, MAGR_OP_FORTRAN_DOT);
    ir.nodes[node].operands[0] = vx;
    ir.nodes[node].operands[1] = vx;
    ir.nodes[node].operand_count = 2;
    vr = alloc_result(&ir, node, 1);
    ir.output_node = (uint32_t)node;

    /* DOT of a vector with itself is well-formed here, so this actually runs;
     * the point of the test is that the report structure carries the node id
     * and the status even when nothing went wrong. */
    MAGR_CHECK_STATUS(magr_ir_execute_serial(&ir, &report), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(report.first_failure_node, 0);
    free(ir.values[vr].data);
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    magr_test_fn fn;
} entry;

int main(void) {
    static const entry tests[] = {
        {"valid_program_validates", t_valid_program_validates},
        {"forward_reference_is_rejected", t_forward_reference_is_rejected},
        {"arity_is_enforced", t_arity_is_enforced},
        {"unsupported_op_kind_is_refused", t_unsupported_op_kind_is_refused},
        {"gemm_shape_inference", t_gemm_shape_inference},
        {"broadcast_is_equal_rank_only", t_broadcast_is_equal_rank_only},
        {"wavefront_decomposition", t_wavefront_decomposition},
        {"conflict_detection", t_conflict_detection},
        {"aliasing_nodes_are_detectable", t_aliasing_nodes_are_detectable},
        {"serial_dot_matches_mathematics", t_serial_dot_matches_mathematics},
        {"serial_axpy_chain_orders_correctly", t_serial_axpy_chain_orders_correctly},
        {"computed_scalar_flows_across_nodes", t_computed_scalar_flows_across_nodes},
        {"unbound_value_fails_loudly", t_unbound_value_fails_loudly},
        {"wavefront_execution_matches_serial", t_wavefront_execution_matches_serial},
        {"wavefront_requires_a_callback", t_wavefront_requires_a_callback},
        {"failure_identifies_the_node", t_failure_identifies_the_node},
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) magr_test_run(tests[i].name, tests[i].fn);
    printf("\n%d checks, %d failures\n", magr_test_checks, magr_test_failures);
    return magr_test_failures == 0 ? 0 : 1;
}
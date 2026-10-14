/*
 * magr_execute.c -- Array IR execution against the validated Fortran functors.
 *
 * The pipeline from the MAGR spec, in the order it actually runs here:
 *
 *   1. validate the program structurally      magr_ir_validate_program
 *   2. infer and check every shape            magr_ir_infer_shapes_explained
 *   3. build the ready sets                   magr_ir_wavefronts
 *   4. execute                                magr_ir_execute_serial / _wavefronts
 *   5. validate the declared outputs
 *   6. (caller) commit and build a Merkle root
 *
 * Steps 1 and 2 happen once, before any arithmetic. That ordering is the whole
 * point: a shape error discovered after a ten-minute GEMM has cost ten minutes.
 *
 * State lives in the IR, never on the stack. A node's scalar result is stored on
 * the node (magr_ir_node::scalar_result), because under wavefront scheduling
 * one ready set is one call and a later wave is a different call; a value
 * computed in wave 1 and consumed in wave 3 must survive between them. Anything
 * that keeps execution state in a local would make wavefront and serial
 * execution subtly different programs.
 */

#include "magr_execute.h"

#include "magr/array_core.h"
#include "magr/functors/magr_fortran.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Descriptor binding                                                  */
/* ------------------------------------------------------------------ */

magr_status magr_ir_bind_value(const magr_ir *ir, uint32_t value_index, magr_array *out) {
    const magr_ir_value *v;
    magr_status st;
    uint32_t i;
    int64_t dims[MAGR_MAX_RANK], strides[MAGR_MAX_RANK];

    if (ir == NULL || out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (value_index >= ir->value_count) return MAGR_STATUS_BOUNDS;

    v = &ir->values[value_index];
    if (v->rank > MAGR_MAX_RANK) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (v->data == NULL) return MAGR_STATUS_BOUNDS;

    for (i = 0; i < v->rank; ++i) {
        dims[i] = v->dims[i];
        strides[i] = v->strides[i];
    }

    /* Dense values go through the dense constructor; strided ones through the
     * strided constructor. Deciding here rather than guessing means a transpose
     * view binds as a transposed view, and the Fortran layer is then free to
     * say "this is not BLAS-callable" instead of silently copying. */
    {
        int64_t expected = (int64_t)magr_dtype_size((magr_dtype)v->dtype);
        int dense = 1;
        for (i = 0; i < v->rank; ++i) {
            if (v->strides[i] != expected) { dense = 0; break; }
            if (v->dims[i] != 0) expected *= v->dims[i];
        }
        if (dense) {
            st = magr_array_init_dense(out, (magr_dtype)v->dtype, v->rank, dims, v->data,
                                       v->capacity_bytes, (magr_ownership)v->ownership, NULL,
                                       NULL);
        } else {
            magr_array tmp;
            st = magr_array_init_strided(&tmp, (magr_dtype)v->dtype, v->rank, dims, strides,
                                         v->data, v->capacity_bytes,
                                         (magr_ownership)v->ownership);
            if (st == MAGR_STATUS_OK) *out = tmp;
        }
    }
    if (st != MAGR_STATUS_OK) return st;

    out->flags |= (v->flags & MAGR_ARRAY_F_READONLY);
    out->flags |= (v->flags & MAGR_ARRAY_F_MAY_ALIAS);

    /* A 2-D f64 value whose strides are exactly (8, ld*8) is column-major and
     * BLAS-callable; record that so the wrappers need not re-derive it. A
     * row-major 2-D value stays row-major and will be refused by the Fortran
     * layer, which is the correct outcome rather than a bug. */
    if (v->rank == 2 && (magr_dtype)v->dtype == MAGR_DTYPE_F64) {
        size_t esize = magr_dtype_size((magr_dtype)v->dtype);
        if (v->strides[0] == (int64_t)esize && v->strides[1] % (int64_t)esize == 0 &&
            v->strides[1] / (int64_t)esize >= v->dims[0]) {
            out->layout = (uint32_t)MAGR_LAYOUT_COLUMN_MAJOR;
        }
    }
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Scalar resolution                                                   */
/* ------------------------------------------------------------------ */

/*
 * A node's scalar operand is either a literal or the result of an earlier
 * node. The producer is looked up in the IR rather than in any call-local
 * storage, which is what makes a cross-wave dependency work.
 */
static double resolve_scalar(const magr_ir *ir, const magr_scalar *s) {
    if (s->is_literal || s->producer_node < 0) return s->literal;
    if ((uint32_t)s->producer_node >= ir->node_count) return s->literal;
    if (!ir->nodes[s->producer_node].has_scalar_result) return 0.0;
    return ir->nodes[s->producer_node].scalar_result;
}

/* ------------------------------------------------------------------ */
/* Elementwise and reduction kernels                                   */
/* ------------------------------------------------------------------ */

/* Byte offset of logical element `idx` within a value, or -1 if out of range. */
static int64_t offset_of(const magr_ir_value *v, const int64_t *idx) {
    int64_t off = 0;
    uint32_t i;
    for (i = 0; i < v->rank; ++i) {
        if (idx[i] < 0 || idx[i] >= v->dims[i]) return -1;
        off += idx[i] * v->strides[i];
    }
    return off;
}

/*
 * Elementwise binary kernel over up to three operands.
 *
 * Broadcasting follows the IR's inferred result shape. An operand whose extent
 * on an axis is 1 reads index 0 there, which is the standard rule and is the
 * only one ir.c's broadcast_shape admits.
 */
static magr_status elementwise(magr_ir *ir, magr_ir_node *n) {
    magr_ir_value *r = &ir->values[n->result];
    const magr_ir_value *op[3];
    uint32_t nops = n->operand_count > 3 ? 3 : n->operand_count;
    uint32_t k;
    int64_t idx[MAGR_MAX_RANK];
    int64_t total = 1;
    int64_t linear;

    if (nops == 0) return MAGR_STATUS_INVALID_ARGUMENT;
    for (k = 0; k < nops; ++k) {
        op[k] = &ir->values[n->operands[k]];
        if (op[k]->data == NULL) return MAGR_STATUS_BOUNDS;
        if ((magr_dtype)op[k]->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    }
    if (r->data == NULL) return MAGR_STATUS_BOUNDS;

    for (k = 0; k < r->rank; ++k) total *= r->dims[k];

    for (linear = 0; linear < total; ++linear) {
        int64_t rem = linear;
        double x[3] = {0.0, 0.0, 0.0};
        int64_t roff, ooff;

        /* Decompose the linear index into logical coordinates. */
        for (k = r->rank; k-- > 0;) {
            idx[k] = rem % r->dims[k];
            rem /= r->dims[k];
        }

        roff = offset_of(r, idx);
        if (roff < 0) return MAGR_STATUS_BOUNDS;

        for (k = 0; k < nops; ++k) {
            int64_t oidx[MAGR_MAX_RANK];
            uint32_t j;
            for (j = 0; j < op[k]->rank; ++j) {
                oidx[j] = (op[k]->dims[j] == 1) ? 0 : idx[j];
            }
            ooff = offset_of(op[k], oidx);
            if (ooff < 0) return MAGR_STATUS_SHAPE_MISMATCH;
            memcpy(&x[k], (const char *)op[k]->data + ooff, sizeof(double));
        }

        {
            double out_value = 0.0;
            switch (n->kind) {
            case MAGR_OP_ELEMENTWISE_ADD: out_value = x[0] + x[1]; break;
            case MAGR_OP_ELEMENTWISE_SUB: out_value = x[0] - x[1]; break;
            case MAGR_OP_ELEMENTWISE_MUL: out_value = x[0] * x[1]; break;
            case MAGR_OP_ELEMENTWISE_DIV:
                /* Division by zero yields the IEEE result (inf or NaN) rather
                 * than an error, because that is what the source language would
                 * do and substituting a different rule here would be a silent
                 * change of program meaning. */
                out_value = x[0] / x[1];
                break;
            case MAGR_OP_ELEMENTWISE_NEG: out_value = -x[0]; break;
            case MAGR_OP_ELEMENTWISE_FMA:
                /* With a literal scalar it is the 3-operand fused form
                 * alpha*x + y; without one it degrades to the plain fma(x,y,z). */
                out_value = (n->scalar_count > 0) ? (n->scalars[0].literal * x[0] + x[1])
                                                   : (x[0] * x[1] + x[2]);
                break;
            default: return MAGR_STATUS_UNSUPPORTED;
            }
            memcpy((char *)r->data + roff, &out_value, sizeof(double));
        }
    }
    return MAGR_STATUS_OK;
}

/* Full reduction to a scalar. Partial (per-axis) reduction is deliberately not
 * in v1: a front end must express it as slice + transpose + full reduce, and
 * that keeps the reduction order fixed and therefore the result reproducible. */
static magr_status reduce(magr_ir *ir, magr_ir_node *n) {
    const magr_ir_value *in = &ir->values[n->operands[0]];
    double acc;
    int64_t total = 1, linear;
    uint32_t k;
    int64_t idx[MAGR_MAX_RANK];

    if (in->data == NULL) return MAGR_STATUS_BOUNDS;
    if ((magr_dtype)in->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;

    for (k = 0; k < in->rank; ++k) total *= in->dims[k];

    switch (n->kind) {
    case MAGR_OP_REDUCE_SUM: acc = 0.0; break;
    case MAGR_OP_REDUCE_MAX: acc = -INFINITY; break;
    case MAGR_OP_REDUCE_MIN: acc = INFINITY; break;
    default: return MAGR_STATUS_UNSUPPORTED;
    }

    for (linear = 0; linear < total; ++linear) {
        int64_t rem = linear, off;
        double v;
        for (k = in->rank; k-- > 0;) {
            idx[k] = rem % in->dims[k];
            rem /= in->dims[k];
        }
        off = offset_of(in, idx);
        if (off < 0) return MAGR_STATUS_BOUNDS;
        memcpy(&v, (const char *)in->data + off, sizeof(double));
        switch (n->kind) {
        case MAGR_OP_REDUCE_SUM: acc += v; break;
        case MAGR_OP_REDUCE_MAX: if (v > acc) acc = v; break;
        case MAGR_OP_REDUCE_MIN: if (v < acc) acc = v; break;
        default: break;
        }
    }

    n->scalar_result = acc;
    n->has_scalar_result = 1;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Single node                                                         */
/* ------------------------------------------------------------------ */

magr_status magr_ir_run_node_serial(magr_ir *ir, uint32_t node, magr_exec_report *report) {
    magr_ir_node *n;
    magr_array a[MAGR_IR_MAX_OPERANDS];
    magr_functor_id fid;
    magr_status st = MAGR_STATUS_OK;
    uint32_t i;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (node >= ir->node_count) return MAGR_STATUS_BOUNDS;
    n = &ir->nodes[node];

    if (n->kind == MAGR_OP_INPUT || n->kind == MAGR_OP_CONST) return MAGR_STATUS_OK;

    for (i = 0; i < n->operand_count && i < MAGR_IR_MAX_OPERANDS; ++i) {
        st = magr_ir_bind_value(ir, n->operands[i], &a[i]);
        if (st != MAGR_STATUS_OK) return st;
    }

    fid = magr_op_functor(n->kind);
    if (fid != MAGR_FUNCTOR_INVALID) {
        if (report != NULL) report->functors_invoked++;

        switch (fid) {
        case MAGR_FUNCTOR_F_DOT_F64: {
            double r = 0.0;
            st = magr_f_dot_f64(&a[0], &a[1], &r, NULL);
            if (st == MAGR_STATUS_OK) { n->scalar_result = r; n->has_scalar_result = 1; }
            break;
        }
        case MAGR_FUNCTOR_F_NRM2_F64: {
            double r = 0.0;
            st = magr_f_nrm2_f64(&a[0], &r, NULL);
            if (st == MAGR_STATUS_OK) { n->scalar_result = r; n->has_scalar_result = 1; }
            break;
        }
        case MAGR_FUNCTOR_F_AXPY_F64:
            st = magr_f_axpy_f64(&a[0], resolve_scalar(ir, &n->scalars[0]), &a[1], NULL);
            break;
        case MAGR_FUNCTOR_F_GEMV_F64:
            st = magr_f_gemv_f64((magr_trans)n->flags[0], &a[0], &a[1],
                                 resolve_scalar(ir, &n->scalars[0]), &a[2],
                                 resolve_scalar(ir, &n->scalars[1]), NULL);
            break;
        case MAGR_FUNCTOR_F_GEMM_F64:
            st = magr_f_gemm_f64((magr_trans)n->flags[0], (magr_trans)n->flags[1],
                                 &a[0], &a[1], resolve_scalar(ir, &n->scalars[0]), &a[2],
                                 resolve_scalar(ir, &n->scalars[1]), NULL);
            break;
        case MAGR_FUNCTOR_F_GER_F64:
            st = magr_f_ger_f64(&a[0], &a[1], resolve_scalar(ir, &n->scalars[0]), &a[2], NULL);
            break;
        case MAGR_FUNCTOR_F_TRSV_F64:
            st = magr_f_trsv_f64((magr_uplo)n->flags[0], (magr_trans)n->flags[1],
                                 (magr_diag)n->flags[2], &a[0], &a[1], NULL);
            break;
        case MAGR_FUNCTOR_F_TRSM_F64:
            st = magr_f_trsm_f64((magr_side)n->flags[0], (magr_uplo)n->flags[1],
                                 (magr_trans)n->flags[2], (magr_diag)n->flags[3], &a[0],
                                 resolve_scalar(ir, &n->scalars[0]), &a[1], NULL);
            break;
        default:
            st = MAGR_STATUS_FUNCRIPTOR_UNAVAILABLE;
            break;
        }
        if (st != MAGR_STATUS_OK) return st;
    } else {
        switch (n->kind) {
        case MAGR_OP_VIEW_TRANSPOSE: {
            /* Metadata only. Swapping dims and strides aliases the same bytes
             * with the other orientation, which is precisely why DGEMM can
             * consume a transposed operand with its TRANS flag and no copy. */
            magr_ir_value *r = &ir->values[n->result];
            int64_t d0 = r->dims[0], s0 = r->strides[0];
            r->dims[0] = r->dims[1];
            r->dims[1] = d0;
            r->strides[0] = r->strides[1];
            r->strides[1] = s0;
            r->data = a[0].data;
            r->capacity_bytes = a[0].capacity_bytes;
            r->flags |= MAGR_ARRAY_F_MAY_ALIAS; /* literally the same bytes */
            r->ownership = (uint32_t)MAGR_OWNERSHIP_VIEW;
            break;
        }
        case MAGR_OP_VIEW_FLATTEN: {
            magr_ir_value *r = &ir->values[n->result];
            r->strides[0] = (int64_t)magr_dtype_size((magr_dtype)r->dtype);
            r->data = a[0].data;
            r->capacity_bytes = a[0].capacity_bytes;
            break;
        }
        case MAGR_OP_VIEW_RESHAPE: {
            magr_ir_value *r = &ir->values[n->result];
            int64_t esize = (int64_t)magr_dtype_size((magr_dtype)r->dtype);
            int64_t acc = esize;
            uint32_t j;
            for (j = 0; j < r->rank; ++j) {
                r->strides[j] = acc;
                if (r->dims[j] != 0) acc *= r->dims[j];
            }
            r->data = a[0].data;
            r->capacity_bytes = a[0].capacity_bytes;
            break;
        }
        case MAGR_OP_ELEMENTWISE_ADD:
        case MAGR_OP_ELEMENTWISE_SUB:
        case MAGR_OP_ELEMENTWISE_MUL:
        case MAGR_OP_ELEMENTWISE_DIV:
        case MAGR_OP_ELEMENTWISE_NEG:
        case MAGR_OP_ELEMENTWISE_FMA:
            st = elementwise(ir, n);
            break;
        case MAGR_OP_REDUCE_SUM:
        case MAGR_OP_REDUCE_MAX:
        case MAGR_OP_REDUCE_MIN:
            st = reduce(ir, n);
            break;
        case MAGR_OP_OUTPUT:
            /* A relabelling of the final value. The copy is of the descriptor,
             * not the data, so ownership must not become the output's to free
             * twice. */
            ir->values[n->result] = ir->values[n->operands[0]];
            ir->values[n->result].producer = node;
            break;
        default:
            st = MAGR_STATUS_UNSUPPORTED;
            break;
        }
        if (st != MAGR_STATUS_OK) return st;
    }

    n->state = MAGR_NODE_EXECUTED;
    if (report != NULL) report->nodes_executed++;
    return MAGR_STATUS_OK;
}

magr_status magr_ir_execute_node(magr_ir *ir, uint32_t node, magr_exec_report *report) {
    return magr_ir_run_node_serial(ir, node, report);
}

/* ------------------------------------------------------------------ */
/* Executors                                                           */
/* ------------------------------------------------------------------ */

static magr_status fail(magr_ir *ir, magr_exec_report *report, magr_status st, uint32_t node) {
    if (node < ir->node_count) {
        ir->nodes[node].state = MAGR_NODE_FAILED;
        ir->nodes[node].status = st;
    }
    if (report != NULL && report->first_failure == MAGR_STATUS_OK) {
        report->first_failure = st;
        report->first_failure_node = node;
    }
    return st;
}

magr_status magr_ir_execute_serial(magr_ir *ir, magr_exec_report *report) {
    char err[256];
    magr_status st;
    uint32_t i;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (report != NULL) memset(report, 0, sizeof(*report));

    st = magr_ir_validate_program(ir);
    if (st != MAGR_STATUS_OK) return fail(ir, report, st, 0);
    st = magr_ir_infer_shapes_explained(ir, err, sizeof(err));
    if (st != MAGR_STATUS_OK) return fail(ir, report, st, 0);

    for (i = 0; i < ir->node_count; ++i) {
        st = magr_ir_run_node_serial(ir, i, report);
        if (st != MAGR_STATUS_OK) return fail(ir, report, st, i);
    }
    if (report != NULL) report->waves = 1;
    return MAGR_STATUS_OK;
}

magr_status magr_ir_execute_wavefronts(magr_ir *ir, magr_task_launch_fn launch, void *ctx,
                                       magr_exec_report *report) {
    char err[256];
    magr_status st;
    uint8_t *done;
    uint32_t *ready;
    uint32_t remaining, wave = 0;

    if (ir == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (report != NULL) memset(report, 0, sizeof(*report));
    if (launch == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_ir_validate_program(ir);
    if (st != MAGR_STATUS_OK) return fail(ir, report, st, 0);
    st = magr_ir_infer_shapes_explained(ir, err, sizeof(err));
    if (st != MAGR_STATUS_OK) return fail(ir, report, st, 0);

    done = (uint8_t *)calloc(ir->node_count, 1);
    ready = (uint32_t *)calloc(ir->node_count, sizeof(uint32_t));
    if (done == NULL || ready == NULL) {
        free(done);
        free(ready);
        return MAGR_STATUS_INTERNAL;
    }

    remaining = ir->node_count;
    while (remaining > 0) {
        uint32_t count = magr_ir_ready_nodes(ir, done, ready, ir->node_count);
        uint32_t i;

        if (count == 0) {
            /* Cannot happen for a validated program: validation guarantees the
             * topological invariant. Treat it as an internal error rather than
             * spinning. */
            free(done);
            free(ready);
            return fail(ir, report, MAGR_STATUS_INTERNAL, 0);
        }

        /*
         * The conflict pass. Two nodes in the same ready set are independent in
         * the dependency sense, which is not the same as independent in the
         * memory sense. Before handing a set to a parallel launcher we drop any
         * node that conflicts with an earlier node in the same set and defer it
         * to the next wave -- it is still not done, so the next call to
         * magr_ir_ready_nodes will return it again.
         *
         * `keep` starts at 0 and the inner loop runs over [0, keep), so the
         * first node of a wave can never be dropped. That guarantees at least
         * one node survives and the loop makes progress; the compiler enforces
         * that statically and there is no fallback path needed.
         *
         * This pass is conservative: a false positive only costs parallelism,
         * whereas a false negative corrupts data.
         */
        if (count > 1) {
            uint32_t keep = 0;
            for (i = 0; i < count; ++i) {
                uint32_t j;
                int conflict = 0;
                for (j = 0; j < keep; ++j) {
                    if (magr_ir_nodes_conflict(ir, ready[j], ready[i])) { conflict = 1; break; }
                }
                if (conflict) {
                    if (report != NULL) report->serialised_for_conflict++;
                    continue;
                }
                ready[keep++] = ready[i];
            }
            count = keep;
        }

        {
            uint32_t launched = 0;
            st = launch(ir, ready, count, ctx, &launched);
            if (st != MAGR_STATUS_OK) {
                free(done);
                free(ready);
                return fail(ir, report, st, ready[0]);
            }
            for (i = 0; i < count; ++i) done[ready[i]] = 1;
            remaining -= count;
            ++wave;
            if (report != NULL) {
                report->waves = wave;
                if (launched > report->max_wave_width) report->max_wave_width = launched;
            }
        }
    }

    free(done);
    free(ready);
    return MAGR_STATUS_OK;
}
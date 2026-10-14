/*
 * magr_ir_schedule.c -- dependency analysis and wavefront decomposition for the
 * Array IR.
 *
 * Separated from ir.c because this layer answers questions ABOUT the program
 * (what order, how much parallelism, do two nodes touch the same memory) and
 * does not execute anything. Execution lives in chapel-runtime/magr_execute.c,
 * which is the only place that combines this analysis with the Fortran
 * functors.
 *
 * The conflict predicate in this file is the single most important function in
 * the scheduler. A scheduler that decides independence from node ids, or from
 * the IR author's belief that two nodes are unrelated, will eventually run two
 * nodes against the same buffer and produce a wrong answer that looks fine.
 */

#include "magr/ir.h"

#include <stdlib.h>

/* Byte span a value's logical extent can touch, measured from its base
 * pointer. For a strided view this is the extreme byte, not the dense size --
 * a strided view of a larger buffer touches bytes the dense size does not
 * cover, and treating them as disjoint is how two "independent" nodes end up
 * writing the same element. */
static int64_t span_of(const magr_ir *ir, uint32_t value_index) {
    const magr_ir_value *v;
    int64_t min_off = 0, max_off = 0;
    int first = 1;
    uint32_t i;

    if (ir == NULL || value_index >= ir->value_count) return 0;
    v = &ir->values[value_index];
    for (i = 0; i < v->rank; ++i) {
        int64_t s, span;
        if (v->dims[i] == 0) continue;
        s = v->strides[i];
        span = s * (v->dims[i] - 1);
        if (first) {
            min_off = (s < 0) ? span : 0;
            max_off = (s < 0) ? 0 : span;
            first = 0;
        } else {
            if (s < 0) { if (span < min_off) min_off = span; }
            else       { if (span > max_off) max_off = span; }
        }
    }
    if (first) return 0;
    return max_off - min_off + (int64_t)magr_dtype_size((magr_dtype)v->dtype);
}

/* Internal: does the value index correspond to memory this node touches? */
static int node_touches_value(const magr_ir_node *n, uint32_t value_index) {
    uint32_t i;
    for (i = 0; i < n->operand_count; ++i) {
        if (n->operands[i] == value_index) return 1;
    }
    /* A node also touches whatever it wrote, for the purposes of conflict with
     * a node that read the value earlier: the buffer identity is what matters
     * and the buffer is the same either way. */
    if (n->result >= 0 && (uint32_t)n->result == value_index) return 1;
    return 0;
}

int magr_ir_nodes_conflict(const magr_ir *ir, uint32_t node_a, uint32_t node_b) {
    const magr_ir_node *na, *nb;
    uint32_t va, vb;

    if (ir == NULL) return 1; /* fail safe: assume conflict */
    if (node_a >= ir->node_count || node_b >= ir->node_count) return 1;
    if (node_a == node_b) return 1;

    na = &ir->nodes[node_a];
    nb = &ir->nodes[node_b];

    /* Compare by physical buffer identity. Two logical arrays over the same
     * memory at different offsets can still overlap, so we compare the byte
     * range each node can touch, not just the base pointer. */
    for (va = 0; va < ir->value_count; ++va) {
        const magr_ir_value *av = &ir->values[va];
        for (vb = 0; vb < ir->value_count; ++vb) {
            const magr_ir_value *bv = &ir->values[vb];
            const unsigned char *ab, *bb;
            int64_t alen, blen;
            if (av->data == NULL || bv->data == NULL) continue;
            if (!node_touches_value(na, va) && !node_touches_value(na, vb)) continue;
            if (!node_touches_value(nb, vb) && !node_touches_value(nb, va)) continue;

            ab = (const unsigned char *)av->data;
            bb = (const unsigned char *)bv->data;
            alen = span_of(ir, va);
            blen = span_of(ir, vb);
            if (alen <= 0 || blen <= 0) continue;
            /* Half-open interval overlap. Using half-open intervals is what
             * makes two adjacent slices non-conflicting; using inclusive
             * endpoints would make every neighbouring element a conflict and
             * silently serialise the entire program. */
            if (ab < bb + blen && bb < ab + alen) return 1;
        }
    }
    return 0;
}

int magr_ir_has_aliasing_nodes(const magr_ir *ir) {
    uint32_t i;
    if (ir == NULL) return 0;
    for (i = 0; i < ir->node_count; ++i) {
        uint32_t k;
        for (k = 0; k < ir->nodes[i].operand_count; ++k) {
            const magr_ir_value *v = &ir->values[ir->nodes[i].operands[k]];
            if (v != NULL && (v->flags & MAGR_ARRAY_F_MAY_ALIAS) != 0) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ready sets and wavefronts                                           */
/* ------------------------------------------------------------------ */

uint32_t magr_ir_ready_nodes(const magr_ir *ir, const uint8_t *done, uint32_t *ready,
                             uint32_t max_ready) {
    uint32_t count = 0, i, j;

    if (ir == NULL || done == NULL || ready == NULL) return 0;
    for (i = 0; i < ir->node_count && count < max_ready; ++i) {
        int ready_now = 1;
        if (done[i]) continue;
        for (j = 0; j < i && ready_now; ++j) {
            /* Node i consumes value v, produced by node p; i is ready only if
             * every producer is done. Producers are always < i by the
             * topological invariant. */
            uint32_t k;
            for (k = 0; k < ir->nodes[i].operand_count; ++k) {
                uint32_t producer = ir->values[ir->nodes[i].operands[k]].producer;
                if (producer != UINT32_MAX && !done[producer]) { ready_now = 0; break; }
            }
        }
        if (ready_now) ready[count++] = i;
    }
    return count;
}

uint32_t magr_ir_topological_order(const magr_ir *ir, uint32_t *order) {
    uint8_t *done;
    uint32_t n = 0;

    if (ir == NULL || order == NULL) return 0;
    done = (uint8_t *)calloc(ir->node_count, 1);
    if (done == NULL) return 0;

    while (n < ir->node_count) {
        uint32_t ready[MAGR_IR_MAX_OPS];
        uint32_t count = magr_ir_ready_nodes(ir, done, ready, MAGR_IR_MAX_OPS);
        uint32_t i;
        if (count == 0) break; /* cycle: caller must have validated already */
        for (i = 0; i < count; ++i) {
            done[ready[i]] = 1;
            order[n++] = ready[i];
        }
    }
    free(done);
    return n;
}

uint32_t magr_ir_wavefronts(const magr_ir *ir, uint32_t *starts, uint32_t *sizes,
                            uint32_t max_waves) {
    uint8_t *done;
    uint32_t waves = 0, total = 0;

    if (ir == NULL || starts == NULL || sizes == NULL || max_waves == 0) return 0;
    done = (uint8_t *)calloc(ir->node_count, 1);
    if (done == NULL) return 0;

    while (total < ir->node_count && waves < max_waves) {
        uint32_t ready[MAGR_IR_MAX_OPS];
        uint32_t count = magr_ir_ready_nodes(ir, done, ready, MAGR_IR_MAX_OPS);
        uint32_t i;
        if (count == 0) break;
        starts[waves] = total;
        sizes[waves] = count;
        total += count;
        for (i = 0; i < count; ++i) done[ready[i]] = 1;
        ++waves;
    }
    free(done);
    return waves;
}

uint32_t magr_ir_depth(const magr_ir *ir) {
    uint32_t starts[MAGR_IR_MAX_OPS], sizes[MAGR_IR_MAX_OPS];
    return magr_ir_wavefronts(ir, starts, sizes, MAGR_IR_MAX_OPS);
}
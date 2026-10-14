/*
 * magr/ir.h -- the typed Array IR.
 *
 * Chapel, Klong, and Uiua do not lower to each other and are not expected to.
 * They lower to THIS, and this is the only thing the scheduler executes. That
 * is what makes "unsupported operations fail explicitly" a structural property
 * rather than a promise: an operation that no front end can express does not
 * exist as an IR node, so there is nothing to execute.
 *
 * The IR is a data structure, not a text format. `magr/ir_json.h` reads and
 * writes a canonical JSON projection of it, which is how the Uiua adapter hands
 * a program to the runtime and how the execution manifest records what ran.
 *
 * Node kinds are deliberately split into three families:
 *
 *   MAGR_OP_FORTRAN_*  lowers to exactly one validated Fortran functor call.
 *                     These are the numerically load-bearing operations.
 *   MAGR_OP_VIEW_*     pure metadata manipulation (reshape, slice, transpose,
 *                     broadcast). No arithmetic, no data movement, and
 *                     therefore safe to reorder and to fuse.
 *   MAGR_OP_ELEMENTWISE_*  arithmetic MAGR implements itself, because no
 *                     reference BLAS routine covers it. These carry their own
 *                     elementwise semantic contract and are the boundary where
 *                     "preserve the numerical semantics of the source
 *                     language" actually costs something.
 *
 * Anything that cannot be expressed in one of these families is
 * MAGR_OP_UNSUPPORTED at load time, not a runtime surprise.
 */

#ifndef MAGR_IR_H
#define MAGR_IR_H

#include "magr/abi/array_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_IR_MAX_OPS 4096
#define MAGR_IR_MAX_OPERANDS 8
#define MAGR_IR_MAX_SCALARS 4
#define MAGR_IR_MAX_DIMS MAGR_MAX_RANK

typedef enum magr_op_kind {
    MAGR_OP_UNSUPPORTED = 0,

    /* Fortran functors */
    MAGR_OP_FORTRAN_DOT = 1,
    MAGR_OP_FORTRAN_AXPY = 2,
    MAGR_OP_FORTRAN_NRM2 = 3,
    MAGR_OP_FORTRAN_GEMV = 4,
    MAGR_OP_FORTRAN_GEMM = 5,
    MAGR_OP_FORTRAN_GER = 6,
    MAGR_OP_FORTRAN_TRSV = 7,
    MAGR_OP_FORTRAN_TRSM = 8,

    /* Views */
    MAGR_OP_VIEW_RESHAPE = 16,
    MAGR_OP_VIEW_SLICE = 17,
    MAGR_OP_VIEW_TRANSPOSE = 18,
    MAGR_OP_VIEW_BROADCAST = 19,
    MAGR_OP_VIEW_FLATTEN = 20,

    /* Elementwise arithmetic implemented by MAGR */
    MAGR_OP_ELEMENTWISE_ADD = 32,
    MAGR_OP_ELEMENTWISE_SUB = 33,
    MAGR_OP_ELEMENTWISE_MUL = 34,
    MAGR_OP_ELEMENTWISE_DIV = 35,
    MAGR_OP_ELEMENTWISE_NEG = 36,
    MAGR_OP_ELEMENTWISE_FMA = 37,

    /* Reductions not covered by BLAS */
    MAGR_OP_REDUCE_SUM = 48,
    MAGR_OP_REDUCE_MAX = 49,
    MAGR_OP_REDUCE_MIN = 50,

    /* Control */
    MAGR_OP_CONST = 64,
    MAGR_OP_INPUT = 65,
    MAGR_OP_OUTPUT = 66
} magr_op_kind;

const char *magr_op_kind_name(magr_op_kind kind);
magr_op_kind magr_op_kind_from_name(const char *name);

/* Number of array operands a node of this kind requires. The loader uses this
 * to reject a malformed program before any descriptor is built. */
uint32_t magr_op_kind_arity(magr_op_kind kind);

/* Maps a Fortran-backed node to its functor ID, or MAGR_FUNCTOR_INVALID. */
magr_functor_id magr_op_functor(magr_op_kind kind);

/* Whether the scheduler may reorder this node relative to others. Stateful or
 * aliasing operations answer 0 and the scheduler serialises them. */
int magr_op_is_pure(magr_op_kind kind);

/* Whether the node can be fused with an adjacent node without changing
 * results. Only view ops and elementwise ops qualify. Fortran functors do not:
 * DGEMM's accumulation order is part of its contract. */
int magr_op_is_fusable(magr_op_kind kind);

typedef enum magr_node_state {
    MAGR_NODE_UNRESOLVED = 0,
    MAGR_NODE_VALIDATED = 1,
    MAGR_NODE_EXECUTED = 2,
    MAGR_NODE_FAILED = 3
} magr_node_state;

/* Scalar operand. Either an immediate (lit) or a reference to another node's
 * scalar result, which is how a computed dot product feeds a GEMM's alpha. */
typedef struct magr_scalar {
    double literal;
    int is_literal;
    int32_t producer_node; /* index into ir->nodes, or -1 */
} magr_scalar;

typedef struct magr_ir_node {
    magr_op_kind kind;
    int32_t result;        /* index of this node's output value */
    uint32_t operands[MAGR_IR_MAX_OPERANDS];
    uint32_t operand_count;
    magr_scalar scalars[MAGR_IR_MAX_SCALARS];
    uint32_t scalar_count;
    /* i64 flag values, meaning depends on kind (TRANS, UPLO, DIAG, SIDE). */
    int64_t flags[4];
    uint32_t flag_count;
    /* Output shape, filled by validation, not by the front end. */
    int64_t dims[MAGR_IR_MAX_DIMS];
    uint32_t rank;
    magr_dtype dtype;

    /* Scalar result of a value-producing reduction (DOT, NRM2, reduce.sum).
     * Stored on the NODE, not in a stack frame, because a node may be executed
     * in one call and its scalar consumed by a node executed in a later call --
     * which is exactly what happens under wavefront scheduling, where each
     * ready set is a separate call. Keeping this in the IR is what makes
     * serial and wavefront execution produce the same program state. */
    double scalar_result;
    uint8_t has_scalar_result;

    magr_node_state state;
    magr_status status; /* when state == MAGR_NODE_FAILED */
} magr_ir_node;

/* A value flowing through the IR. Each is backed by one buffer in one arena. */
typedef struct magr_ir_value {
    magr_dtype dtype;
    uint32_t rank;
    int64_t dims[MAGR_IR_MAX_DIMS];
    int64_t strides[MAGR_IR_MAX_DIMS];
    int64_t capacity_bytes;
    void *data;
    uint32_t flags;
    uint32_t producer; /* node index that produced it, or -1 for inputs */
    magr_ownership ownership;
} magr_ir_value;

typedef struct magr_ir {
    uint32_t struct_size;
    uint32_t abi_version;
    magr_ir_node nodes[MAGR_IR_MAX_OPS];
    uint32_t node_count;
    magr_ir_value values[MAGR_IR_MAX_OPS];
    uint32_t value_count;
    /* Input bindings, name -> value index. Kept as a small flat list because a
     * hash map here would be more code than the whole rest of the loader. */
    char (*input_names)[64];
    uint32_t input_count;
    uint32_t output_node; /* node whose result is the program's output */
} magr_ir;

magr_status magr_ir_init(magr_ir *ir);

/*
 * Structural validation of a loaded program: arity, operand indices in range,
 * no cycles (enforced by requiring that an operand is either an input or a
 * node with a strictly lower index -- the loader enforces a topological order,
 * which makes cycle detection free), scalar producers in range, and op-kind
 * recognised.
 *
 * Doing this before execution means a malformed program fails with a specific
 * status rather than producing garbage or dereferencing a bad descriptor.
 */
magr_status magr_ir_validate_program(const magr_ir *ir);

/*
 * Shape and contract validation using the descriptors in `values`. Fills each
 * node's `rank`, `dims`, and `dtype`. This is where a lowered Uiua tacit
 * program's broadcasting is decided, and it is deliberately conservative: it
 * refuses to guess.
 */
magr_status magr_ir_infer_shapes(magr_ir *ir);

/* Convenience: infer then report the first failure with a human-readable note
 * into `err` (may be NULL). */
magr_status magr_ir_infer_shapes_explained(magr_ir *ir, char *err, size_t err_len);

/* ------------------------------------------------------------------ */
/* Scheduling                                                          */
/* ------------------------------------------------------------------ */

/*
 * The IR arrives in topological order (enforced by magr_ir_validate_program),
 * so a valid program needs no sort. These functions expose the scheduling
 * structure explicitly anyway, because "the order happens to already be right"
 * is not something a scheduler should rely on implicitly -- if that invariant
 * is ever relaxed, the failure should be loud.
 *
 * Kahn's algorithm: repeatedly take the set of nodes whose dependencies are all
 * satisfied. Nodes within one ready set are mutually independent and are the
 * only ones that may run concurrently.
 */
int magr_ir_topological_order(const magr_ir *ir, uint32_t *order);

/* Fills `ready` with the nodes whose dependencies have all completed, given
 * `done[]` (length ir->node_count). Returns the count written. */
uint32_t magr_ir_ready_nodes(const magr_ir *ir, const uint8_t *done, uint32_t *ready,
                             uint32_t max_ready);

/*
 * Wavefront decomposition: fills `sizes[i]` with the number of nodes in wave i
 * and `starts[i]` with the offset into `order`. Returns the number of waves.
 *
 * A wave is the set of nodes that could all run at the same time. This is
 * exactly the information Chapel's `cobegin` needs, and it is also the
 * measurement of how much parallelism a program actually has.
 */
uint32_t magr_ir_wavefronts(const magr_ir *ir, uint32_t *starts, uint32_t *sizes,
                            uint32_t max_waves);

/*
 * Critical path length, in nodes. Equals the number of waves, so it is reported
 * separately only to make the distinction explicit: it is the lower bound on
 * makespan under unlimited parallelism.
 */
uint32_t magr_ir_depth(const magr_ir *ir);

/*
 * THE ONE SCHEDULING INVARIANT THAT IS NOT NEGOTIABLE:
 *
 * Two nodes may run concurrently only if the memory they touch is disjoint.
 * "Disjoint" is decided on the buffer base pointer and capacity of every array
 * each node reads or writes -- not on the node ids, and not on the IR's claim
 * that the nodes are "independent". A GEMM writing C and an AXPY reading the
 * same C are different nodes and are NOT independent.
 *
 * magr_ir_nodes_conflict(a, b) is that predicate. The scheduler must consult it
 * before launching anything concurrently. An earlier revision of this tree
 * named its serial executor `magr_schedule_parallel`, which was misleading,
 * because with no callback it did exactly what the serial one did.
 */
int magr_ir_nodes_conflict(const magr_ir *ir, uint32_t node_a, uint32_t node_b);

/* Nodes that declare MAGR_ARRAY_F_MAY_ALIAS are never reordered relative to
 * anything else touching the same arena. Reports whether any node in the
 * program does, so a caller can refuse to parallelise rather than guess. */
int magr_ir_has_aliasing_nodes(const magr_ir *ir);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_IR_H */
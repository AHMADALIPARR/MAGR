/*
 * magr/abi/array_abi.h
 *
 * MAGR Array ABI, version 1.0.0
 *
 * This header is the language-neutral contract between:
 *
 *     Chapel  (scheduler / orchestrator)
 *     Klong   (array evaluation semantics)
 *     Uiua    (tacit program composition, lowered to Array IR)
 *     Fortran 77 numerical functors (numerical backbone)
 *
 * Design rules that this header exists to enforce:
 *
 *   1. Nothing here assumes a specific compiler's object format. Fortran is
 *      reached exclusively through `extern "C"` declarations in
 *      magr/functors/fortran_f77.h that state the calling convention
 *      explicitly. See docs/abi.md.
 *
 *   2. Every cross-boundary struct is self-describing: it carries its own
 *      `struct_size` and `abi_version`. A callee that receives a struct of an
 *      unexpected size or version rejects it instead of misreading fields.
 *      Padding is therefore additive-only at the *tail*.
 *
 *   3. Memory ownership is explicit in the type system. There is no implicit
 *      "the callee frees it" rule.
 *
 *   4. Errors are returned, not thrown, and never reported via `errno`.
 *      Numerical routines that cannot return a status (e.g. DDOT) report
 *      failure through MAGR wrappers that pre-validate arguments.
 *
 *   5. Numerical semantics are declared, not incidental. When a caller needs
 *      bit-identical results it must ask for MAGR_NUM_REORDER_NONE, which
 *      disables any FMA contraction in the wrappers.
 */

#ifndef MAGR_ARRAY_ABI_H
#define MAGR_ARRAY_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI versioning                                                      */
/* ------------------------------------------------------------------ */

#define MAGR_ABI_VERSION_MAJOR 1
#define MAGR_ABI_VERSION_MINOR 0
#define MAGR_ABI_VERSION_PATCH 0
#define MAGR_ABI_VERSION_STRING "1.0.0"

/* Packed as (major << 16) | (minor << 8) | patch for cheap integer compare. */
#define MAGR_ABI_VERSION \
    ((MAGR_ABI_VERSION_MAJOR << 16) | (MAGR_ABI_VERSION_MINOR << 8) | MAGR_ABI_VERSION_PATCH)

/* Maximum rank addressable by the v1 ABI. Chosen so that the array descriptor
 * stays a reasonable size for by-value passing in every binding language. */
#define MAGR_MAX_RANK 8

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum magr_status {
    MAGR_STATUS_OK = 0,
    MAGR_STATUS_INVALID_ARGUMENT = 1,   /* null pointer, bad enum, NaN where forbidden */
    MAGR_STATUS_ABI_MISMATCH = 2,       /* struct_size / abi_version not accepted */
    MAGR_STATUS_RANK_UNSUPPORTED = 3,   /* rank > MAGR_MAX_RANK */
    MAGR_STATUS_SHAPE_MISMATCH = 4,     /* dimension contract violated */
    MAGR_STATUS_STRIDE_UNSUPPORTED = 5, /* stride not representable / not legal for op */
    MAGR_STATUS_BOUNDS = 6,             /* descriptor references memory it does not own */
    MAGR_STATUS_SINGULAR = 7,           /* triangular solve found a zero pivot */
    MAGR_STATUS_DIVISION_BY_ZERO = 8,   /* DNRM2-style rescaling cannot proceed */
    MAGR_STATUS_UNSUPPORTED = 9,        /* operation deliberately refused, not a bug */
    MAGR_STATUS_FUNACTOR_UNAVAILABLE = 10, /* descriptor known but binary not loaded */
    MAGR_STATUS_MANIFEST_REJECTED = 11, /* descriptor failed validation */
    MAGR_STATUS_INTERNAL = 12
} magr_status;

/* Human-readable, stable, never NULL. Suitable for logs and for embedding in a
 * canonical execution manifest. */
const char *magr_status_string(magr_status status);

/* ------------------------------------------------------------------ */
/* Element types and numeric semantics                                 */
/* ------------------------------------------------------------------ */

typedef enum magr_dtype {
    MAGR_DTYPE_INVALID = 0,
    MAGR_DTYPE_F64 = 1, /* IEEE-754 binary64  -- v1 numerical backbone */
    MAGR_DTYPE_F32 = 2, /* IEEE-754 binary32 */
    MAGR_DTYPE_I64 = 3,
    MAGR_DTYPE_I32 = 4,
    MAGR_DTYPE_U8 = 5,
    MAGR_DTYPE_BOOL = 6
} magr_dtype;

/* Size in bytes of one element, or 0 for MAGR_DTYPE_INVALID. */
size_t magr_dtype_size(magr_dtype dtype);

/* True for the IEEE-754 floating-point types. */
int magr_dtype_is_float(magr_dtype dtype);

const char *magr_dtype_name(magr_dtype dtype);

/* Floating-point evaluation contract for a call. This is part of the ABI
 * because a caller that needs reproducibility must be able to *ask* for it and
 * because the value must be committed to the execution manifest. */
typedef enum magr_num_contract {
    /* No constraint. Wrapper may contract multiply-add into FMA. Fastest. */
    MAGR_NUM_REORDER_DEFAULT = 0,
    /* No FMA contraction in MAGR's own wrapper code. Does NOT retroactively
     * change the accumulation order inside the Fortran routine itself. */
    MAGR_NUM_REORDER_NO_FMA = 1,
    /* Caller promises the result is only used with a documented tolerance.
     * Recorded in the manifest so a replay knows what was promised. */
    MAGR_NUM_REORDER_TOLERANT = 2
} magr_num_contract;

/* ------------------------------------------------------------------ */
/* Layout, ownership, array flags                                      */
/* ------------------------------------------------------------------ */

/* Fortran 77 / BLAS convention: element (i,j) of an M x N matrix with leading
 * dimension `ld` lives at element index i + j*ld. This is the ONLY layout the
 * v1 numerical functors accept, and it is the default for a 2-D array whose
 * last stride equals the first dimension times the element size. */
typedef enum magr_layout {
    MAGR_LAYOUT_INVALID = 0,
    MAGR_LAYOUT_COLUMN_MAJOR = 1,
    MAGR_LAYOUT_ROW_MAJOR = 2,
    /* Strides are explicitly given and are not derivable from dims. Row-major
     * views over column-major storage use this. */
    MAGR_LAYOUT_STRIDED = 3
} magr_layout;

typedef enum magr_ownership {
    /* The callee must not free, realloc, or outlive the borrow. */
    MAGR_OWNERSHIP_BORROWED = 0,
    /* The callee owns `data` and must release it with magr_array_free.
     * `release` is the matching deallocator; when NULL, free() is used.
     * `release_ctx` is passed to it verbatim. */
    MAGR_OWNERSHIP_OWNED = 1,
    /* Neither: the buffer is a region carved out of a larger arena. The arena
     * owner is responsible for its lifetime. */
    MAGR_OWNERSHIP_VIEW = 2
} magr_ownership;

enum {
    /* Element values are read-only for the duration of the call. */
    MAGR_ARRAY_F_READONLY = 1u << 0,
    /* Buffer may alias another buffer in the same operation (e.g. in-place
     * AXPY where DY and DX are the same memory). Must be *declared*, never
     * assumed, because BLAS does not promise correctness under aliasing unless
     * the specific routine documents it. */
    MAGR_ARRAY_F_MAY_ALIAS = 1u << 1,
    /* Buffer is zero-length. `data` may be NULL for such arrays. */
    MAGR_ARRAY_F_EMPTY = 1u << 2
};

/* ------------------------------------------------------------------ */
/* The array descriptor                                                */
/* ------------------------------------------------------------------ */

/*
 * A view: shape + strides + element type + a pointer into somebody's memory.
 * `dims` and `strides` describe the *logical* array; `data` points at the
 * element that logical index (0,0,...,0) refers to. `offset` is retained so a
 * sub-view can record where it starts inside its parent arena.
 *
 * Strides are in BYTES, not elements. Byte strides let a view express a f64
 * matrix over an f32 buffer without an element-type lie.
 *
 * Ownership transfer NEVER happens through this struct alone. Passing a struct
 * transfers at most a borrow. A callee that needs ownership takes it with an
 * explicit `magr_array_take_ownership(&arr)` and documents it in its contract.
 */
typedef struct magr_array {
    uint32_t struct_size; /* sizeof(magr_array) as built by the caller */
    uint32_t abi_version; /* MAGR_ABI_VERSION as built by the caller */
    uint32_t dtype;       /* magr_dtype */
    uint32_t layout;      /* magr_layout */
    uint32_t rank;        /* 0 for a scalar-shaped array of 1 element */
    uint32_t flags;       /* MAGR_ARRAY_F_* bitmask */
    void *data;           /* may be NULL iff element_count == 0 */
    int64_t offset;       /* element offset of this view within its arena */
    int64_t dims[MAGR_MAX_RANK];
    int64_t strides[MAGR_MAX_RANK]; /* bytes */
    int64_t nbytes;                /* size of the *logical* extent in bytes */
    int64_t capacity_bytes;        /* bytes reachable from data */
    uint32_t ownership; /* magr_ownership */
    void *release_ctx;  /* opaque, passed to release() */
    void (*release)(void *ctx, void *data);
} magr_array;

/* ------------------------------------------------------------------ */
/* Functor identity                                                    */
/* ------------------------------------------------------------------ */

/*
 * Functor IDs are stable, permanent, and never recycled. A descriptor file,
 * a manifest, or a Merkle node that names a functor ID remains interpretable
 * after the binary is replaced, as long as MAGR_ABI_VERSION is unchanged.
 *
 * Encoding: ASCII prefix "F_" + kind + "_" + element type + "_" + v1, e.g.
 * "F_GEMM_F64_V1". They are exposed as enums for C ergonomics and as strings
 * for cross-language stability; the string form is authoritative.
 */
typedef enum magr_functor_id {
    MAGR_FUNCTOR_INVALID = 0,
    MAGR_FUNCTOR_F_DOT_F64 = 1,
    MAGR_FUNCTOR_F_AXPY_F64 = 2,
    MAGR_FUNCTOR_F_NRM2_F64 = 3,
    MAGR_FUNCTOR_F_GEMV_F64 = 4,
    MAGR_FUNCTOR_F_GEMM_F64 = 5,
    MAGR_FUNCTOR_F_GER_F64 = 6,
    MAGR_FUNCTOR_F_TRSV_F64 = 7,
    MAGR_FUNCTOR_F_TRSM_F64 = 8,
    MAGR_FUNCTOR__COUNT = 9
} magr_functor_id;

/* Stable string name, e.g. "F_GEMM_F64_V1". Returns NULL for out-of-range. */
const char *magr_functor_name(magr_functor_id id);

/* Inverse lookup. Returns MAGR_FUNCTOR_INVALID for unknown names. */
magr_functor_id magr_functor_from_name(const char *name);

/* ------------------------------------------------------------------ */
/* Contract expressed at the call site                                 */
/* ------------------------------------------------------------------ */

/*
 * Per-call numerical + validation contract, passed by pointer.
 *
 * `requires_column_major` is how a caller states "my 2-D buffers really are
 * column-major with these leading dimensions". The wrappers refuse anything
 * else rather than silently copying, because a silent copy would change the
 * performance model and hide a mis-lowered IR node.
 *
 * `tolerance` is advisory metadata for the conformance harness. MAGR never
 * silently relaxes a documented tolerance; it only records what was requested.
 */
typedef struct magr_call_contract {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t num_contract;   /* magr_num_contract */
    uint32_t requires_column_major;
    double rel_tolerance;    /* 0 => caller demands bitwise equality */
    double abs_tolerance;
    const char *provenance;  /* optional, non-owned, for manifests */
} magr_call_contract;

/* Fills `out` with a valid default contract for the running ABI. */
magr_status magr_call_contract_init(magr_call_contract *out);

/* ------------------------------------------------------------------ */
/* Threading and reentrancy                                            */
/* ------------------------------------------------------------------ */

/*
 * v1 guarantees:
 *
 *   - All functors are reentrant and free of global mutable state. Concurrent
 *     calls on disjoint buffers are safe; concurrent calls on overlapping
 *     buffers are the caller's responsibility to prevent or to declare via
 *     MAGR_ARRAY_F_MAY_ALIAS.
 *   - All validation and error reporting is reentrant. magr_status_string()
 *     returns a pointer to a string literal and allocates nothing.
 *   - The one exception to reentrancy is the Fortran XERBLA error handler,
 *     which is upstream global state. MAGR wrappers pre-validate every
 *     argument so XERBLA is never reached on a well-formed call; a call that
 *     reaches XERBLA indicates an internal bug and is reported as
 *     MAGR_STATUS_INTERNAL after the handler returns.
 *   - The scheduler may run functors in parallel, but never reorders two calls
 *     that both declare MAGR_ARRAY_F_MAY_ALIAS on the same underlying arena.
 */

#ifdef __cplusplus
}
#endif

#endif /* MAGR_ARRAY_ABI_H */
/*
 * magr/functors/magr_fortran.h
 *
 * The public, versioned, language-neutral entry points to the Fortran
 * numerical backbone. Every Chapel, Klong, and Uiua caller goes through these
 * eight functions and never touches a raw BLAS symbol.
 *
 * Each entry point:
 *
 *   1. validates descriptors against the Array ABI (dtype, rank, shape,
 *      strides, leading dimension, capacity) using array-core, and
 *   2. refuses rather than coerces: it never materialises a temporary copy to
 *      make an ill-formed call "work", and
 *   3. returns a magr_status. It never throws, never sets errno, and never
 *      calls exit().
 *
 * Why validation lives here and not only in the scheduler: Klong and Uiua can
 * produce strided views in ways the Chapel front end does not, so the functor
 * boundary is the last place at which a bad shape is still cheap to reject.
 *
 * Numerical note: the reference BLAS performs no argument validation of its
 * own beyond XERBLA. Passing LDA < MAX(1,M) does not raise, it reads and
 * writes out of bounds. The checks below are therefore load-bearing for memory
 * safety, not merely good manners.
 */

#ifndef MAGR_FORTRAN_H
#define MAGR_FORTRAN_H

#include "magr/abi/array_abi.h"
#include "magr/array_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Descriptor / provenance introspection                                */
/* ------------------------------------------------------------------ */

/* Version of the wrapper layer itself (independent of MAGR_ABI_VERSION and of
 * the BLAS revision). Recorded in every execution manifest. */
#define MAGR_FORTRAN_WRAPPER_VERSION "1.0.0"

/* Human-readable identification of the numerical backend actually linked in.
 * Filled in at link time from MAGR_BLAS_REVISION / MAGR_F77_COMPILER_ID via
 * the build system; never hard-coded at the call site. */
const char *magr_fortran_backend_id(void);

/* Compiler identity string recorded in the execution manifest, e.g.
 * "GNU Fortran 13.2.0". Never fabricated: the build system fills these in from
 * the compiler itself and the generated config header is committed. */
const char *magr_fortran_backend_compiler(void);

/* Returns the upstream Fortran routine name backing a functor, e.g. "DGEMM".
 * NULL for MAGR_FUNCTOR_INVALID. */
const char *magr_fortran_upstream_routine(magr_functor_id id);

/*
 * Asserts that no XERBLA call has happened since the process started (or since
 * the last magr_xerbla_reset_probe). Returns MAGR_STATUS_INTERNAL and the
 * offending routine name if the counter is non-zero.
 *
 * This exists because the failure mode of a wrong hidden-character-length or
 * wrong-integer-kind call is not a crash. It is a BLAS that quietly accepts an
 * argument, returns, and writes a plausible wrong answer. The conformance suite
 * runs this probe around every call so that class of bug cannot hide.
 */
magr_status magr_xerbla_probe(const char **offending_routine);

void magr_xerbla_reset_probe(void);

/* ------------------------------------------------------------------ */
/* F_DOT_F64 -- DDOT                                                  */
/* ------------------------------------------------------------------ */

/*
 * result = sum_i x[i] * y[i], with strides incx and incy.
 *
 * Preconditions:  x, y are f64 vectors with positive increments; capacity
 *                 backs the full strided extent.
 * Postconditions: returns MAGR_STATUS_OK and stores the dot product in `out`.
 *                 Zero-length vectors return 0.0, matching upstream DDOT.
 * `out` may be NULL only when `out_is_null_ok` semantics are wanted; it is not,
 * so a NULL `out` is MAGR_STATUS_INVALID_ARGUMENT.
 */
magr_status magr_f_dot_f64(const magr_array *x,
                           const magr_array *y,
                           double *out,
                           const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_NRM2_F64 -- DNRM2                                                 */
/* ------------------------------------------------------------------ */

/*
 * out = ||x||_2, computed with upstream's scaling algorithm so that it is
 * overflow- and underflow-resistant.
 *
 * Zero-length vector returns 0.0.
 */
magr_status magr_f_nrm2_f64(const magr_array *x,
                            double *out,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_AXPY_F64 -- DAXPY                                                 */
/* ------------------------------------------------------------------ */

/* y <- alpha*x + y, with strides. */
magr_status magr_f_axpy_f64(const magr_array *x,
                            double alpha,
                            const magr_array *y,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_GEMV_F64 -- DGEMV                                                 */
/* ------------------------------------------------------------------ */

/*
 * y <- alpha*op(A)*x + beta*y, with op(A) = A if trans == MAGR_TRANS_N and A^T
 * otherwise.
 *
 * `a` must be a column-major f64 matrix. Its logical shape depends on trans:
 *   trans == N: a.dims == {m, n}
 *   trans == T: a.dims == {n, m}
 * `x` has `m` elements under N, `n` under T; `y` has the other one.
 *
 * Quick return, per upstream: if m == 0 or (alpha == 0 and beta == 1), the
 * routine returns without touching y. MAGR preserves that and documents it,
 * because a scheduler that assumed y was always written would be wrong.
 */
magr_status magr_f_gemv_f64(magr_trans trans,
                            const magr_array *a,
                            const magr_array *x,
                            double alpha,
                            const magr_array *y,
                            double beta,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_GEMM_F64 -- DGEMM                                                 */
/* ------------------------------------------------------------------ */

/*
 * C <- alpha*op(A)*op(B) + beta*C, all column-major.
 *
 *   op(A): transa == N -> (m,k), transa == T -> (k,m)
 *   op(B): transb == N -> (k,n), transb == T -> (n,k)
 *   C is always (m,n) and is both input and output.
 *
 * C is an INOUT buffer. If `c` aliases `a` or `b`, that must be declared via
 * MAGR_ARRAY_F_MAY_ALIAS on the aliasing descriptor; reference BLAS does not
 * promise correctness otherwise and MAGR will not pretend it does.
 */
magr_status magr_f_gemm_f64(magr_trans transa,
                            magr_trans transb,
                            const magr_array *a,
                            const magr_array *b,
                            double alpha,
                            const magr_array *c,
                            double beta,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_GER_F64 -- DGER                                                   */
/* ------------------------------------------------------------------ */

/* A <- alpha*x*y^T + A. Rank-one update of a column-major (m,n) matrix. */
magr_status magr_f_ger_f64(const magr_array *x,
                           const magr_array *y,
                           double alpha,
                           const magr_array *a,
                           const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_TRSV_F64 -- DTRSV                                                */
/* ------------------------------------------------------------------ */

/*
 * Solves op(A)*x = b in place: x <- op(A)^-1 * x.
 *
 * `a` is (n,n) column-major and is NOT modified.
 * `x` is an n-element f64 vector and IS overwritten with the solution.
 *
 * A structurally singular triangular matrix is undefined behaviour in upstream
 * DTRSV, which divides by the pivot without checking. MAGR detects the
 * unconditional all-zero diagonal case up front and returns
 * MAGR_STATUS_SINGULAR. It cannot detect a general singularity cheaply, so
 * callers who need that must request a factorisation instead. This limit is
 * documented rather than papered over.
 */
magr_status magr_f_trsv_f64(magr_uplo uplo,
                            magr_trans trans,
                            magr_diag diag,
                            const magr_array *a,
                            const magr_array *x,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* F_TRSM_F64 -- DTRSM                                                */
/* ------------------------------------------------------------------ */

/*
 * B <- alpha * op(A)^-1 * B   (side == LEFT)
 * B <- alpha * B * op(A)^-1   (side == RIGHT)
 *
 * B is (m,n) column-major when side == LEFT and (n,m) when side == RIGHT; it is
 * both input and output. A is (k,k) with k = m for LEFT and k = n for RIGHT.
 *
 * Singular-A caveat identical to DTRSV above.
 */
magr_status magr_f_trsm_f64(magr_side side,
                            magr_uplo uplo,
                            magr_trans transa,
                            magr_diag diag,
                            const magr_array *a,
                            double alpha,
                            const magr_array *b,
                            const magr_call_contract *contract);

/* ------------------------------------------------------------------ */
/* Generic dispatch                                                    */
/* ------------------------------------------------------------------ */

/*
 * A generic, descriptor-driven dispatch used by the Array IR executor so that
 * the scheduler can execute an IR node without a switch statement per call
 * site. Argument packing is documented per functor in
 * fortran-functors/descriptors/*.json and mirrored in docs/abi.md:
 *
 *   scalars buffer (doubles first, then int64 flags), then array descriptors
 *   in Fortran argument order.
 *
 * `contract` may be NULL, in which case a default is used.
 */
typedef struct magr_functor_args {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t num_scalars;         /* doubles first, then int64 flags */
    uint32_t num_arrays;
    const double *scalars;        /* may be NULL */
    const int64_t *flags;         /* may be NULL */
    const magr_array *arrays;     /* array descriptor array, or NULL */
    double *out_scalars;          /* storage for scalar results, or NULL */
} magr_functor_args;

magr_status magr_functor_invoke(magr_functor_id id,
                                const magr_functor_args *args,
                                magr_status *status_out);

/*
 * Machine-readable descriptor lookup. Returns NULL if the id is unknown or the
 * descriptor file was not compiled into this binary. The returned JSON is a
 * NUL-terminated string literal owned by the library.
 */
const char *magr_functor_descriptor_json(magr_functor_id id);

/* Non-zero if the descriptor for `id` is present and its self-declared ABI
 * version is compatible with this build. */
int magr_functor_available(magr_functor_id id);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_FORTRAN_H */
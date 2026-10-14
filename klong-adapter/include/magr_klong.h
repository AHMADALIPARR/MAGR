/*
 * magr_klong.h -- Klong integration.
 *
 * Klong supplies array-oriented evaluation semantics: terse right-to-left
 * composition over vectors and matrices, with no scalar/array distinction in
 * most operators. Its strength here is that a whole numerical expression is one
 * token; its risk is that several of its idioms have NO BLAS equivalent and
 * that quietly lowering them to something BLAS-shaped would change the
 * program's meaning.
 *
 * So the policy is: lower the documented subset, and refuse everything else
 * with MAGR_STATUS_UNSUPPORTED. Refusal is the feature. "Test that unsupported
 * operations fail explicitly" is in the spec precisely because a runtime that
 * guesses is a runtime that is wrong without saying so.
 *
 * SUPPORTED SUBSET
 *
 *   Klong form          |  Array IR             |  Fortran
 *   --------------------+-----------------------+------------------
 *   +/x*y               |  fortran.dot(x, y)    |  DDOT
 *   +/y*x               |  fortran.dot(y, x)    |  DDOT   (commutative)
 *   a*x+y               |  fortran.axpy(a,x,y)  |  DAXPY
 *   x*a+y               |  fortran.axpy(a,x,y)  |  DAXPY   (same, reordered)
 *   .nrm2 x             |  fortran.nrm2(x)      |  DNRM2
 *   x . nrm2           |  fortran.nrm2(x)      |  DNRM2
 *   +/x                 |  reduce.sum(x)        |  MAGR kernel
 *   x@|y                |  fortran.gemm(...)    |  DGEMM   (column-major only)
 *   +/x:|y              |  fortran.gemv(...)    |  DGEMV   (column-major only)
 *   \x                   |  elementwise.neg(x)   |  MAGR kernel
 *   !x  (of length 1)   |  view.transpose(x)   |  metadata only
 *
 * DELIBERATELY NOT LOWERED, and why
 *
 *   x y      concatenation      no BLAS analogue; would need a real concat
 *                               kernel, not a fake dot
 *   #x       length             metadata, but not an IR node kind in v1
 *   x@y      general matmul     Klong allows any layout; we accept only
 *                               column-major and refuse row-major rather than
 *                               inserting a silent transposing copy
 *   ,x       enlist / over      control flow; the IR has no loop node, so an
 *                               overlarge expression must be unrolled by the
 *                               caller
 *   x[0;3]   k-style slicing    needs a real slice node with step and offset;
 *                               view.slice is declared in the IR but its
 *                               lowering is not implemented yet
 *
 * The last four are listed so the gap is visible rather than discovered later
 * as "it just returned UNSUPPORTED".
 *
 * NOTE ON EVIDENCE
 *
 * Nothing in this adapter has been compiled or executed. See STATUS.md.
 */

#ifndef MAGR_KLONG_H
#define MAGR_KLONG_H

#include "magr/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds +/x*y over two bound f64 vectors. */
magr_status magr_klong_lower_dot(const magr_ir_value *x, const magr_ir_value *y, magr_ir *ir);

/* Builds a*x+y. `y` must be a writable f64 vector. */
magr_status magr_klong_lower_axpy(const magr_ir_value *x, const magr_ir_value *y, double alpha,
                                  magr_ir *ir);

/* Builds .nrm2 x. */
magr_status magr_klong_lower_nrm2(const magr_ir_value *x, magr_ir *ir);

/* Builds x@y for column-major f64 matrices. Refuses row-major with
 * MAGR_STATUS_STRIDE_UNSUPPORTED rather than copying. */
magr_status magr_klong_lower_matmul(const magr_ir_value *a, const magr_ir_value *b, magr_ir *ir);

/*
 * Recognises one of the supported surface forms and reports which IR kind it
 * would lower to, WITHOUT building a program. This is what lets a front end
 * give a precise diagnostic ("you wrote `x y`, which is concatenation; MAGR
 * does not implement it") instead of a bare UNSUPPORTED.
 *
 * `text` is the whitespace-stripped Klong fragment. Returns MAGR_STATUS_OK and
 * sets *kind_out, or MAGR_STATUS_UNSUPPORTED.
 */
magr_status magr_klong_classify(const char *text, magr_op_kind *kind_out);

/* The same, but returns a sentence a human can act on. Never NULL. */
const char *magr_klong_unsupported_reason(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_KLONG_H */
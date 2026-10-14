/*
 * magr/functors/fortran_f77.h
 *
 * The ONLY place in MAGR that knows how to call Fortran.
 *
 * Why this file exists
 * --------------------
 * Fortran 77 has no C ABI. A C translation unit that declares `void dgemv_()`
 * and calls it is relying on four undocumented conventions at once:
 *
 *   1. Symbol naming.  gfortran, ifort, and flang all emit lowercase names
 *      with exactly one trailing underscore by default (fno-underscoring off).
 *      Some vendors emit none, some emit two.
 *
 *   2. Argument passing.  Fortran passes scalars and array base addresses by
 *      reference in every ABI MAGR targets. gfortran does not require
 *      `bind(C)` or `iso_c_binding` for this to be true, but it is not
 *      guaranteed by the standard.
 *
 *   3. Hidden CHARACTER*1 length arguments. Fortran 77 passes the declared
 *      length of every CHARACTER dummy argument as a trailing scalar. For
 *      CHARACTER*1 that is a 1. gfortran uses `size_t` (or `C_SIZE_T`) for
 *      these hidden lengths; some compilers use `int`.
 *
 *   4. Integer kind.  BLAS' `INTEGER` is gfortran's default integer, i.e.
 *      32-bit two's complement on every platform MAGR supports. Passing a
 *      C `long` would silently corrupt the leading dimension.
 *
 * Each of those is stated explicitly below rather than left to chance. If you
 * port MAGR to a toolchain whose conventions differ, change THIS FILE and the
 * build's generated `magr_fortran_config.h`, and nothing else.
 *
 * Historical length subtlety
 * --------------------------
 * The reference BLAS declares TRANS et al. as CHARACTER*1, so the hidden
 * length is 1. This is confirmed in the vendored sources: `dgemv.f` line 1 is
 *     SUBROUTINE DGEMV(TRANS,M,N,ALPHA,A,LDA,X,INCX,BETA,Y,INCY)
 * with `CHARACTER TRANS` and the post-1995 f2c-era annotation
 * `*> TRANS is CHARACTER*1`. The wrappers below therefore pass a trailing 1.
 *
 * Do not "simplify" these declarations by calling dgemv_ with fewer arguments.
 * Omitting a hidden character length is undefined behaviour that usually
 * manifests as a garbage transpose flag and a silently wrong result, which is
 * the single worst failure mode in this system.
 */

#ifndef MAGR_FORTRAN_F77_H
#define MAGR_FORTRAN_F77_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Toolchain assumptions, overridable at build time                    */
/* ------------------------------------------------------------------ */

#ifndef MAGR_F77_SUFFIX
#define MAGR_F77_SUFFIX "_" /* gfortran / ifort / flang default */
#endif

#ifndef MAGR_F77_CDECL
#define MAGR_F77_CDECL /* all supported toolchains use cdecl on x86-64/AArch64 */
#endif

/* Hidden CHARACTER length argument type. gfortran >= 9 uses size_t. Older
 * gfortran and some vendor compilers use int. */
#ifndef MAGR_F77_CHARLEN_T
#define MAGR_F77_CHARLEN_T size_t
#endif

/* Hidden CHARACTER*1 length. See the header comment. */
#define MAGR_F77_CHAR1_LEN ((MAGR_F77_CHARLEN_T)1)

/* Fortran INTEGER kind used by reference BLAS. */
#ifndef MAGR_F77_INT_T
#define MAGR_F77_INT_T int32_t
#endif

#define MAGR_F77_PASTE_(a, b) a##b
#define MAGR_F77_PASTE(a, b) MAGR_F77_PASTE_(a, b)
#define MAGR_F77_SYM(name) MAGR_F77_PASTE(name, MAGR_F77_SUFFIX)

/* ------------------------------------------------------------------ */
/* Raw reference-BLAS entry points                                     */
/* ------------------------------------------------------------------ */

/*
 * Upstream Fortran signatures, verbatim, with pointer parameters and hidden
 * character lengths appended. Order is never rearranged.
 *
 *   DDOT(N,DX,INCX,DY,INCY) -> DOUBLE PRECISION
 *   DNRM2(N,X,INCX)          -> DOUBLE PRECISION
 *   DAXPY(N,DA,DX,INCX,DY,INCY)
 *   DGEMV(TRANS,M,N,ALPHA,A,LDA,X,INCX,BETA,Y,INCY)
 *   DGEMM(TRANSA,TRANSB,M,N,K,ALPHA,A,LDA,B,LDB,BETA,C,LDC)
 *   DGER(M,N,ALPHA,X,INCX,Y,INCY,A,LDA)
 *   DTRSV(UPLO,TRANS,DIAG,N,A,LDA,X,INCX)
 *   DTRSM(SIDE,UPLO,TRANSA,DIAG,M,N,ALPHA,A,LDA,B,LDB)
 *
 * DNRM2 is a Fortran 90 `real(wp)` function in the vendored source
 * (dnrm2.f90, `wp = kind(1.d0)`), but it returns in a double register exactly
 * like the F77 functions and takes no hidden character argument.
 */

MAGR_F77_CDECL double MAGR_F77_SYM(ddot)(const MAGR_F77_INT_T *n,
                                         const double *dx,
                                         const MAGR_F77_INT_T *incx,
                                         const double *dy,
                                         const MAGR_F77_INT_T *incy);

MAGR_F77_CDECL double MAGR_F77_SYM(dnrm2)(const MAGR_F77_INT_T *n,
                                          const double *x,
                                          const MAGR_F77_INT_T *incx);

MAGR_F77_CDECL void MAGR_F77_SYM(daxpy)(const MAGR_F77_INT_T *n,
                                         const double *da,
                                         const double *dx,
                                         const MAGR_F77_INT_T *incx,
                                         double *dy,
                                         const MAGR_F77_INT_T *incy);

MAGR_F77_CDECL void MAGR_F77_SYM(dgemv)(const char *trans,
                                         const MAGR_F77_INT_T *m,
                                         const MAGR_F77_INT_T *n,
                                         const double *alpha,
                                         const double *a,
                                         const MAGR_F77_INT_T *lda,
                                         const double *x,
                                         const MAGR_F77_INT_T *incx,
                                         const double *beta,
                                         double *y,
                                         const MAGR_F77_INT_T *incy,
                                         MAGR_F77_CHARLEN_T trans_len);

MAGR_F77_CDECL void MAGR_F77_SYM(dgemm)(const char *transa,
                                         const char *transb,
                                         const MAGR_F77_INT_T *m,
                                         const MAGR_F77_INT_T *n,
                                         const MAGR_F77_INT_T *k,
                                         const double *alpha,
                                         const double *a,
                                         const MAGR_F77_INT_T *lda,
                                         const double *b,
                                         const MAGR_F77_INT_T *ldb,
                                         const double *beta,
                                         double *c,
                                         const MAGR_F77_INT_T *ldc,
                                         MAGR_F77_CHARLEN_T transa_len,
                                         MAGR_F77_CHARLEN_T transb_len);

MAGR_F77_CDECL void MAGR_F77_SYM(dger)(const MAGR_F77_INT_T *m,
                                        const MAGR_F77_INT_T *n,
                                        const double *alpha,
                                        const double *x,
                                        const MAGR_F77_INT_T *incx,
                                        const double *y,
                                        const MAGR_F77_INT_T *incy,
                                        double *a,
                                        const MAGR_F77_INT_T *lda);

MAGR_F77_CDECL void MAGR_F77_SYM(dtrsv)(const char *uplo,
                                         const char *trans,
                                         const char *diag,
                                         const MAGR_F77_INT_T *n,
                                         const double *a,
                                         const MAGR_F77_INT_T *lda,
                                         double *x,
                                         const MAGR_F77_INT_T *incx,
                                         MAGR_F77_CHARLEN_T uplo_len,
                                         MAGR_F77_CHARLEN_T trans_len,
                                         MAGR_F77_CHARLEN_T diag_len);

MAGR_F77_CDECL void MAGR_F77_SYM(dtrsm)(const char *side,
                                         const char *uplo,
                                         const char *transa,
                                         const char *diag,
                                         const MAGR_F77_INT_T *m,
                                         const MAGR_F77_INT_T *n,
                                         const double *alpha,
                                         const double *a,
                                         const MAGR_F77_INT_T *lda,
                                         double *b,
                                         const MAGR_F77_INT_T *ldb,
                                         MAGR_F77_CHARLEN_T side_len,
                                         MAGR_F77_CHARLEN_T uplo_len,
                                         MAGR_F77_CHARLEN_T transa_len,
                                         MAGR_F77_CHARLEN_T diag_len);

/* ------------------------------------------------------------------ */
/* XERBLA: the upstream global error handler                           */
/* ------------------------------------------------------------------ */

/*
 * Reference BLAS signals argument errors by calling XERBLA with a string name
 * and an error integer, then returning. XERBLA has a SAVEd counter
 * (`SAVE / integer infof / infof = 0`), i.e. process-global mutable state and
 * therefore NOT reentrant.
 *
 * MAGR never lets a well-formed call reach it -- the wrappers pre-validate
 * every argument -- but a call that does reach it means the wrapper has a bug,
 * and we want loud evidence of that rather than silence. magr_xerbla_probe()
 * exposes the counter so tests can assert that MAGR's own validation path was
 * taken instead.
 */
MAGR_F77_CDECL void MAGR_F77_SYM(xerbla)(const char *srname,
                                          const MAGR_F77_INT_T *info,
                                          MAGR_F77_CHARLEN_T len);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_FORTRAN_F77_H */
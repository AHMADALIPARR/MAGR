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
 * magr_blas.c -- flat C pass-through to the vendored reference BLAS.
 *
 * Two jobs, both mechanical and both easy to get wrong, which is why they are
 * concentrated in one file:
 *
 *   1. Fortran argument order, which is never rearranged.
 *   2. The trailing hidden CHARACTER*1 length on every character dummy.
 *
 * See the header for why (2) is a correctness issue and not a formality.
 */

#include "magr_blas.h"

#include "magr/functors/fortran_f77.h"

#include <string.h>

#ifndef MAGR_BLAS_REVISION
/* Derived by the build from the vendored archive. If the build did not set it,
 * saying so beats claiming a version we did not establish. */
#define MAGR_BLAS_REVISION "unknown"
#endif

const char *magr_blas_backend_id(void) { return "netlib-blas/" MAGR_BLAS_REVISION; }

const char *magr_blas_upstream_name(const char *functor_id) {
    if (functor_id == NULL) return NULL;
    if (strcmp(functor_id, F_DOT_F64) == 0) return "DDOT";
    if (strcmp(functor_id, F_AXPY_F64) == 0) return "DAXPY";
    if (strcmp(functor_id, F_NRM2_F64) == 0) return "DNRM2";
    if (strcmp(functor_id, F_GEMV_F64) == 0) return "DGEMV";
    if (strcmp(functor_id, F_GEMM_F64) == 0) return "DGEMM";
    if (strcmp(functor_id, F_GER_F64) == 0) return "DGER";
    if (strcmp(functor_id, F_TRSV_F64) == 0) return "DTRSV";
    if (strcmp(functor_id, F_TRSM_F64) == 0) return "DTRSM";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* XERBLA tripwire                                                     */
/* ------------------------------------------------------------------ */

/*
 * Reference BLAS reports argument errors by calling XERBLA and returning.
 * XERBLA holds its call count in a Fortran SAVE variable, i.e. process-global
 * mutable state -- the one piece of non-reentrancy in the numerical backend.
 *
 * MAGR pre-validates every argument, so a well-formed call never reaches it. A
 * call that DOES reach it means the wrapper has a bug, and the bug is the kind
 * that returns a plausible wrong answer rather than crashing. So we watch the
 * counter.
 *
 * Under gfortran the SAVE variable is emitted as a `.bss` symbol named
 * `xerbla.infof`, which mangles to `xerbla_infof` with the configured suffix.
 * Reading it is therefore an explicit assumption about one toolchain's emitted
 * symbol names. It is compiled in only when the build defines
 * MAGR_XERBLA_INTERPOSE; otherwise the probe is wired to a private counter that
 * nothing ever increments, and `magr_blas_xerbla_interposed()` reports 0 so a
 * caller can tell the difference instead of being handed a false all-clear.
 *
 * Consequence, stated plainly: when interposition is unavailable this probe is
 * NOT evidence that XERBLA was never called. It is only a tripwire on platforms
 * where the symbol is known. Numerical conformance tests are what actually
 * establish correctness; this narrows the search when one of them fails.
 */
#if defined(MAGR_XERBLA_INTERPOSE)
extern int MAGR_F77_SYM(xerbla)_infof;
static int *const kXerblaCounter = &MAGR_F77_SYM(xerbla)_infof;
int magr_blas_xerbla_interposed(void) { return 1; }
#else
static int magr_xerbla_private_counter = 0;
static int *const kXerblaCounter = &magr_xerbla_private_counter;
int magr_blas_xerbla_interposed(void) { return 0; }
#endif

int magr_blas_xerbla_count(void) { return *kXerblaCounter; }

void magr_blas_xerbla_reset(void) { *kXerblaCounter = 0; }

/* ------------------------------------------------------------------ */
/* Level 1                                                             */
/* ------------------------------------------------------------------ */

double magr_ddot(int n, const double *x, int incx, const double *y, int incy) {
    return MAGR_F77_SYM(ddot)(&n, x, &incx, y, &incy);
}

void magr_daxpy(int n, double alpha, const double *x, int incx, double *y, int incy) {
    MAGR_F77_SYM(daxpy)(&n, &alpha, x, &incx, y, &incy);
}

double magr_dnrm2(int n, const double *x, int incx) {
    return MAGR_F77_SYM(dnrm2)(&n, x, &incx);
}

/* ------------------------------------------------------------------ */
/* Level 2                                                             */
/* ------------------------------------------------------------------ */

/* DGEMV(TRANS,M,N,ALPHA,A,LDA,X,INCX,BETA,Y,INCY) + hidden length for TRANS. */
void magr_dgemv(char trans, int m, int n, double alpha,
                const double *a, int lda,
                const double *x, int incx,
                double beta, double *y, int incy) {
    MAGR_F77_SYM(dgemv)(&trans, &m, &n, &alpha, a, &lda, x, &incx, &beta, y, &incy,
                        MAGR_F77_CHAR1_LEN);
}

/* DGER has no character argument, so no hidden length. */
void magr_dger(int m, int n, double alpha,
               const double *x, int incx,
               const double *y, int incy,
               double *a, int lda) {
    MAGR_F77_SYM(dger)(&m, &n, &alpha, x, &incx, y, &incy, a, &lda);
}

/* DTRSV(UPLO,TRANS,DIAG,N,A,LDA,X,INCX) + three hidden lengths, in
 * declaration order: UPLO, TRANS, DIAG. */
void magr_dtrsv(char uplo, char trans, char diag, int n,
                const double *a, int lda,
                double *x, int incx) {
    MAGR_F77_SYM(dtrsv)(&uplo, &trans, &diag, &n, a, &lda, x, &incx,
                        MAGR_F77_CHAR1_LEN, /* UPLO  */
                        MAGR_F77_CHAR1_LEN, /* TRANS */
                        MAGR_F77_CHAR1_LEN);/* DIAG  */
}

/* ------------------------------------------------------------------ */
/* Level 3                                                             */
/* ------------------------------------------------------------------ */

/* DGEMM(TRANSA,TRANSB,M,N,K,ALPHA,A,LDA,B,LDB,BETA,C,LDC)
 * + hidden lengths for TRANSA then TRANSB, in that order. */
void magr_dgemm(char transa, char transb, int m, int n, int k,
                double alpha,
                const double *a, int lda,
                const double *b, int ldb,
                double beta, double *c, int ldc) {
    MAGR_F77_SYM(dgemm)(&transa, &transb, &m, &n, &k, &alpha, a, &lda, b, &ldb,
                        &beta, c, &ldc,
                        MAGR_F77_CHAR1_LEN, /* TRANSA */
                        MAGR_F77_CHAR1_LEN);/* TRANSB */
}

/* DTRSM(SIDE,UPLO,TRANSA,DIAG,M,N,ALPHA,A,LDA,B,LDB)
 * + hidden lengths for SIDE, UPLO, TRANSA, DIAG, in that order. */
void magr_dtrsm(char side, char uplo, char transa, char diag,
                int m, int n, double alpha,
                const double *a, int lda,
                double *b, int ldb) {
    MAGR_F77_SYM(dtrsm)(&side, &uplo, &transa, &diag, &m, &n, &alpha, a, &lda, b, &ldb,
                        MAGR_F77_CHAR1_LEN, /* SIDE   */
                        MAGR_F77_CHAR1_LEN, /* UPLO   */
                        MAGR_F77_CHAR1_LEN, /* TRANSA */
                        MAGR_F77_CHAR1_LEN);/* DIAG   */
}
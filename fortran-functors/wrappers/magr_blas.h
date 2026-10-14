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
 * magr_blas.h -- the flat C convenience layer over the vendored Fortran.
 *
 * Layering, and why there is a layer at all:
 *
 *   magr_fortran.c     descriptor validation, shape rules, singular checks
 *        |              (the layer that makes an ill-formed call impossible)
 *        v
 *   magr_blas.h/.c     THIS FILE: thin, argument-order-faithful pass-through
 *        |              to Fortran, plus the hidden CHARACTER lengths
 *        v
 *   fortran_f77.h      the only place that declares Fortran symbols
 *        |
 *        v
 *   ddot_ daxpy_ dnrm2_ dgemv_ dgemm_ dger_ dtrsv_ dtrsm_
 *
 * There is exactly one calling path from C into Fortran. An earlier revision of
 * this tree had two, which is how you end up with one file that fixes the
 * hidden-length bug and another that does not.
 *
 * ------------------------------------------------------------------------
 * THE HIDDEN CHARACTER LENGTH ARGUMENT
 * ------------------------------------------------------------------------
 * Fortran 77 passes the declared length of every CHARACTER dummy argument as a
 * trailing hidden scalar. DGEMV's TRANS is CHARACTER*1, so a correct call
 * passes TWELVE arguments, the twelfth being the value 1.
 *
 * Omitting it is undefined behaviour. On x86-64 System V it usually appears to
 * work, because the missing argument would have occupied a register the callee
 * does not read. It is not guaranteed on any ABI and it is guaranteed to break
 * on any toolchain that lays out character lengths differently. The failure is
 * not a crash -- it is a plausible wrong answer.
 *
 * The four affected routines and their hidden lengths:
 *
 *   DGEMV  TRANSA... 1 hidden  (TRANS)
 *   DGEMM  2 hidden   (TRANSA, TRANSB)
 *   DTRSV  3 hidden   (UPLO, TRANS, DIAG)
 *   DTRSM  4 hidden   (SIDE, UPLO, TRANSA, DIAG)
 *
 * ------------------------------------------------------------------------
 * WHAT THIS LAYER DOES NOT DO
 * ------------------------------------------------------------------------
 * It does not validate anything. Passing a bad leading dimension here is an
 * out-of-bounds access inside Fortran, exactly as it would be if you called
 * DGEMM directly. Every caller that is not a test must go through
 * magr_fortran.h instead. The header says so, and the symbols are named
 * `magr_` rather than the Fortran names to keep the two visually distinct.
 */

#ifndef MAGR_BLAS_H
#define MAGR_BLAS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_BLAS_ABI_VERSION "1.0.0"

/*
 * Functor IDs as strings. Authoritative form is the Array ABI's
 * MAGR_FUNCTOR_* enum, whose names carry a _V1 suffix; these are the
 * unversioned short names kept from the Phase 1 patch, retained because the
 * shipped descriptor manifest and the test IDs use them.
 * tools/check_descriptors.py asserts the two naming schemes refer to the same
 * eight routines and have not drifted.
 */
#define F_DOT_F64 "F_DOT_F64"
#define F_AXPY_F64 "F_AXPY_F64"
#define F_NRM2_F64 "F_NRM2_F64"
#define F_GEMV_F64 "F_GEMV_F64"
#define F_GEMM_F64 "F_GEMM_F64"
#define F_GER_F64 "F_GER_F64"
#define F_TRSV_F64 "F_TRSV_F64"
#define F_TRSM_F64 "F_TRSM_F64"

/* --- Level 1: vector/vector --- */

double magr_ddot(int n, const double *x, int incx, const double *y, int incy);
void magr_daxpy(int n, double alpha, const double *x, int incx, double *y, int incy);
double magr_dnrm2(int n, const double *x, int incx);

/* --- Level 2: matrix/vector --- */

void magr_dgemv(char trans, int m, int n, double alpha,
                const double *a, int lda,
                const double *x, int incx,
                double beta, double *y, int incy);

void magr_dger(int m, int n, double alpha,
               const double *x, int incx,
               const double *y, int incy,
               double *a, int lda);

void magr_dtrsv(char uplo, char trans, char diag, int n,
                const double *a, int lda,
                double *x, int incx);

/* --- Level 3: matrix/matrix --- */

void magr_dgemm(char transa, char transb, int m, int n, int k,
                double alpha,
                const double *a, int lda,
                const double *b, int ldb,
                double beta, double *c, int ldc);

void magr_dtrsm(char side, char uplo, char transa, char diag,
                int m, int n, double alpha,
                const double *a, int lda,
                double *b, int ldb);

/*
 * Upstream Fortran routine name behind each functor. Useful for diagnostics
 * and for mapping a failing XERBLA call back to a source line.
 */
const char *magr_blas_upstream_name(const char *functor_id);

/* Identifies the linked numerical backend, e.g.
 * "netlib-blas/3.12.0". Never fabricated: filled in by the build from
 * MAGR_BLAS_REVISION, which the build script derives from the vendored
 * archive rather than from a hard-coded string. */
const char *magr_blas_backend_id(void);

/*
 * XERBLA tripwire. See magr_blas.c for why this exists and what it does and
 * does not prove.
 *
 * magr_blas_xerbla_interposed() returns 1 only when the build could bind to
 * the Fortran SAVE counter. When it returns 0, a clean counter means nothing --
 * callers must not read "no XERBLA calls" as evidence of anything.
 */
int magr_blas_xerbla_interposed(void);
int magr_blas_xerbla_count(void);
void magr_blas_xerbla_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_BLAS_H */
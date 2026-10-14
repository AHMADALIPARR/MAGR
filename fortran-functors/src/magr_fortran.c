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
 * magr_fortran.c -- the validated, ABI-stable bridge to the Fortran backbone.
 *
 * Everything in this file follows one rule: by the time a Fortran symbol is
 * called, every precondition the routine relies on has already been checked,
 * because the routine itself does not check. Reference BLAS reads LDA columns
 * unconditionally; a wrong leading dimension is a silent out-of-bounds write,
 * not an exception.
 */

#include "magr/functors/magr_fortran.h"

#include "magr_blas.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Backend identification                                              */
/* ------------------------------------------------------------------ */

#ifndef MAGR_BLAS_REVISION
#define MAGR_BLAS_REVISION "unknown"
#endif
#ifndef MAGR_F77_COMPILER_ID
#define MAGR_F77_COMPILER_ID "unknown"
#endif
#ifndef MAGR_F77_COMPILER_VERSION
#define MAGR_F77_COMPILER_VERSION "unknown"
#endif

const char *magr_fortran_backend_id(void) {
    return "netlib-blas/" MAGR_BLAS_REVISION
#if !defined(MAGR_F77_CDECL)
           "/non-cdecl"
#endif
           ;
}

const char *magr_fortran_backend_compiler(void) {
    return MAGR_F77_COMPILER_ID " " MAGR_F77_COMPILER_VERSION;
}

static const char *const kUpstreamRoutines[MAGR_FUNCTOR__COUNT] = {
    NULL, "DDOT", "DAXPY", "DNRM2", "DGEMV", "DGEMM", "DGER", "DTRSV", "DTRSM"};

const char *magr_fortran_upstream_routine(magr_functor_id id) {
    if ((int)id <= 0 || (int)id >= MAGR_FUNCTOR__COUNT) return NULL;
    return kUpstreamRoutines[(int)id];
}

/* ------------------------------------------------------------------ */
/* Backend compilation flags                                           */
/* ------------------------------------------------------------------ */

/*
 * Records whether the numerical backend was built with FMA contraction.
 *
 * `-ffp-contract=off` is REQUIRED for bit-identical replay across builds. With
 * contraction on, the compiler may fuse a*x+y into an FMA in DGEMV's inner loop,
 * which changes the rounding and therefore changes the output bits. This is
 * read from the build system, which derives it from the actual flags used, not
 * from a comment that hopes they were right.
 *
 * Consequence, stated plainly: MAGR cannot promise bit-identical results for
 * GEMV/GEMM across arbitrary builds. It CAN promise that a build records whether
 * it was contracted, and that two builds with the same recorded flags and the
 * same pinned BLAS revision will agree. See the numerical_contract block in
 * each descriptor.
 */
int magr_fortran_fp_contract_disabled(void) {
#if defined(MAGR_F77_FP_CONTRACT_OFF)
    return 1;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static magr_status contract_or_default(const magr_call_contract *in, magr_call_contract *tmp) {
    if (in != NULL) {
        if (in->struct_size < offsetof(magr_call_contract, provenance) + sizeof(in->provenance))
            return MAGR_STATUS_ABI_MISMATCH;
        if (((in->abi_version >> 16) & 0xFFFFu) != ((uint32_t)MAGR_ABI_VERSION >> 16))
            return MAGR_STATUS_ABI_MISMATCH;
        return MAGR_STATUS_OK;
    }
    (void)tmp;
    return magr_call_contract_init(tmp);
}

static magr_status need_f64_vector(const magr_array *a, const char *what) {
    magr_status st;
    if (a == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((magr_dtype)a->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;
    if (a->rank != 1) return MAGR_STATUS_RANK_UNSUPPORTED;
    st = magr_array_validate(a, MAGR_ROLE_CONSTRAINT);
    if (st != MAGR_STATUS_OK) return st;
    (void)what;
    return MAGR_STATUS_OK;
}

static magr_status as_int32(int64_t v, MAGR_F77_INT_T *out) {
    if (v < (int64_t)INT32_MIN || v > (int64_t)INT32_MAX) return MAGR_STATUS_SHAPE_MISMATCH;
    *out = (MAGR_F77_INT_T)v;
    return MAGR_STATUS_OK;
}

/* Reads a diagonal element of a column-major (n,n) matrix. */
static double matrix_diag_at(const magr_array *a, int64_t i, int64_t j) {
    int64_t esize = (int64_t)magr_dtype_size((magr_dtype)a->dtype);
    return ((const double *)((const char *)a->data + i * a->strides[0] + j * a->strides[1]))[0];
}

static int matrix_diag_all_zero(const magr_array *a, magr_diag diag, magr_uplo uplo) {
    int64_t i, n;
    if (diag == MAGR_DIAG_UNIT) return 0; /* unit diag is never singular */
    n = a->dims[0];
    for (i = 0; i < n; ++i) {
        double d = matrix_diag_at(a, i, i);
        if (!(d == 0.0)) return 0;
    }
    (void)uplo;
    return 1;
}

/* ------------------------------------------------------------------ */
/* F_DOT_F64                                                           */
/* ------------------------------------------------------------------ */

magr_status magr_f_dot_f64(const magr_array *x,
                           const magr_array *y,
                           double *out,
                           const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T n, incx, incy;
    int64_t incx64 = 0, incy64 = 0;
    int64_t common;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    st = need_f64_vector(y, "y");
    if (st != MAGR_STATUS_OK) return st;

    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(y, &incy64);
    if (st != MAGR_STATUS_OK) return st;

    if (x->dims[0] != y->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    common = x->dims[0];
    if (common == 0) { *out = 0.0; return MAGR_STATUS_OK; }

    if (as_int32(common, &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (as_int32(incy64, &incy) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    *out = magr_ddot((int)n, (const double *)x->data, (int)incx, (const double *)y->data, (int)incy);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_NRM2_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_nrm2_f64(const magr_array *x,
                            double *out,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T n, incx;
    int64_t incx64 = 0;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;

    if (x->dims[0] == 0) { *out = 0.0; return MAGR_STATUS_OK; }

    if (as_int32(x->dims[0], &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    *out = magr_dnrm2((int)n, (const double *)x->data, (int)incx);
    /* DNRM2 signals a rescaling failure by returning NaN in the F77 original;
     * the F90 rewrite propagates it as well. Report it rather than hide it. */
    if (isnan(*out)) return MAGR_STATUS_DIVISION_BY_ZERO;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_AXPY_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_axpy_f64(const magr_array *x,
                            double alpha,
                            const magr_array *y,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T n, incx, incy;
    int64_t incx64 = 0, incy64 = 0;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    st = need_f64_vector(y, "y");
    if (st != MAGR_STATUS_OK) return st;
    /* y is written, so it must be a writable view even though its type in the
     * signature is `const` (const on the descriptor, not on the buffer). */
    if ((y->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(y, &incy64);
    if (st != MAGR_STATUS_OK) return st;

    if (x->dims[0] != y->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (x->dims[0] == 0) return MAGR_STATUS_OK;
    if (isnan(alpha)) return MAGR_STATUS_INVALID_ARGUMENT;

    if (as_int32(x->dims[0], &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (as_int32(incy64, &incy) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    magr_daxpy((int)n, alpha, (const double *)x->data, (int)incx, (double *)y->data, (int)incy);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_GEMV_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_gemv_f64(magr_trans trans,
                            const magr_array *a,
                            const magr_array *x,
                            double alpha,
                            const magr_array *y,
                            double beta,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T m, n, lda, incx, incy;
    int64_t lda64 = 0, incx64 = 0, incy64 = 0;
    int64_t am, an;
    char tc;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (a == NULL || y == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    if ((y->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_array_as_blas_matrix(a, &lda64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(y, &incy64);
    if (st != MAGR_STATUS_OK) return st;

    if (trans == MAGR_TRANS_N) { am = a->dims[0]; an = a->dims[1]; }
    else                      { am = a->dims[1]; an = a->dims[0]; }
    if (x->dims[0] != am) return MAGR_STATUS_SHAPE_MISMATCH;
    if (y->dims[0] != an) return MAGR_STATUS_SHAPE_MISMATCH;

    if (isnan(alpha) || isnan(beta)) return MAGR_STATUS_INVALID_ARGUMENT;

    if (as_int32(am, &m) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(an, &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(lda64, &lda) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (as_int32(incy64, &incy) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    tc = magr_trans_char(trans);
    magr_dgemv(tc, (int)m, (int)n, alpha,
                (const double *)a->data, (int)lda,
                (const double *)x->data, (int)incx,
                beta,
                (double *)y->data, (int)incy);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_GEMM_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_gemm_f64(magr_trans transa,
                            magr_trans transb,
                            const magr_array *a,
                            const magr_array *b,
                            double alpha,
                            const magr_array *c,
                            double beta,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T m, n, k, lda, ldb, ldc;
    int64_t lda64 = 0, ldb64 = 0, ldc64 = 0;
    int64_t am, ak, bk, bn, k64;
    char ta, tb;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (a == NULL || b == NULL || c == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((c->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_array_as_blas_matrix(a, &lda64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_matrix(b, &ldb64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_matrix(c, &ldc64);
    if (st != MAGR_STATUS_OK) return st;

    if (transa == MAGR_TRANS_N) { am = a->dims[0]; ak = a->dims[1]; }
    else                        { am = a->dims[1]; ak = a->dims[0]; }
    if (transb == MAGR_TRANS_N) { bk = b->dims[0]; bn = b->dims[1]; }
    else                        { bk = b->dims[1]; bn = b->dims[0]; }

    if (ak != bk) return MAGR_STATUS_SHAPE_MISMATCH;
    if (c->dims[0] != am || c->dims[1] != bn) return MAGR_STATUS_SHAPE_MISMATCH;
    k64 = ak;

    if (isnan(alpha) || isnan(beta)) return MAGR_STATUS_INVALID_ARGUMENT;

    if (as_int32(am, &m) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(bn, &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(k64, &k) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(lda64, &lda) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(ldb64, &ldb) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(ldc64, &ldc) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;

    ta = magr_trans_char(transa);
    tb = magr_trans_char(transb);
    magr_dgemm(ta, tb, (int)m, (int)n, (int)k, alpha,
                (const double *)a->data, (int)lda,
                (const double *)b->data, (int)ldb,
                beta,
                (double *)c->data, (int)ldc);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_GER_F64                                                           */
/* ------------------------------------------------------------------ */

magr_status magr_f_ger_f64(const magr_array *x,
                           const magr_array *y,
                           double alpha,
                           const magr_array *a,
                           const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T m, n, lda, incx, incy;
    int64_t lda64 = 0, incx64 = 0, incy64 = 0;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (a == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((a->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    st = need_f64_vector(y, "y");
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(y, &incy64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_matrix(a, &lda64);
    if (st != MAGR_STATUS_OK) return st;

    if (x->dims[0] != a->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (y->dims[0] != a->dims[1]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (isnan(alpha)) return MAGR_STATUS_INVALID_ARGUMENT;

    if (as_int32(a->dims[0], &m) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(a->dims[1], &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(lda64, &lda) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (as_int32(incy64, &incy) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    magr_dger((int)m, (int)n, alpha,
                (const double *)x->data, (int)incx,
                (const double *)y->data, (int)incy,
                (double *)a->data, (int)lda);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_TRSV_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_trsv_f64(magr_uplo uplo,
                            magr_trans trans,
                            magr_diag diag,
                            const magr_array *a,
                            const magr_array *x,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T n, lda, incx;
    int64_t lda64 = 0, incx64 = 0;
    char uc, tc, dc;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (a == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((x != NULL) && (x->flags & MAGR_ARRAY_F_READONLY) != 0)
        return MAGR_STATUS_INVALID_ARGUMENT;

    st = need_f64_vector(x, "x");
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_matrix(a, &lda64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_vector(x, &incx64);
    if (st != MAGR_STATUS_OK) return st;

    if (a->dims[0] != a->dims[1]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (x->dims[0] != a->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (x->dims[0] == 0) return MAGR_STATUS_OK;

    if (matrix_diag_all_zero(a, diag, uplo)) return MAGR_STATUS_SINGULAR;

    if (as_int32(a->dims[0], &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(lda64, &lda) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(incx64, &incx) != MAGR_STATUS_OK) return MAGR_STATUS_STRIDE_UNSUPPORTED;

    uc = magr_uplo_char(uplo);
    tc = magr_trans_char(trans);
    dc = magr_diag_char(diag);
    magr_dtrsv(uc, tc, dc, (int)n,
                (const double *)a->data, (int)lda,
                (double *)x->data, (int)incx);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* F_TRSM_F64                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_f_trsm_f64(magr_side side,
                            magr_uplo uplo,
                            magr_trans transa,
                            magr_diag diag,
                            const magr_array *a,
                            double alpha,
                            const magr_array *b,
                            const magr_call_contract *contract) {
    magr_call_contract tmp;
    magr_status st;
    MAGR_F77_INT_T m, n, lda, ldb;
    int64_t lda64 = 0, ldb64 = 0;
    char sc, uc, tc, dc;

    st = contract_or_default(contract, &tmp);
    if (st != MAGR_STATUS_OK) return st;
    if (a == NULL || b == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if ((b->flags & MAGR_ARRAY_F_READONLY) != 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = magr_array_as_blas_matrix(a, &lda64);
    if (st != MAGR_STATUS_OK) return st;
    st = magr_array_as_blas_matrix(b, &ldb64);
    if (st != MAGR_STATUS_OK) return st;

    if (a->dims[0] != a->dims[1]) return MAGR_STATUS_SHAPE_MISMATCH;
    if (side == MAGR_SIDE_LEFT) {
        if (b->dims[0] != a->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    } else {
        if (b->dims[1] != a->dims[0]) return MAGR_STATUS_SHAPE_MISMATCH;
    }
    if (isnan(alpha)) return MAGR_STATUS_INVALID_ARGUMENT;
    if (matrix_diag_all_zero(a, diag, uplo)) return MAGR_STATUS_SINGULAR;

    if (as_int32(b->dims[0], &m) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(b->dims[1], &n) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(lda64, &lda) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;
    if (as_int32(ldb64, &ldb) != MAGR_STATUS_OK) return MAGR_STATUS_SHAPE_MISMATCH;

    sc = magr_side_char(side);
    uc = magr_uplo_char(uplo);
    tc = magr_trans_char(transa);
    dc = magr_diag_char(diag);
    magr_dtrsm(sc, uc, tc, dc, (int)m, (int)n, alpha,
                (const double *)a->data, (int)lda,
                (double *)b->data, (int)ldb);
    return MAGR_STATUS_OK;
}
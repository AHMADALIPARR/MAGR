/*
 * tests/fortran-conformance/test_fortran_conformance.c
 *
 * Numerical conformance for the eight baseline functors.
 *
 * Every expected value here is either exact in binary floating point (values
 * chosen so the true result is representable) or checked against a tolerance
 * justified in the comment. NOTHING is compared against a previous run of
 * MAGR: a test that asserts "the same as last time" cannot detect a change in
 * behaviour, it can only detect a change in the environment. The references are
 * mathematics, not history.
 *
 * Coverage follows the spec's verification requirements: zero-length and
 * singleton vectors, non-unit strides and valid leading dimensions, all
 * transpose modes, degenerate and invalid cases, and quick-return behaviour.
 *
 * STATUS: source complete, not compiled or executed here. No Fortran or C
 * toolchain was available. See STATUS.md.
 */

#include "magr_test.h"

#include "magr/array_core.h"
#include "magr_blas.h"
#include "magr/functors/magr_fortran.h"

#include <math.h>
#include <stdlib.h>

int magr_test_failures = 0;
int magr_test_checks = 0;
const char *magr_test_current = NULL;

/*
 * Tolerance policy.
 *
 * 1e-13 relative for sums of a few terms of O(1) values is comfortably tighter
 * than double-precision epsilon (2.2e-16) accumulated over the term counts used
 * here, and tight enough that a wrong transpose or a wrong leading dimension
 * fails loudly -- those produce O(1) errors, not O(1e-13) ones.
 */
#define REL 1e-13
#define ABS 1e-300

/* Small helpers to build f64 views without repeating eight lines each time. */
static magr_array vec(const double *p, int64_t n, int64_t buf_bytes) {
    magr_array a;
    magr_array_init_vector(&a, MAGR_DTYPE_F64, n, (void *)(uintptr_t)p, buf_bytes,
                           MAGR_OWNERSHIP_BORROWED);
    return a;
}

static magr_array mat(double *p, int64_t rows, int64_t cols, int64_t ld, int64_t buf_bytes) {
    magr_array a;
    magr_array_init_matrix_col_major(&a, MAGR_DTYPE_F64, rows, cols, ld, p, buf_bytes,
                                     MAGR_OWNERSHIP_BORROWED);
    return a;
}

/* ------------------------------------------------------------------ */
/* Level 1                                                             */
/* ------------------------------------------------------------------ */

static int t_dot_basic(void) {
    /* 1*5 + 2*6 + 3*7 + 4*8 = 70, exact in binary64. */
    double x[] = {1, 2, 3, 4};
    double y[] = {5, 6, 7, 8};
    double r = 0.0;
    magr_array xa = vec(x, 4, sizeof(x)), ya = vec(y, 4, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_dot_f64(&xa, &ya, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 70.0, REL, ABS);
    return 0;
}

static int t_dot_strided(void) {
    /* INCX = INCY = 2 over an interleaved buffer picks out elements 0,2,4,6. */
    double x[] = {1, 99, 2, 99, 3, 99, 4, 99};
    double y[] = {5, 99, 6, 99, 7, 99, 8, 99};
    double r = 0.0;
    magr_array xs, ys;
    int64_t dims[1] = {4};
    int64_t strides[1];

    strides[0] = 2 * 8;
    MAGR_CHECK_STATUS(magr_array_init_strided(&xs, MAGR_DTYPE_F64, 1, dims, strides, x,
                                              (int64_t)sizeof(x), MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_init_strided(&ys, MAGR_DTYPE_F64, 1, dims, strides, y,
                                              (int64_t)sizeof(y), MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_dot_f64(&xs, &ys, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 70.0, REL, ABS);
    return 0;
}

static int t_dot_zero_and_singleton(void) {
    double one = 42.0;
    double r = -1.0;
    magr_array empty, single;

    /* Zero length: no buffer, no UB, result 0. This is the case the spec calls
     * out explicitly and the one most likely to be mishandled by a wrapper
     * that dereferences a NULL data pointer. */
    MAGR_CHECK_STATUS(magr_array_init_vector(&empty, MAGR_DTYPE_F64, 0, NULL, 0,
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_dot_f64(&empty, &empty, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 0.0, REL, ABS);

    MAGR_CHECK_STATUS(magr_array_init_vector(&single, MAGR_DTYPE_F64, 1, &one,
                                              (int64_t)sizeof(one), MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_dot_f64(&single, &single, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 42.0 * 42.0, REL, ABS);
    return 0;
}

static int t_dot_length_mismatch_is_rejected(void) {
    double x[4], y[3];
    double r = 0.0;
    magr_array xa = vec(x, 4, sizeof(x)), ya = vec(y, 3, sizeof(y));
    MAGR_CHECK_STATUS(magr_f_dot_f64(&xa, &ya, &r, NULL), MAGR_STATUS_SHAPE_MISMATCH);
    return 0;
}

static int t_nrm2_basic(void) {
    /* sqrt(1 + 4 + 9 + 16) = sqrt(30). */
    double x[] = {1, 2, 3, 4};
    double r = 0.0;
    magr_array xa = vec(x, 4, sizeof(x));
    MAGR_CHECK_STATUS(magr_f_nrm2_f64(&xa, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, sqrt(30.0), REL, ABS);
    return 0;
}

static int t_nrm2_zero_and_overflow_guard(void) {
    double zero[4] = {0, 0, 0, 0};
    double big[4];
    double r = -1.0;
    magr_array za, ba;
    int i;

    za = vec(zero, 4, sizeof(zero));
    MAGR_CHECK_STATUS(magr_f_nrm2_f64(&za, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_BITS_F64(r, 0.0);

    /* Overflow guard: naively summing x^2 overflows at ~1e200, but the true norm
     * is ~1e200 and representable. DNRM2's scaling algorithm must return the
     * finite answer. This is the test that catches an implementation which
     * "simplifies" DNRM2 into a naive sum of squares. */
    for (i = 0; i < 4; ++i) big[i] = 1e200;
    ba = vec(big, 4, sizeof(big));
    MAGR_CHECK_STATUS(magr_f_nrm2_f64(&ba, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 2e200, REL, ABS);
    MAGR_CHECK_MSG(isfinite(r), "norm of 4 x 1e200 must be finite");
    return 0;
}

static int t_nrm2_underflow_guard(void) {
    /* The mirror case: naive summation underflows to zero where the true norm
     * is ~1e-200 and representable. */
    double tiny[4];
    double r = -1.0;
    magr_array ta;
    int i;

    for (i = 0; i < 4; ++i) tiny[i] = 1e-200;
    ta = vec(tiny, 4, sizeof(tiny));
    MAGR_CHECK_STATUS(magr_f_nrm2_f64(&ta, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(r, 2e-200, REL, ABS);
    return 0;
}

static int t_axpy_basic_and_strided(void) {
    double x[4] = {1, 2, 3, 4};
    double y[4] = {10, 20, 30, 40};
    double xs[8] = {1, 0, 2, 0, 3, 0, 4, 0};
    double ys[8];
    int64_t dims[1] = {4}, strides[1] = {16};
    magr_array xa = vec(x, 4, sizeof(x));
    magr_array ya = vec(y, 4, sizeof(y));
    magr_array xsv, ysv;

    MAGR_CHECK_STATUS(magr_f_axpy_f64(&xa, 2.0, &ya, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 12.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 24.0, REL, ABS);
    MAGR_CHECK_NEAR(y[2], 36.0, REL, ABS);
    MAGR_CHECK_NEAR(y[3], 48.0, REL, ABS);
    /* x must be untouched. */
    MAGR_CHECK_NEAR(x[0], 1.0, REL, ABS);

    memcpy(ys, y, sizeof(ys));
    MAGR_CHECK_STATUS(magr_array_init_strided(&xsv, MAGR_DTYPE_F64, 1, dims, strides, xs,
                                              (int64_t)sizeof(xs), MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_init_strided(&ysv, MAGR_DTYPE_F64, 1, dims, strides, ys,
                                              (int64_t)sizeof(ys), MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_axpy_f64(&xsv, 1.0, &ysv, NULL), MAGR_STATUS_OK);
    /* The even slots must have advanced by 2,3,4,5; the odd slots untouched. */
    MAGR_CHECK_NEAR(ys[0], 14.0, REL, ABS);
    MAGR_CHECK_NEAR(ys[2], 25.0, REL, ABS);
    MAGR_CHECK_NEAR(ys[1], 24.0, REL, ABS);
    MAGR_CHECK_NEAR(ys[3], 40.0, REL, ABS);
    return 0;
}

static int t_axpy_inplace_and_alpha_zero(void) {
    double buf[4] = {1, 2, 3, 4};
    double snapshot[4];
    magr_array a = vec(buf, 4, sizeof(buf));

    memcpy(snapshot, buf, sizeof(buf));

    /* In-place AXPY with alpha 1 doubles the vector. DAXPY documents that this
     * is valid, and MAGR declares the aliasing rather than assuming it. */
    a.flags |= MAGR_ARRAY_F_MAY_ALIAS;
    MAGR_CHECK_STATUS(magr_f_axpy_f64(&a, 1.0, &a, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(buf[0], 2.0, REL, ABS);
    MAGR_CHECK_NEAR(buf[3], 8.0, REL, ABS);

    /* alpha = 0 leaves y exactly as it is. */
    memcpy(buf, snapshot, sizeof(buf));
    MAGR_CHECK_STATUS(magr_f_axpy_f64(&a, 0.0, &a, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_BITS_F64(buf[0], 1.0);
    return 0;
}

static int t_axpy_readonly_output_is_refused(void) {
    double x[2] = {1, 2}, y[2] = {3, 4};
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));
    ya.flags |= MAGR_ARRAY_F_READONLY;
    MAGR_CHECK_STATUS(magr_f_axpy_f64(&xa, 1.0, &ya, NULL), MAGR_STATUS_INVALID_ARGUMENT);
    MAGR_CHECK_NEAR(y[0], 3.0, REL, ABS);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Level 2                                                             */
/* ------------------------------------------------------------------ */

static int t_gemv_basic(void) {
    /* A = [[1,3],[2,4]] column-major, i.e. buffer {1,2,3,4}.
     * x = {1,1}, y = {0,0}
     * N: y = A x = {4, 6}  */
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 1};
    double y[2] = {0, 0};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &aa, &xa, 1.0, &ya, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 4.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 6.0, REL, ABS);
    return 0;
}

static int t_gemv_transpose(void) {
    /* The same matrix transposed: y = A^T x = {3, 7}. */
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 1};
    double y[2] = {0, 0};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_T, &aa, &xa, 1.0, &ya, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 3.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 7.0, REL, ABS);
    return 0;
}

static int t_gemv_alpha_beta(void) {
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 1};
    double y[2] = {10, 20};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    /* y = 2*A x + 3*y_old = 2*{4,6} + 3*{10,20} = {38, 72} */
    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &aa, &xa, 2.0, &ya, 3.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 38.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 72.0, REL, ABS);
    return 0;
}

static int t_gemv_quick_return(void) {
    /* beta = 1 and alpha = 0: upstream returns without touching y. A caller
     * that assumed y was always written would be wrong, so this is pinned. */
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 1};
    double y[2] = {77, 88};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &aa, &xa, 0.0, &ya, 1.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_BITS_F64(y[0], 77.0);
    MAGR_CHECK_BITS_F64(y[1], 88.0);
    return 0;
}

static int t_gemv_leading_dimension(void) {
    /* A 2x2 logical matrix inside a 4-row padded buffer. Column 0 must still be
     * taken from rows 0 and 1, not from the whole column. Getting this wrong is
     * the classic LDA bug and it produces plausible numbers, so it is tested
     * with padding that is deliberately poisoned. */
    double a[8] = {1, 2, -999, -999, 3, 4, -999, -999};
    double x[2] = {1, 1};
    double y[2] = {0, 0};
    magr_array aa = mat(a, 2, 2, 4, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &aa, &xa, 1.0, &ya, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 4.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 6.0, REL, ABS);
    return 0;
}

static int t_gemv_row_major_is_refused(void) {
    /* A row-major matrix is a valid array but is NOT BLAS-callable. The wrapper
     * must refuse it rather than silently transposing and copying: a silent copy
     * would hide a mis-lowered front end and change the cost model. */
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 1};
    double y[2] = {0, 0};
    magr_array aa, xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_array_init_matrix_row_major(&aa, MAGR_DTYPE_F64, 2, 2, a,
                                                      (int64_t)sizeof(a),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_validate(&aa, MAGR_ROLE_INPUT), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &aa, &xa, 1.0, &ya, 0.0, NULL),
                      MAGR_STATUS_STRIDE_UNSUPPORTED);
    return 0;
}

static int t_ger_rank_one(void) {
    /* A += alpha * x y^T. With A = 0, alpha = 1, x = {1,2}, y = {3,4,5}
     * the result is [[3,4,5],[6,8,10]] column-major. */
    double a[6] = {0, 0, 0, 0, 0, 0};
    double x[2] = {1, 2};
    double y[3] = {3, 4, 5};
    magr_array aa = mat(a, 2, 3, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 3, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_ger_f64(&xa, &ya, 1.0, &aa, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(a[0], 3.0, REL, ABS);  /* A(0,0) */
    MAGR_CHECK_NEAR(a[1], 6.0, REL, ABS);  /* A(1,0) */
    MAGR_CHECK_NEAR(a[2], 4.0, REL, ABS);  /* A(0,1) */
    MAGR_CHECK_NEAR(a[3], 8.0, REL, ABS);  /* A(1,1) */
    MAGR_CHECK_NEAR(a[4], 5.0, REL, ABS);  /* A(0,2) */
    MAGR_CHECK_NEAR(a[5], 10.0, REL, ABS); /* A(1,2) */
    return 0;
}

static int t_ger_accumulates(void) {
    /* GER is an update, not an assignment: the second call must add again.
     * A = [[1,3],[2,4]] (column-major {1,2,3,4}), x = {1,0}, y = {0,1}.
     * x y^T = [[0,1],[0,0]], so alpha * x y^T = [[0,2],[0,0]] per call.
     * After two calls A = [[1, 3 + 2 + 2],[2, 4]] = [[1,7],[2,4]]. */
    double a[4] = {1, 2, 3, 4};
    double x[2] = {1, 0};
    double y[2] = {0, 1};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x)), ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_ger_f64(&xa, &ya, 2.0, &aa, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_ger_f64(&xa, &ya, 2.0, &aa, NULL), MAGR_STATUS_OK);
    /* Column-major: a[0]=A(0,0), a[1]=A(1,0), a[2]=A(0,1), a[3]=A(1,1). */
    MAGR_CHECK_NEAR(a[0], 1.0, REL, ABS);
    MAGR_CHECK_NEAR(a[1], 2.0, REL, ABS);
    MAGR_CHECK_NEAR(a[2], 7.0, REL, ABS);
    MAGR_CHECK_NEAR(a[3], 4.0, REL, ABS);
    return 0;
}

static int t_trsv_lower_unit(void) {
    /* Lower unit-triangular A = [[1,0],[2,1]], b = {3,5}
     * forward substitution: x0 = 3; x1 = 5 - 2*3 = -1. */
    double a[4] = {1, 2, 0, 1}; /* column-major: col0={1,2}, col1={0,1} */
    double x[2] = {3, 5};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x));

    MAGR_CHECK_STATUS(magr_f_trsv_f64(MAGR_UPLO_LOWER, MAGR_TRANS_N, MAGR_DIAG_UNIT, &aa, &xa,
                                      NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(x[0], 3.0, REL, ABS);
    MAGR_CHECK_NEAR(x[1], -1.0, REL, ABS);
    return 0;
}

static int t_trsv_upper_nonunit(void) {
    /* Upper non-unit A = [[2,1],[0,4]], b = {6,8}
     * back substitution: x1 = 8/4 = 2; x0 = (6-1*2)/2 = 2. */
    double a[4] = {2, 0, 1, 4};
    double x[2] = {6, 8};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x));

    MAGR_CHECK_STATUS(magr_f_trsv_f64(MAGR_UPLO_UPPER, MAGR_TRANS_N, MAGR_DIAG_NONUNIT, &aa, &xa,
                                      NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(x[0], 2.0, REL, ABS);
    MAGR_CHECK_NEAR(x[1], 2.0, REL, ABS);
    return 0;
}

static int t_trsv_zero_diagonal_is_refused(void) {
    /* Upstream DTRSV divides by A(i,i) with no check. MAGR refuses the
     * unconditional zero-diagonal case up front and must leave x untouched. */
    double a[4] = {0, 2, 0, 1};
    double x[2] = {6, 8};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x));

    MAGR_CHECK_STATUS(magr_f_trsv_f64(MAGR_UPLO_LOWER, MAGR_TRANS_N, MAGR_DIAG_NONUNIT, &aa, &xa,
                                      NULL),
                      MAGR_STATUS_SINGULAR);
    MAGR_CHECK_NEAR(x[0], 6.0, REL, ABS); /* not modified */
    MAGR_CHECK_NEAR(x[1], 8.0, REL, ABS);
    return 0;
}

static int t_trsv_unit_diagonal_is_never_singular(void) {
    /* DIAG=UNIT means the stored diagonal is ignored, so a matrix whose stored
     * diagonal is zero is perfectly well-posed and must NOT be refused. */
    double a[4] = {0, 2, 0, 0}; /* stored diagonal is zero; effective diagonal is 1 */
    double x[2] = {6, 8};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array xa = vec(x, 2, sizeof(x));

    /* Lower unit: x0 = 6; x1 = 8 - 2*6 = -4 */
    MAGR_CHECK_STATUS(magr_f_trsv_f64(MAGR_UPLO_LOWER, MAGR_TRANS_N, MAGR_DIAG_UNIT, &aa, &xa,
                                      NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(x[0], 6.0, REL, ABS);
    MAGR_CHECK_NEAR(x[1], -4.0, REL, ABS);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Level 3                                                             */
/* ------------------------------------------------------------------ */

static int t_gemm_basic(void) {
    /* A = [[1,3],[2,4]] (col-major {1,2,3,4}), B = [[5,6],[7,8]]
     * (col-major {5,7,6,8}), C = 0.
     * A*B = [[1*5+3*7, 1*6+3*8], [2*5+4*7, 2*6+4*8]] = [[26,30],[38,44]] */
    double a[4] = {1, 2, 3, 4};
    double b[4] = {5, 7, 6, 8};
    double c[4] = {0, 0, 0, 0};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));
    magr_array ca = mat(c, 2, 2, 2, sizeof(c));

    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_N, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(c[0], 26.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 38.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 30.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 44.0, REL, ABS);
    return 0;
}

static int t_gemm_transposes(void) {
    double a[4] = {1, 2, 3, 4};
    double b[4] = {5, 7, 6, 8};
    double c[4];
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));
    magr_array ca;

    /* A^T = [[1,2],[3,4]]; A^T*B = [[1*5+2*7, 1*6+2*8],[3*5+4*7,3*6+4*8]] = [[19,22],[43,50]] */
    c[0] = c[1] = c[2] = c[3] = 0.0;
    ca = mat(c, 2, 2, 2, sizeof(c));
    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_T, MAGR_TRANS_N, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(c[0], 19.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 43.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 22.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 44.0, REL, ABS);

    /* B^T = [[5,7],[6,8]]; A*B^T = [[1*5+3*6, 1*7+3*8],[2*5+4*6,2*7+4*8]] = [[23,31],[34,46]] */
    c[0] = c[1] = c[2] = c[3] = 0.0;
    ca = mat(c, 2, 2, 2, sizeof(c));
    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_T, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(c[0], 23.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 34.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 31.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 46.0, REL, ABS);
    return 0;
}

static int t_gemm_alpha_beta(void) {
    /* C = 2*A*B + 3*C_old with C_old = 1 gives 2*[[26,30],[38,44]] + 3 = [[55,63],[79,91]] */
    double a[4] = {1, 2, 3, 4};
    double b[4] = {5, 7, 6, 8};
    double c[4] = {1, 1, 1, 1};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));
    magr_array ca = mat(c, 2, 2, 2, sizeof(c));

    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_N, &aa, &ba, 2.0, &ca, 3.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(c[0], 55.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 79.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 63.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 91.0, REL, ABS);
    return 0;
}

static int t_gemm_leading_dimension(void) {
    /* Same product as t_gemm_basic but every matrix sits in a padded buffer.
     * The padding is poisoned, so reading it would corrupt the result loudly. */
    double a[8] = {1, 2, -999, -999, 3, 4, -999, -999};
    double b[8] = {5, 7, -999, -999, 6, 8, -999, -999};
    double c[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    magr_array aa = mat(a, 2, 2, 4, sizeof(a));
    magr_array ba = mat(b, 2, 2, 4, sizeof(b));
    magr_array ca = mat(c, 2, 2, 4, sizeof(c));

    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_N, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_OK);
    /* C is written through LDC = 4, so C(i,j) lands at c[i + j*4]. The result is
     * [[26,30],[38,44]], hence c = {26, 38, <pad>, <pad>, 30, 50, <pad>, <pad>}.
     * Asserting c[2] and c[3] would be asserting on padding -- which is how a
     * wrong-LDA bug can hide. */
    MAGR_CHECK_NEAR(c[0], 26.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 38.0, REL, ABS);
    MAGR_CHECK_NEAR(c[4], 30.0, REL, ABS);
    MAGR_CHECK_NEAR(c[5], 44.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 0.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 0.0, REL, ABS);
    MAGR_CHECK_NEAR(c[6], 0.0, REL, ABS);
    MAGR_CHECK_NEAR(c[7], 0.0, REL, ABS);
    return 0;
}

static int t_gemm_non_square(void) {
    /* 2x3 * 3x2 -> 2x2. A = [[1,2,3],[4,5,6]] col-major, B = [[7,8],[9,10],[11,12]] */
    double a[6] = {1, 4, 2, 5, 3, 6};
    double b[6] = {7, 9, 11, 8, 10, 12};
    double c[4] = {0, 0, 0, 0};
    magr_array aa = mat(a, 2, 3, 2, sizeof(a));
    magr_array ba = mat(b, 3, 2, 3, sizeof(b));
    magr_array ca = mat(c, 2, 2, 2, sizeof(c));

    /* C(0,0) = 1*7+2*9+3*11 = 58; C(1,0) = 4*7+5*9+6*11 = 139
     * C(0,1) = 1*8+2*10+3*12 = 64; C(1,1) = 4*8+5*10+6*12 = 154 */
    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_N, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(c[0], 58.0, REL, ABS);
    MAGR_CHECK_NEAR(c[1], 139.0, REL, ABS);
    MAGR_CHECK_NEAR(c[2], 64.0, REL, ABS);
    MAGR_CHECK_NEAR(c[3], 154.0, REL, ABS);
    return 0;
}

static int t_gemm_shape_mismatch_is_rejected(void) {
    double a[4] = {1, 2, 3, 4};
    double b[6] = {1, 2, 3, 4, 5, 6}; /* 3x2 */
    double c[4] = {0, 0, 0, 0};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 3, 2, 3, sizeof(b));
    magr_array ca = mat(c, 2, 2, 2, sizeof(c));

    /* K disagrees: A is 2x2, B is 3x2. Upstream would read A's columns beyond
     * its two and produce garbage; MAGR must refuse. */
    MAGR_CHECK_STATUS(magr_f_gemm_f64(MAGR_TRANS_N, MAGR_TRANS_N, &aa, &ba, 1.0, &ca, 0.0, NULL),
                      MAGR_STATUS_SHAPE_MISMATCH);
    MAGR_CHECK_NEAR(c[0], 0.0, REL, ABS);
    return 0;
}

static int t_trsm_left(void) {
    /* Lower unit A = [[1,0],[2,1]], B = [[3,4],[5,6]] col-major {3,5,4,6}
     * solve A X = B column by column:
     *   col0: x0 = 3; x1 = 5 - 2*3 = -1
     *   col1: x0 = 4; x1 = 6 - 2*4 = -2
     * B becomes [[3,4],[-1,-2]] col-major {3,-1,4,-2} */
    double a[4] = {1, 2, 0, 1};
    double b[4] = {3, 5, 4, 6};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));

    MAGR_CHECK_STATUS(magr_f_trsm_f64(MAGR_SIDE_LEFT, MAGR_UPLO_LOWER, MAGR_TRANS_N,
                                      MAGR_DIAG_UNIT, &aa, 1.0, &ba, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(b[0], 3.0, REL, ABS);
    MAGR_CHECK_NEAR(b[1], -1.0, REL, ABS);
    MAGR_CHECK_NEAR(b[2], 4.0, REL, ABS);
    MAGR_CHECK_NEAR(b[3], -2.0, REL, ABS);
    return 0;
}

static int t_trsm_right(void) {
    /* Right-side solve X = B * op(A)^-1 with lower unit A = [[1,0],[2,1]] and
     * B = [[3,4],[5,6]] (column-major {3,5,4,6}).
     *
     * A^-1 = [[1,0],[-2,1]]. B*A^-1 = [[3 - 8, 4],[5 - 12, 6]] = [[-5,4],[-7,6]].
     *
     * Solved row-wise to check the arithmetic: row0 [p,q] of X satisfies
     * [p,q]*A = [p + 2q, q] = [3,4] -> q = 4, p = -5.
     * row1 [r,s]: r + 2s = 5, s = 6 -> s = 6, r = -7.
     * X = [[-5,4],[-7,6]], column-major buffer {-5,-7,4,6}.
     *
     * The `-1, -2` figures an earlier draft of this test asserted came from
     * subtracting 2*row0's FIRST element instead of column-solving; they were
     * wrong and the test would have failed. */
    double a[4] = {1, 2, 0, 1};
    double b[4] = {3, 5, 4, 6};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));

    MAGR_CHECK_STATUS(magr_f_trsm_f64(MAGR_SIDE_RIGHT, MAGR_UPLO_LOWER, MAGR_TRANS_N,
                                      MAGR_DIAG_UNIT, &aa, 1.0, &ba, NULL),
                      MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(b[0], -5.0, REL, ABS);
    MAGR_CHECK_NEAR(b[1], -7.0, REL, ABS);
    MAGR_CHECK_NEAR(b[2], 4.0, REL, ABS);
    MAGR_CHECK_NEAR(b[3], 6.0, REL, ABS);
    return 0;
}

static int t_trsm_zero_diagonal_is_refused(void) {
    double a[4] = {0, 2, 0, 0};
    double b[4] = {3, 5, 4, 6};
    magr_array aa = mat(a, 2, 2, 2, sizeof(a));
    magr_array ba = mat(b, 2, 2, 2, sizeof(b));

    MAGR_CHECK_STATUS(magr_f_trsm_f64(MAGR_SIDE_LEFT, MAGR_UPLO_LOWER, MAGR_TRANS_N,
                                      MAGR_DIAG_NONUNIT, &aa, 1.0, &ba, NULL),
                      MAGR_STATUS_SINGULAR);
    MAGR_CHECK_NEAR(b[0], 3.0, REL, ABS); /* unmodified */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cross-cutting                                                       */
/* ------------------------------------------------------------------ */

static int t_xerbla_probe_is_consistent(void) {
    /*
     * Every call above was well-formed, so no XERBLA call should have happened.
     *
     * What this test can honestly claim depends on whether the probe is bound to
     * the real Fortran SAVE counter. If it is not, a clean reading means
     * NOTHING and this test only checks that the API is callable. Saying so
     * matters: a probe that silently degrades to "always zero" looks exactly
     * like a probe that found no problems, and that is the worst possible
     * failure mode for a diagnostic.
     */
    int interposed = magr_blas_xerbla_interposed();
    int count = magr_blas_xerbla_count();

    if (interposed) {
        MAGR_CHECK_MSG(count == 0,
                       "XERBLA fired during conformance tests: a wrapper let an invalid "
                       "argument reach Fortran");
    } else {
        /* Not a failure -- the platform simply does not support the probe. */
        printf("       (XERBLA probe not interposed on this toolchain; reading of %d "
               "carries no information)\n",
               count);
    }
    return 0;
}

static int t_functional_composition(void) {
    /* The point of having the ABI is composition. Compute
     *     r = (alpha * (a . b)) + ||c||
     * through the ABI and check the arithmetic, so the test exercises the path
     * a real program takes rather than eight isolated calls. */
    double a[4] = {1, 0, 0, 1};
    double b[4] = {2, 3, 4, 5};
    double c[4] = {3, 4, 0, 0};
    double r = 0.0, nrm = 0.0;
    magr_array aa = vec(a, 4, sizeof(a)), ba = vec(b, 4, sizeof(b)), ca = vec(c, 4, sizeof(c));

    MAGR_CHECK_STATUS(magr_f_dot_f64(&aa, &ba, &r, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_f_nrm2_f64(&ca, &nrm, NULL), MAGR_STATUS_OK);
    /* a . b = 2 + 5 = 7; ||c|| = 5; 2*7 + 5 = 19 */
    MAGR_CHECK_NEAR(2.0 * r + nrm, 19.0, REL, ABS);
    return 0;
}

static int t_trsv_round_trips_with_gemm(void) {
    /* A = L (lower unit triangular). Solve L X = A via TRSV, then check that
     * L X = A holds by re-multiplying with DGEMV. A real solver's correctness
     * is the round trip, not the intermediate values. */
    double l[4] = {1, 2, 0, 1};
    double x[2] = {3, 5};
    double y[2] = {0, 0};
    magr_array la = mat(l, 2, 2, 2, sizeof(l));
    magr_array xa = vec(x, 2, sizeof(x));
    magr_array ya = vec(y, 2, sizeof(y));

    MAGR_CHECK_STATUS(magr_f_trsv_f64(MAGR_UPLO_LOWER, MAGR_TRANS_N, MAGR_DIAG_UNIT, &la, &xa,
                                      NULL),
                      MAGR_STATUS_OK);
    /* Recover b = A x = {3, 5} from the ORIGINAL matrix, not the solution. */
    MAGR_CHECK_STATUS(magr_f_gemv_f64(MAGR_TRANS_N, &la, &xa, 1.0, &ya, 0.0, NULL), MAGR_STATUS_OK);
    MAGR_CHECK_NEAR(y[0], 3.0, REL, ABS);
    MAGR_CHECK_NEAR(y[1], 5.0, REL, ABS);
    return 0;
}

typedef struct {
    const char *name;
    magr_test_fn fn;
} entry;

int main(void) {
    static const entry tests[] = {
        {"dot_basic", t_dot_basic},
        {"dot_strided", t_dot_strided},
        {"dot_zero_and_singleton", t_dot_zero_and_singleton},
        {"dot_length_mismatch_is_rejected", t_dot_length_mismatch_is_rejected},
        {"nrm2_basic", t_nrm2_basic},
        {"nrm2_zero_and_overflow_guard", t_nrm2_zero_and_overflow_guard},
        {"nrm2_underflow_guard", t_nrm2_underflow_guard},
        {"axpy_basic_and_strided", t_axpy_basic_and_strided},
        {"axpy_inplace_and_alpha_zero", t_axpy_inplace_and_alpha_zero},
        {"axpy_readonly_output_is_refused", t_axpy_readonly_output_is_refused},
        {"gemv_basic", t_gemv_basic},
        {"gemv_transpose", t_gemv_transpose},
        {"gemv_alpha_beta", t_gemv_alpha_beta},
        {"gemv_quick_return", t_gemv_quick_return},
        {"gemv_leading_dimension", t_gemv_leading_dimension},
        {"gemv_row_major_is_refused", t_gemv_row_major_is_refused},
        {"ger_rank_one", t_ger_rank_one},
        {"ger_accumulates", t_ger_accumulates},
        {"trsv_lower_unit", t_trsv_lower_unit},
        {"trsv_upper_nonunit", t_trsv_upper_nonunit},
        {"trsv_zero_diagonal_is_refused", t_trsv_zero_diagonal_is_refused},
        {"trsv_unit_diagonal_is_never_singular", t_trsv_unit_diagonal_is_never_singular},
        {"gemm_basic", t_gemm_basic},
        {"gemm_transposes", t_gemm_transposes},
        {"gemm_alpha_beta", t_gemm_alpha_beta},
        {"gemm_leading_dimension", t_gemm_leading_dimension},
        {"gemm_non_square", t_gemm_non_square},
        {"gemm_shape_mismatch_is_rejected", t_gemm_shape_mismatch_is_rejected},
        {"trsm_left", t_trsm_left},
        {"trsm_right", t_trsm_right},
        {"trsm_zero_diagonal_is_refused", t_trsm_zero_diagonal_is_refused},
        {"functional_composition", t_functional_composition},
        {"trsv_round_trips_with_gemm", t_trsv_round_trips_with_gemm},
        {"xerbla_probe_is_consistent", t_xerbla_probe_is_consistent},
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) magr_test_run(tests[i].name, tests[i].fn);
    printf("\n%d checks, %d failures\n", magr_test_checks, magr_test_failures);
    return magr_test_failures == 0 ? 0 : 1;
}
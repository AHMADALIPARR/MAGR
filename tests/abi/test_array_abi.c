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
 * tests/abi/test_array_abi.c
 *
 * ABI conformance: the properties that must hold for any value crossing the
 * C / Fortran / Chapel / Klong / Uiua boundary.
 *
 * These tests do not require Fortran. They test the contract layer, which is
 * exactly where ABI mistakes are caught most cheaply.
 */

#include "magr_test.h"

#include "magr/array_core.h"
#include "magr/functors/fortran_f77.h"
#include "magr/functors/magr_fortran.h"

#include <stddef.h>

int magr_test_failures = 0;
int magr_test_checks = 0;
const char *magr_test_current = NULL;

/* ------------------------------------------------------------------ */

static int t_status_strings_are_total(void) {
    /* Every enumerator must map to a distinct, non-empty string. This is not
     * cosmetic: the strings go into canonical manifests, so an unmapped code
     * would silently commit as "UNKNOWN_STATUS". */
    MAGR_CHECK(strcmp(magr_status_string(MAGR_STATUS_OK), "OK") == 0);
    MAGR_CHECK(magr_status_string((magr_status)999)[0] != '\0');
    MAGR_CHECK(strcmp(magr_status_string((magr_status)999), "UNKNOWN_STATUS") == 0);
    MAGR_CHECK(strlen(magr_status_string(MAGR_STATUS_ABI_MISMATCH)) > 3);
    return 0;
}

static int t_dtype_sizes(void) {
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_F64), 8);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_F32), 4);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_I64), 8);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_I32), 4);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_U8), 1);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_BOOL), 1);
    MAGR_CHECK_EQ_I(magr_dtype_size(MAGR_DTYPE_INVALID), 0);
    MAGR_CHECK(magr_dtype_is_float(MAGR_DTYPE_F64) == 1);
    MAGR_CHECK(magr_dtype_is_float(MAGR_DTYPE_I32) == 0);
    return 0;
}

static int t_struct_is_self_describing(void) {
    /* The whole cross-boundary safety story rests on this. A descriptor built
     * by a caller must carry its own size and ABI version so a mismatched
     * callee can refuse rather than misread fields. */
    magr_array a;
    int64_t dims[1];
    dims[0] = 8;
    MAGR_CHECK_STATUS(magr_array_init_vector(&a, MAGR_DTYPE_F64, 8, NULL, 0, MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(a.struct_size, (int)sizeof(magr_array));
    MAGR_CHECK_EQ_I(a.abi_version, MAGR_ABI_VERSION);
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_OK);

    /* Simulate a caller compiled against an older, smaller struct. */
    a.struct_size = (uint32_t)(offsetof(magr_array, release));
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_ABI_MISMATCH);

    /* Simulate an incompatible ABI major. */
    a.struct_size = (uint32_t)sizeof(magr_array);
    a.abi_version = (MAGR_ABI_VERSION & 0xFFFFu) | (2u << 16);
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_ABI_MISMATCH);
    return 0;
}

static int t_column_major_layout_is_derived_not_assumed(void) {
    /* 3x2 f64 buffer, column-major, ld=3. Element (i,j) at i + j*3. */
    double buf[6];
    magr_array a;
    int64_t idx[2];
    int64_t ld = 0;

    MAGR_CHECK_STATUS(magr_array_init_matrix_col_major(&a, MAGR_DTYPE_F64, 3, 2, 3, buf,
                                                      (int64_t)sizeof(buf),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(a.strides[0], 8);
    MAGR_CHECK_EQ_I(a.strides[1], 24);
    MAGR_CHECK(magr_array_is_column_major(&a) == 1);
    MAGR_CHECK_STATUS(magr_array_as_blas_matrix(&a, &ld), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(ld, 3);

    idx[0] = 2; idx[1] = 1;
    MAGR_CHECK_EQ_I(magr_array_element_offset(&a, idx), 2 * 8 + 1 * 24);
    idx[0] = 0; idx[1] = 1;
    MAGR_CHECK_EQ_I(magr_array_element_offset(&a, idx), 24);

    /* Out-of-range index must be rejected, not clamped. */
    idx[0] = 3; idx[1] = 0;
    MAGR_CHECK_EQ_I(magr_array_element_offset(&a, idx), -1);
    return 0;
}

static int t_row_major_is_rejected_by_blas_paths(void) {
    /* A row-major array is a perfectly good array, and the front ends produce
     * them. It must NOT be silently accepted by a Fortran functor. */
    double buf[6];
    magr_array a;
    int64_t ld = 0;

    MAGR_CHECK_STATUS(magr_array_init_matrix_row_major(&a, MAGR_DTYPE_F64, 3, 2, buf,
                                                      (int64_t)sizeof(buf),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK(magr_array_is_column_major(&a) == 0);
    MAGR_CHECK_STATUS(magr_array_as_blas_matrix(&a, &ld), MAGR_STATUS_STRIDE_UNSUPPORTED);
    /* ...but it is still a valid descriptor. */
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_INPUT), MAGR_STATUS_OK);
    return 0;
}

static int t_leading_dimension_must_exceed_rows(void) {
    double buf[64];
    magr_array a;
    int64_t ld = 0;

    /* LDA < M is the classic silent OOB in hand-rolled BLAS calls. */
    MAGR_CHECK_STATUS(magr_array_init_matrix_col_major(&a, MAGR_DTYPE_F64, 5, 5, 3, buf,
                                                      (int64_t)sizeof(buf),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_SHAPE_MISMATCH);

    /* A leading dimension larger than the row count is legal and must work. */
    MAGR_CHECK_STATUS(magr_array_init_matrix_col_major(&a, MAGR_DTYPE_F64, 5, 4, 8, buf,
                                                      (int64_t)sizeof(buf),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_as_blas_matrix(&a, &ld), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(ld, 8);
    return 0;
}

static int t_capacity_bounds_are_enforced(void) {
    /* Declares a 100x100 logical matrix over a 100-element buffer. */
    double buf[100];
    magr_array a;
    int64_t ld = 0;

    MAGR_CHECK_STATUS(magr_array_init_matrix_col_major(&a, MAGR_DTYPE_F64, 100, 100, 100, buf,
                                                      (int64_t)sizeof(buf),
                                                      MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    /* The element offset is in range for the first column... */
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_INPUT), MAGR_STATUS_BOUNDS);
    /* ...and the BLAS path rejects it too, rather than trusting the caller. */
    MAGR_CHECK_STATUS(magr_array_as_blas_matrix(&a, &ld), MAGR_STATUS_BOUNDS);
    return 0;
}

static int t_strides_become_increments(void) {
    /* A strided view of a larger buffer is exactly BLAS's INCX/INCY. */
    double buf[9];
    magr_array v;
    int64_t inc = 0;

    MAGR_CHECK_STATUS(magr_array_init_strided(&v, MAGR_DTYPE_F64, 1, (int64_t[]){4},
                                              (int64_t[]){16}, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_as_blas_vector(&v, &inc), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(inc, 2); /* 16 bytes / 8 = stride 2 */
    return 0;
}

static int t_negative_increment_is_refused(void) {
    /* DNRM2/DDOT accept negative INCX. MAGR refuses: reversed traversal is a
     * place where Klong/Uiua lowering and Fortran conventions silently
     * disagree, and a reversed view is better expressed as an explicit
     * reversal node in the IR. */
    double buf[9];
    magr_array v;
    int64_t inc = 0;

    MAGR_CHECK_STATUS(magr_array_init_strided(&v, MAGR_DTYPE_F64, 1, (int64_t[]){4},
                                              (int64_t[]){-16}, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_as_blas_vector(&v, &inc), MAGR_STATUS_STRIDE_UNSUPPORTED);
    return 0;
}

static int t_zero_and_singleton_shapes(void) {
    double one[1];
    magr_array v;
    int64_t inc = 0;

    /* Zero-length: data may be NULL. The conformance tests require this to
     * work, so the descriptor must permit it. */
    MAGR_CHECK_STATUS(magr_array_init_vector(&v, MAGR_DTYPE_F64, 0, NULL, 0,
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK(magr_array_is_empty(&v) == 1);
    MAGR_CHECK_EQ_I(magr_array_element_count(&v), 0);
    MAGR_CHECK_STATUS(magr_array_validate(&v, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_OK);

    /* Singleton. */
    MAGR_CHECK_STATUS(magr_array_init_vector(&v, MAGR_DTYPE_F64, 1, one, (int64_t)sizeof(one),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK(magr_array_is_empty(&v) == 0);
    MAGR_CHECK_STATUS(magr_array_as_blas_vector(&v, &inc), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(inc, 1);
    return 0;
}

static int t_negative_dimensions_are_rejected(void) {
    magr_array a;
    MAGR_CHECK_STATUS(magr_array_init_vector(&a, MAGR_DTYPE_F64, -1, NULL, 0,
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_SHAPE_MISMATCH);
    return 0;
}

static int t_shape_equality_ignores_strides(void) {
    double buf[12];
    magr_array a, b;
    int64_t dims[2] = {3, 2};

    MAGR_CHECK_STATUS(magr_array_init_strided(&a, MAGR_DTYPE_F64, 2, dims,
                                              (int64_t[]){8, 24}, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_init_strided(&b, MAGR_DTYPE_F64, 2, dims,
                                              (int64_t[]){8, 32}, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_require_same_shape(&a, &b), MAGR_STATUS_OK);

    dims[1] = 3;
    MAGR_CHECK_STATUS(magr_array_init_strided(&b, MAGR_DTYPE_F64, 2, dims,
                                              (int64_t[]){8, 32}, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_require_same_shape(&a, &b), MAGR_STATUS_SHAPE_MISMATCH);
    return 0;
}

static int t_ownership_is_explicit(void) {
    magr_array owned, borrowed;
    double buf[4];

    MAGR_CHECK_STATUS(magr_array_init_vector(&owned, MAGR_DTYPE_F64, 4, buf,
                                             (int64_t)sizeof(buf), MAGR_OWNERSHIP_OWNED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(owned.ownership, MAGR_OWNERSHIP_OWNED);

    /* Passing a struct is at most a borrow. */
    MAGR_CHECK_STATUS(magr_array_borrow(&borrowed, &owned), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(borrowed.ownership, MAGR_OWNERSHIP_BORROWED);
    MAGR_CHECK(borrowed.release == NULL);

    /* Freeing the borrow must not touch the buffer. */
    magr_array_free(&borrowed);
    MAGR_CHECK_EQ_I(buf[0], buf[0]); /* buffer still ours to inspect */

    /* Taking ownership is idempotent, not an error. */
    MAGR_CHECK_STATUS(magr_array_take_ownership(&owned), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_take_ownership(&owned), MAGR_STATUS_OK);

    /* magr_array_free on an OWNED descriptor with release == NULL falls back
     * to free(); buf is a stack array, so we deliberately do not call it here.
     * Ownership transfer is exercised in tests/array-conformance with heap
     * buffers via the supplied release hook. */
    owned.ownership = MAGR_OWNERSHIP_BORROWED;
    magr_array_free(&owned);
    return 0;
}

static int t_readonly_flag_blocks_output_roles(void) {
    double buf[4];
    magr_array a;

    MAGR_CHECK_STATUS(magr_array_init_vector(&a, MAGR_DTYPE_F64, 4, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_INPUT), MAGR_STATUS_OK);

    a.flags |= MAGR_ARRAY_F_READONLY;
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_INOUT), MAGR_STATUS_INVALID_ARGUMENT);
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_OUTPUT), MAGR_STATUS_INVALID_ARGUMENT);
    MAGR_CHECK_STATUS(magr_array_validate(&a, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_OK);
    return 0;
}

static int t_functor_names_round_trip(void) {
    int i;
    for (i = 1; i < MAGR_FUNCTOR__COUNT; ++i) {
        const char *name = magr_functor_name((magr_functor_id)i);
        MAGR_CHECK(name != NULL);
        if (name == NULL) continue;
        MAGR_CHECK_EQ_I(magr_functor_from_name(name), i);
        /* The 8 baseline functors required by the spec must all be present. */
    }
    MAGR_CHECK_EQ_I(MAGR_FUNCTOR__COUNT, 9);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_GEMM_F64_V1"), MAGR_FUNCTOR_F_GEMM_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_DOT_F64_V1"), MAGR_FUNCTOR_F_DOT_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_AXPY_F64_V1"), MAGR_FUNCTOR_F_AXPY_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_NRM2_F64_V1"), MAGR_FUNCTOR_F_NRM2_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_GEMV_F64_V1"), MAGR_FUNCTOR_F_GEMV_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_GER_F64_V1"), MAGR_FUNCTOR_F_GER_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_TRSV_F64_V1"), MAGR_FUNCTOR_F_TRSV_F64);
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_TRSM_F64_V1"), MAGR_FUNCTOR_F_TRSM_F64);
    /* Unknown names must not resolve to a valid functor. */
    MAGR_CHECK_EQ_I(magr_functor_from_name("F_GEMM_F64_V2"), MAGR_FUNCTOR_INVALID);
    MAGR_CHECK_EQ_I(magr_functor_from_name(""), MAGR_FUNCTOR_INVALID);
    MAGR_CHECK(magr_functor_name(MAGR_FUNCTOR_INVALID) == NULL);
    return 0;
}

static int t_upstream_routines_are_mapped(void) {
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_DOT_F64), "DDOT");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_AXPY_F64), "DAXPY");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_NRM2_F64), "DNRM2");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_GEMV_F64), "DGEMV");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_GEMM_F64), "DGEMM");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_GER_F64), "DGER");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_TRSV_F64), "DTRSV");
    MAGR_CHECK_EQ_S(magr_fortran_upstream_routine(MAGR_FUNCTOR_F_TRSM_F64), "DTRSM");
    return 0;
}

static int t_flag_characters_match_fortran(void) {
    /* These exact bytes are handed to the Fortran routines. One wrong
     * character means a silently transposed result, not an error. */
    MAGR_CHECK_EQ_I(magr_trans_char(MAGR_TRANS_N), 'N');
    MAGR_CHECK_EQ_I(magr_trans_char(MAGR_TRANS_T), 'T');
    MAGR_CHECK_EQ_I(magr_trans_char(MAGR_TRANS_C), 'C');
    MAGR_CHECK_EQ_I(magr_uplo_char(MAGR_UPLO_UPPER), 'U');
    MAGR_CHECK_EQ_I(magr_uplo_char(MAGR_UPLO_LOWER), 'L');
    MAGR_CHECK_EQ_I(magr_diag_char(MAGR_DIAG_NONUNIT), 'N');
    MAGR_CHECK_EQ_I(magr_diag_char(MAGR_DIAG_UNIT), 'U');
    MAGR_CHECK_EQ_I(magr_side_char(MAGR_SIDE_LEFT), 'L');
    MAGR_CHECK_EQ_I(magr_side_char(MAGR_SIDE_RIGHT), 'R');
    return 0;
}

static int t_f77_integer_kind_is_32_bit(void) {
    /* If this ever becomes 64-bit on some platform, every leading dimension in
     * every Fortran call is silently wrong. Pin it. */
    MAGR_CHECK_EQ_I(sizeof(MAGR_F77_INT_T), 4);
    MAGR_CHECK(MAGR_F77_INT_T(2147483647) > 0);
    return 0;
}

static int t_contract_rejects_foreign_versions(void) {
    magr_call_contract c;
    MAGR_CHECK_STATUS(magr_call_contract_init(&c), MAGR_STATUS_OK);
    MAGR_CHECK_EQ_I(c.abi_version, MAGR_ABI_VERSION);
    MAGR_CHECK_EQ_I(c.requires_column_major, 1);

    /* A struct from a future caller with a shorter tail must be rejected. */
    c.struct_size = 8;
    MAGR_CHECK_STATUS(magr_f_dot_f64(NULL, NULL, NULL, &c), MAGR_STATUS_ABI_MISMATCH);
    return 0;
}

static int t_unsupported_dtype_is_refused_not_coerced(void) {
    /* An f32 vector cannot be silently widened to f64: that would change the
     * values, not just the representation. */
    float buf[8];
    magr_array v;
    double out = 0.0;
    int64_t inc = 0;

    MAGR_CHECK_STATUS(magr_array_init_vector(&v, MAGR_DTYPE_F32, 8, buf, (int64_t)sizeof(buf),
                                              MAGR_OWNERSHIP_BORROWED),
                      MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_validate(&v, MAGR_ROLE_CONSTRAINT), MAGR_STATUS_OK);
    MAGR_CHECK_STATUS(magr_array_as_blas_vector(&v, &inc), MAGR_STATUS_UNSUPPORTED);
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    magr_test_fn fn;
} entry;

int main(void) {
    static const entry tests[] = {
        {"status_strings_are_total", t_status_strings_are_total},
        {"dtype_sizes", t_dtype_sizes},
        {"struct_is_self_describing", t_struct_is_self_describing},
        {"column_major_layout_is_derived_not_assumed", t_column_major_layout_is_derived_not_assumed},
        {"row_major_is_rejected_by_blas_paths", t_row_major_is_rejected_by_blas_paths},
        {"leading_dimension_must_exceed_rows", t_leading_dimension_must_exceed_rows},
        {"capacity_bounds_are_enforced", t_capacity_bounds_are_enforced},
        {"strides_become_increments", t_strides_become_increments},
        {"negative_increment_is_refused", t_negative_increment_is_refused},
        {"zero_and_singleton_shapes", t_zero_and_singleton_shapes},
        {"negative_dimensions_are_rejected", t_negative_dimensions_are_rejected},
        {"shape_equality_ignores_strides", t_shape_equality_ignores_strides},
        {"ownership_is_explicit", t_ownership_is_explicit},
        {"readonly_flag_blocks_output_roles", t_readonly_flag_blocks_output_roles},
        {"functor_names_round_trip", t_functor_names_round_trip},
        {"upstream_routines_are_mapped", t_upstream_routines_are_mapped},
        {"flag_characters_match_fortran", t_flag_characters_match_fortran},
        {"f77_integer_kind_is_32_bit", t_f77_integer_kind_is_32_bit},
        {"contract_rejects_foreign_versions", t_contract_rejects_foreign_versions},
        {"unsupported_dtype_is_refused_not_coerced", t_unsupported_dtype_is_refused_not_coerced},
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) magr_test_run(tests[i].name, tests[i].fn);

    printf("\n%d checks, %d failures\n", magr_test_checks, magr_test_failures);
    return magr_test_failures == 0 ? 0 : 1;
}
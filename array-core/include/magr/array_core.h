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
 * magr/array_core.h
 *
 * Array metadata, shape/stride validation, ownership, and the primitives that
 * every other MAGR component builds on. This is the only place that is allowed
 * to compute "does this descriptor make sense".
 *
 * Validation philosophy: array_core never guesses. If a descriptor is
 * ambiguous, malformed, or only interpretable under assumptions the caller did
 * not state, it is rejected with a specific status. Silent reinterpretation of a
 * buffer is how numerical systems acquire silent corruption.
 */

#ifndef MAGR_ARRAY_CORE_H
#define MAGR_ARRAY_CORE_H

#include "magr/abi/array_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Descriptor construction                                             */
/* ------------------------------------------------------------------ */

/*
 * Builds a column-major descriptor for `dims` over an existing buffer.
 *
 * `data` must point to the element that logical index (0,...,0) refers to.
 * The caller keeps ownership unless `ownership` is MAGR_OWNERSHIP_OWNED, in
 * which case `release` must be non-NULL when `data` was not obtained from
 * malloc.
 *
 * `buffer_bytes` is the number of bytes actually reachable from `data`. It is
 * used to bounds-check strided views; pass 0 only if you know the buffer is
 * exactly the dense extent of the array.
 */
magr_status magr_array_init_dense(magr_array *out,
                                  magr_dtype dtype,
                                  uint32_t rank,
                                  const int64_t *dims,
                                  void *data,
                                  int64_t buffer_bytes,
                                  magr_ownership ownership,
                                  void *release_ctx,
                                  void (*release)(void *, void *));

/* Builds a strided (non-dense) descriptor. Strides are in bytes. */
magr_status magr_array_init_strided(magr_array *out,
                                    magr_dtype dtype,
                                    uint32_t rank,
                                    const int64_t *dims,
                                    const int64_t *strides_bytes,
                                    void *data,
                                    int64_t buffer_bytes,
                                    magr_ownership ownership);

/* Convenience: 1-D dense array of `n` elements. */
magr_status magr_array_init_vector(magr_array *out,
                                   magr_dtype dtype,
                                   int64_t n,
                                   void *data,
                                   int64_t buffer_bytes,
                                   magr_ownership ownership);

/* Convenience: 2-D column-major array of `rows` x `cols` with the given leading
 * dimension `ld`. `ld >= rows` is enforced. */
magr_status magr_array_init_matrix_col_major(magr_array *out,
                                             magr_dtype dtype,
                                             int64_t rows,
                                             int64_t cols,
                                             int64_t ld,
                                             void *data,
                                             int64_t buffer_bytes,
                                             magr_ownership ownership);

/* Row-major is NOT part of the v1 numerical functor contract, but it is needed
 * to *ingest* row-major data from Klong/Uiua/Chapel front ends. Such an array
 * is valid for elementwise and view operations and is rejected by the
 * Fortran functors unless it happens to also be column-major. */
magr_status magr_array_init_matrix_row_major(magr_array *out,
                                             magr_dtype dtype,
                                             int64_t rows,
                                             int64_t cols,
                                             void *data,
                                             int64_t buffer_bytes,
                                             magr_ownership ownership);

/* Scalar-shaped array: rank 0, exactly one element. */
magr_status magr_array_init_scalar(magr_array *out,
                                   magr_dtype dtype,
                                   void *data,
                                   magr_ownership ownership);

/* Takes ownership of an already-initialised descriptor, clearing its release
 * hook. Used when handing a buffer from one component to another. */
magr_status magr_array_take_ownership(magr_array *arr);

/* Releases a descriptor's buffer if and only if it is MAGR_OWNERSHIP_OWNED.
 * Clears the descriptor afterwards. Idempotent. */
void magr_array_free(magr_array *arr);

/* Borrows the descriptor: same view, but no release hook and ownership forced
 * to BORROWED. This is the safe way to pass a view to code that must not free
 * it. */
magr_status magr_array_borrow(magr_array *out, const magr_array *in);

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

/* Product of dims. Returns 1 for rank 0. 0 if any dim is 0. Negative dims are
 * a caller bug; validation reports that, this function returns 0. */
int64_t magr_array_element_count(const magr_array *arr);

int magr_array_is_empty(const magr_array *arr);
int magr_array_is_dense(const magr_array *arr);
int magr_array_is_column_major(const magr_array *arr);
int magr_array_is_scalar(const magr_array *arr);

/* Byte offset of logical element `index` from arr->data. Returns -1 if the
 * element is outside the logical extent. */
int64_t magr_array_element_offset(const magr_array *arr, const int64_t *index);

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Full validation of an inbound descriptor as received across a language
 * boundary. Checks, in order:
 *
 *   struct_size >= offsetof(magr_array, release) + sizeof(release)
 *   abi_version major == local major
 *   dtype known, layout known, rank <= MAGR_MAX_RANK
 *   every dim >= 0
 *   product of dims does not overflow int64
 *   strides: for a dense column-major 2-D array, strides must equal
 *            (elem_size, rows*elem_size)
 *   every byte touched by the logical extent lies inside [data, data+capacity)
 *   MAGR_ARRAY_F_READONLY is not set on an output-only role (caller's job)
 *
 * `role` selects which extra checks apply. It is not part of the descriptor
 * because role is a property of the *call*, not of the memory.
 */
typedef enum magr_array_role {
    MAGR_ROLE_INPUT = 0,   /* may alias itself across calls; may be readonly */
    MAGR_ROLE_OUTPUT = 1,  /* must be writable, must be fully specified */
    MAGR_ROLE_INOUT = 2,   /* readable and writable */
    MAGR_ROLE_CONSTRAINT = 3 /* shape/leading dimension reference only */
} magr_array_role;

magr_status magr_array_validate(const magr_array *arr, magr_array_role role);

/* Convenience: validate a whole argument list. `arrs` may be NULL when
 * `count` is 0. Returns the first failure. */
magr_status magr_array_validate_all(const magr_array *arrs,
                                    uint32_t count,
                                    const magr_array_role *roles);

/* Checks that two arrays have identical dtype, rank, and dims. Deliberately
 * ignores strides and ownership. */
magr_status magr_array_require_same_shape(const magr_array *a, const magr_array *b);

/* ------------------------------------------------------------------ */
/* BLAS compatibility checks                                           */
/* ------------------------------------------------------------------ */

/*
 * A 2-D array is BLAS-callable iff:
 *   - dtype is F64 (v1),
 *   - layout is COLUMN_MAJOR,
 *   - strides == {elem_size, ld*elem_size} for some ld >= rows,
 *   - the buffer really contains ld*cols elements.
 * `ld_out` receives that leading dimension so the caller does not recompute it.
 */
magr_status magr_array_as_blas_matrix(const magr_array *arr, int64_t *ld_out);

/* A 1-D array is BLAS-callable iff dtype is F64 and stride == elem_size.
 * A strided vector with stride k*elem_size (k > 0) is passed to the Fortran
 * routine as N and INC=k directly; that is also BLAS-callable. `inc_out`
 * receives the increment. Negative increments are rejected in v1 because the
 * reference BLAS handles them but Klong/Uiua lowering does not produce them
 * and silent asymmetry is a bad trade. */
magr_status magr_array_as_blas_vector(const magr_array *arr, int64_t *inc_out);

/* ------------------------------------------------------------------ */
/* Transpose flags, spelled once                                       */
/* ------------------------------------------------------------------ */

typedef enum magr_trans {
    MAGR_TRANS_N = 0, /* 'N' no transpose */
    MAGR_TRANS_T = 1, /* 'T' transpose */
    MAGR_TRANS_C = 2  /* 'C' conjugate transpose (== T for real dtypes) */
} magr_trans;

/* The single character the Fortran routine expects. MAGR_TRANS_C is lowered to
 * 'C' verbatim for future complex support and documented as equal to 'T' for
 * F64. */
char magr_trans_char(magr_trans t);

const char *magr_trans_name(magr_trans t);

typedef enum magr_side {
    MAGR_SIDE_LEFT = 0,
    MAGR_SIDE_RIGHT = 1
} magr_side;
char magr_side_char(magr_side s);

typedef enum magr_uplo {
    MAGR_UPLO_UPPER = 0,
    MAGR_UPLO_LOWER = 1
} magr_uplo;
char magr_uplo_char(magr_uplo u);

typedef enum magr_diag {
    MAGR_DIAG_NONUNIT = 0,
    MAGR_DIAG_UNIT = 1
} magr_diag;
char magr_diag_char(magr_diag d);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_ARRAY_CORE_H */
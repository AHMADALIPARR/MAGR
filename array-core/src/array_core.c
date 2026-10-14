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

/* magr/array_core.c -- implementation of the MAGR Array ABI metadata layer. */

#include "magr/array_core.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Status strings                                                      */
/* ------------------------------------------------------------------ */

const char *magr_status_string(magr_status status) {
    switch (status) {
    case MAGR_STATUS_OK: return "OK";
    case MAGR_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case MAGR_STATUS_ABI_MISMATCH: return "ABI_MISMATCH";
    case MAGR_STATUS_RANK_UNSUPPORTED: return "RANK_UNSUPPORTED";
    case MAGR_STATUS_SHAPE_MISMATCH: return "SHAPE_MISMATCH";
    case MAGR_STATUS_STRIDE_UNSUPPORTED: return "STRIDE_UNSUPPORTED";
    case MAGR_STATUS_BOUNDS: return "BOUNDS";
    case MAGR_STATUS_SINGULAR: return "SINGULAR";
    case MAGR_STATUS_DIVISION_BY_ZERO: return "DIVISION_BY_ZERO";
    case MAGR_STATUS_UNSUPPORTED: return "UNSUPPORTED";
    case MAGR_STATUS_FUNACTOR_UNAVAILABLE: return "FUNCRIPTOR_UNAVAILABLE";
    case MAGR_STATUS_MANIFEST_REJECTED: return "MANIFEST_REJECTED";
    case MAGR_STATUS_INTERNAL: return "INTERNAL";
    default: return "UNKNOWN_STATUS";
    }
}

/* ------------------------------------------------------------------ */
/* dtype helpers                                                       */
/* ------------------------------------------------------------------ */

size_t magr_dtype_size(magr_dtype dtype) {
    switch (dtype) {
    case MAGR_DTYPE_F64: return 8;
    case MAGR_DTYPE_F32: return 4;
    case MAGR_DTYPE_I64: return 8;
    case MAGR_DTYPE_I32: return 4;
    case MAGR_DTYPE_U8: return 1;
    case MAGR_DTYPE_BOOL: return 1;
    case MAGR_DTYPE_INVALID:
    default: return 0;
    }
}

int magr_dtype_is_float(magr_dtype dtype) {
    return dtype == MAGR_DTYPE_F64 || dtype == MAGR_DTYPE_F32;
}

const char *magr_dtype_name(magr_dtype dtype) {
    switch (dtype) {
    case MAGR_DTYPE_F64: return "f64";
    case MAGR_DTYPE_F32: return "f32";
    case MAGR_DTYPE_I64: return "i64";
    case MAGR_DTYPE_I32: return "i32";
    case MAGR_DTYPE_U8: return "u8";
    case MAGR_DTYPE_BOOL: return "bool";
    case MAGR_DTYPE_INVALID:
    default: return "invalid";
    }
}

/* ------------------------------------------------------------------ */
/* Functor names                                                       */
/* ------------------------------------------------------------------ */

static const char *const kFunctorNames[MAGR_FUNCTOR__COUNT] = {
    NULL,
    "F_DOT_F64_V1",
    "F_AXPY_F64_V1",
    "F_NRM2_F64_V1",
    "F_GEMV_F64_V1",
    "F_GEMM_F64_V1",
    "F_GER_F64_V1",
    "F_TRSV_F64_V1",
    "F_TRSM_F64_V1",
};

const char *magr_functor_name(magr_functor_id id) {
    if ((int)id <= 0 || (int)id >= MAGR_FUNCTOR__COUNT) return NULL;
    return kFunctorNames[(int)id];
}

magr_functor_id magr_functor_from_name(const char *name) {
    int i;
    if (name == NULL) return MAGR_FUNCTOR_INVALID;
    for (i = 1; i < MAGR_FUNCTOR__COUNT; ++i) {
        if (strcmp(kFunctorNames[i], name) == 0) return (magr_functor_id)i;
    }
    return MAGR_FUNCTOR_INVALID;
}

/* ------------------------------------------------------------------ */
/* Flag character helpers                                              */
/* ------------------------------------------------------------------ */

char magr_trans_char(magr_trans t) {
    switch (t) {
    case MAGR_TRANS_N: return 'N';
    case MAGR_TRANS_T: return 'T';
    case MAGR_TRANS_C: return 'C';
    default: return '?';
    }
}

const char *magr_trans_name(magr_trans t) {
    switch (t) {
    case MAGR_TRANS_N: return "N";
    case MAGR_TRANS_T: return "T";
    case MAGR_TRANS_C: return "C";
    default: return "?";
    }
}

char magr_side_char(magr_side s) { return s == MAGR_SIDE_LEFT ? 'L' : 'R'; }

char magr_uplo_char(magr_uplo u) { return u == MAGR_UPLO_UPPER ? 'U' : 'L'; }

char magr_diag_char(magr_diag d) { return d == MAGR_DIAG_UNIT ? 'U' : 'N'; }

/* ------------------------------------------------------------------ */
/* Call contract                                                       */
/* ------------------------------------------------------------------ */

magr_status magr_call_contract_init(magr_call_contract *out) {
    if (out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = (uint32_t)MAGR_ABI_VERSION;
    out->num_contract = (uint32_t)MAGR_NUM_REORDER_DEFAULT;
    out->requires_column_major = 1;
    out->rel_tolerance = 0.0;
    out->abs_tolerance = 0.0;
    out->provenance = NULL;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int64_t checked_product(const int64_t *dims, uint32_t rank, int *overflow) {
    int64_t product = 1;
    uint32_t i;
    *overflow = 0;
    for (i = 0; i < rank; ++i) {
        if (dims[i] == 0) return 0;
        if (dims[i] < 0) { *overflow = 1; return 0; }
        if (dims[i] != 0 && product > (INT64_MAX / dims[i])) { *overflow = 1; return 0; }
        product *= dims[i];
    }
    return product;
}

static magr_status validate_header(const magr_array *arr) {
    uint32_t local_major = (uint32_t)MAGR_ABI_VERSION >> 16;
    if (arr == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (arr->struct_size < offsetof(magr_array, release) + sizeof(arr->release))
        return MAGR_STATUS_ABI_MISMATCH;
    if (((arr->abi_version >> 16) & 0xFFFFu) != local_major)
        return MAGR_STATUS_ABI_MISMATCH;
    if (magr_dtype_size((magr_dtype)arr->dtype) == 0)
        return MAGR_STATUS_INVALID_ARGUMENT;
    if (arr->layout != MAGR_LAYOUT_COLUMN_MAJOR && arr->layout != MAGR_LAYOUT_ROW_MAJOR &&
        arr->layout != MAGR_LAYOUT_STRIDED)
        return MAGR_STATUS_INVALID_ARGUMENT;
    if (arr->rank > MAGR_MAX_RANK) return MAGR_STATUS_RANK_UNSUPPORTED;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static magr_status init_common(magr_array *out,
                               magr_dtype dtype,
                               uint32_t rank,
                               void *data,
                               int64_t buffer_bytes,
                               magr_ownership ownership) {
    int overflow = 0;
    int64_t count;
    size_t esize;

    if (out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    esize = magr_dtype_size(dtype);
    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;
    if (rank > MAGR_MAX_RANK) return MAGR_STATUS_RANK_UNSUPPORTED;
    if (ownership != MAGR_OWNERSHIP_BORROWED && ownership != MAGR_OWNERSHIP_OWNED &&
        ownership != MAGR_OWNERSHIP_VIEW)
        return MAGR_STATUS_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = (uint32_t)MAGR_ABI_VERSION;
    out->dtype = (uint32_t)dtype;
    out->rank = rank;
    out->data = data;
    out->ownership = (uint32_t)ownership;

    count = checked_product(out->dims, rank, &overflow);
    if (overflow) return MAGR_STATUS_SHAPE_MISMATCH;
    out->nbytes = count * (int64_t)esize;
    if (buffer_bytes > 0) out->capacity_bytes = buffer_bytes;
    else out->capacity_bytes = out->nbytes;
    if (out->nbytes == 0) out->flags |= MAGR_ARRAY_F_EMPTY;
    return MAGR_STATUS_OK;
}

magr_status magr_array_init_dense(magr_array *out,
                                  magr_dtype dtype,
                                  uint32_t rank,
                                  const int64_t *dims,
                                  void *data,
                                  int64_t buffer_bytes,
                                  magr_ownership ownership,
                                  void *release_ctx,
                                  void (*release)(void *, void *)) {
    magr_status st;
    size_t esize;
    uint32_t i;
    int64_t row_major_total;

    if (dims == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    esize = magr_dtype_size(dtype);
    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = init_common(out, dtype, rank, data, buffer_bytes, ownership);
    if (st != MAGR_STATUS_OK) return st;

    row_major_total = 1;
    for (i = 0; i < rank; ++i) {
        if (dims[i] < 0) return MAGR_STATUS_SHAPE_MISMATCH;
        out->dims[i] = dims[i];
        out->strides[i] = row_major_total * (int64_t)esize;
        row_major_total = (row_major_total == 0 || dims[i] == 0) ? 0 : row_major_total * dims[i];
    }
    out->layout = (uint32_t)MAGR_LAYOUT_ROW_MAJOR;
    out->nbytes = row_major_total * (int64_t)esize;
    if (row_major_total == 0) out->flags |= MAGR_ARRAY_F_EMPTY;

    out->release_ctx = release_ctx;
    out->release = release;
    return MAGR_STATUS_OK;
}

magr_status magr_array_init_strided(magr_array *out,
                                    magr_dtype dtype,
                                    uint32_t rank,
                                    const int64_t *dims,
                                    const int64_t *strides_bytes,
                                    void *data,
                                    int64_t buffer_bytes,
                                    magr_ownership ownership) {
    magr_status st;
    size_t esize;
    uint32_t i;
    int overflow = 0;
    int64_t min_off = 0, max_off = 0;
    int first = 1;

    if (dims == NULL || strides_bytes == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    esize = magr_dtype_size(dtype);
    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;

    st = init_common(out, dtype, rank, data, buffer_bytes, ownership);
    if (st != MAGR_STATUS_OK) return st;

    for (i = 0; i < rank; ++i) {
        if (dims[i] < 0) return MAGR_STATUS_SHAPE_MISMATCH;
        out->dims[i] = dims[i];
        out->strides[i] = strides_bytes[i];
    }
    out->layout = (uint32_t)MAGR_LAYOUT_STRIDED;

    /* Span of the logical extent under arbitrary strides. */
    (void)checked_product(out->dims, rank, &overflow);
    if (overflow) return MAGR_STATUS_SHAPE_MISMATCH;
    for (i = 0; i < rank; ++i) {
        if (out->dims[i] == 0) continue;
        int64_t s = out->strides[i];
        int64_t span = s * (out->dims[i] - 1);
        if (first) {
            min_off = (s < 0) ? span : 0;
            max_off = (s < 0) ? 0 : span;
            first = 0;
        } else {
            if (s < 0) { if (span < min_off) min_off = span; }
            else       { if (span > max_off) max_off = span; }
        }
    }
    if (!first) {
        int64_t span = max_off - min_off + (int64_t)esize;
        out->nbytes = span;
        out->capacity_bytes = (buffer_bytes > 0) ? buffer_bytes : span;
        out->offset = min_off;
    }
    return MAGR_STATUS_OK;
}

magr_status magr_array_init_vector(magr_array *out,
                                   magr_dtype dtype,
                                   int64_t n,
                                   void *data,
                                   int64_t buffer_bytes,
                                   magr_ownership ownership) {
    int64_t dims[1];
    if (n < 0) return MAGR_STATUS_SHAPE_MISMATCH;
    dims[0] = n;
    return magr_array_init_dense(out, dtype, 1, dims, data, buffer_bytes, ownership, NULL, NULL);
}

magr_status magr_array_init_matrix_col_major(magr_array *out,
                                             magr_dtype dtype,
                                             int64_t rows,
                                             int64_t cols,
                                             int64_t ld,
                                             void *data,
                                             int64_t buffer_bytes,
                                             magr_ownership ownership) {
    magr_status st;
    size_t esize;
    int64_t dims[2], strides[2];

    if (rows < 0 || cols < 0) return MAGR_STATUS_SHAPE_MISMATCH;
    if (ld < rows) return MAGR_STATUS_SHAPE_MISMATCH;
    esize = magr_dtype_size(dtype);
    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;
    if (ld == 0) ld = rows;

    dims[0] = rows;
    dims[1] = cols;
    strides[0] = (int64_t)esize;
    strides[1] = ld * (int64_t)esize;

    st = magr_array_init_strided(out, dtype, 2, dims, strides, data, buffer_bytes, ownership);
    if (st != MAGR_STATUS_OK) return st;
    out->layout = (uint32_t)MAGR_LAYOUT_COLUMN_MAJOR;
    out->nbytes = rows * (int64_t)esize;
    out->capacity_bytes = (buffer_bytes > 0) ? buffer_bytes : ld * cols * (int64_t)esize;
    out->offset = 0;
    if (rows * cols == 0) out->flags |= MAGR_ARRAY_F_EMPTY;
    else out->flags &= ~(uint32_t)MAGR_ARRAY_F_EMPTY;
    return MAGR_STATUS_OK;
}

magr_status magr_array_init_matrix_row_major(magr_array *out,
                                             magr_dtype dtype,
                                             int64_t rows,
                                             int64_t cols,
                                             void *data,
                                             int64_t buffer_bytes,
                                             magr_ownership ownership) {
    int64_t dims[2], strides[2];
    size_t esize = magr_dtype_size(dtype);

    if (rows < 0 || cols < 0) return MAGR_STATUS_SHAPE_MISMATCH;
    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;

    dims[0] = rows;
    dims[1] = cols;
    strides[0] = cols * (int64_t)esize;
    strides[1] = (int64_t)esize;

    return magr_array_init_strided(out, dtype, 2, dims, strides, data, buffer_bytes, ownership);
}

magr_status magr_array_init_scalar(magr_array *out,
                                   magr_dtype dtype,
                                   void *data,
                                   magr_ownership ownership) {
    magr_status st;
    size_t esize = magr_dtype_size(dtype);

    if (esize == 0) return MAGR_STATUS_INVALID_ARGUMENT;
    st = init_common(out, dtype, 0, data, (int64_t)esize, ownership);
    if (st != MAGR_STATUS_OK) return st;
    out->layout = (uint32_t)MAGR_LAYOUT_STRIDED;
    out->nbytes = (int64_t)esize;
    out->capacity_bytes = (int64_t)esize;
    return MAGR_STATUS_OK;
}

magr_status magr_array_take_ownership(magr_array *arr) {
    if (arr == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (arr->ownership == MAGR_OWNERSHIP_OWNED) return MAGR_STATUS_OK;
    if (arr->ownership != MAGR_OWNERSHIP_BORROWED && arr->ownership != MAGR_OWNERSHIP_VIEW)
        return MAGR_STATUS_INVALID_ARGUMENT;
    arr->ownership = (uint32_t)MAGR_OWNERSHIP_OWNED;
    return MAGR_STATUS_OK;
}

void magr_array_free(magr_array *arr) {
    if (arr == NULL) return;
    if (arr->ownership == (uint32_t)MAGR_OWNERSHIP_OWNED && arr->data != NULL) {
        if (arr->release != NULL) arr->release(arr->release_ctx, arr->data);
        else free(arr->data); /* declared in stdlib.h via array_core.h consumers */
    }
    memset(arr, 0, sizeof(*arr));
}

magr_status magr_array_borrow(magr_array *out, const magr_array *in) {
    magr_status st;
    if (in == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = validate_header(in);
    if (st != MAGR_STATUS_OK) return st;
    if (out == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    memcpy(out, in, sizeof(*out));
    out->ownership = (uint32_t)MAGR_OWNERSHIP_BORROWED;
    out->release_ctx = NULL;
    out->release = NULL;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

int64_t magr_array_element_count(const magr_array *arr) {
    int overflow = 0;
    int64_t count;
    if (arr == NULL) return 0;
    count = checked_product(arr->dims, arr->rank, &overflow);
    return overflow ? 0 : count;
}

int magr_array_is_empty(const magr_array *arr) {
    return arr == NULL || (arr->flags & MAGR_ARRAY_F_EMPTY) != 0;
}

int magr_array_is_scalar(const magr_array *arr) {
    return arr != NULL && arr->rank == 0;
}

int magr_array_is_dense(const magr_array *arr) {
    uint32_t i;
    int64_t expected;
    size_t esize;

    if (arr == NULL || arr->rank == 0) return arr != NULL;
    esize = magr_dtype_size((magr_dtype)arr->dtype);
    expected = (int64_t)esize;
    for (i = 0; i < arr->rank; ++i) {
        if (arr->strides[i] != expected) return 0;
        if (arr->dims[i] != 0) expected *= arr->dims[i];
    }
    return 1;
}

int magr_array_is_column_major(const magr_array *arr) {
    size_t esize;
    int64_t ld;

    if (arr == NULL || arr->rank != 2) return 0;
    if (arr->layout != (uint32_t)MAGR_LAYOUT_COLUMN_MAJOR) return 0;
    esize = magr_dtype_size((magr_dtype)arr->dtype);
    if (esize == 0) return 0;
    if (arr->strides[0] != (int64_t)esize) return 0;
    if (arr->strides[1] % (int64_t)esize != 0) return 0;
    ld = arr->strides[1] / (int64_t)esize;
    if (ld < arr->dims[0]) return 0;
    return 1;
}

int64_t magr_array_element_offset(const magr_array *arr, const int64_t *index) {
    size_t esize;
    int64_t off = 0;
    uint32_t i;

    if (arr == NULL) return -1;
    if (arr->rank == 0) return 0;
    if (index == NULL) return -1;
    esize = magr_dtype_size((magr_dtype)arr->dtype);
    if (esize == 0) return -1;

    for (i = 0; i < arr->rank; ++i) {
        if (index[i] < 0 || index[i] >= arr->dims[i]) return -1;
        off += index[i] * arr->strides[i];
    }
    return off;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

magr_status magr_array_validate(const magr_array *arr, magr_array_role role) {
    magr_status st;
    size_t esize;
    uint32_t i;
    int overflow = 0;
    int64_t count, min_off = 0, max_off = 0;
    int first = 1;

    st = validate_header(arr);
    if (st != MAGR_STATUS_OK) return st;
    esize = magr_dtype_size((magr_dtype)arr->dtype);
    count = checked_product(arr->dims, arr->rank, &overflow);
    if (overflow) return MAGR_STATUS_SHAPE_MISMATCH;

    if (count == 0) {
        /* A zero-length array may legally have a NULL data pointer. */
        if (role == MAGR_ROLE_OUTPUT && arr->data == NULL && arr->nbytes != 0)
            return MAGR_STATUS_INVALID_ARGUMENT;
        return MAGR_STATUS_OK;
    }

    if (arr->data == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    if (role == MAGR_ROLE_OUTPUT && (arr->flags & MAGR_ARRAY_F_READONLY) != 0)
        return MAGR_STATUS_INVALID_ARGUMENT;

    if (arr->layout == MAGR_LAYOUT_COLUMN_MAJOR && arr->rank == 2) {
        if (!magr_array_is_column_major(arr)) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    }

    /* Bounds: the extreme byte touched by the logical extent must lie inside
     * the declared capacity. A capacity of 0 means "trust me, it is exactly
     * dense"; we then require the span to equal nbytes. */
    for (i = 0; i < arr->rank; ++i) {
        if (arr->dims[i] == 0) continue;
        {
            int64_t s = arr->strides[i];
            int64_t span = s * (arr->dims[i] - 1);
            if (first) {
                min_off = (s < 0) ? span : 0;
                max_off = (s < 0) ? 0 : span;
                first = 0;
            } else {
                if (s < 0) { if (span < min_off) min_off = span; }
                else       { if (span > max_off) max_off = span; }
            }
        }
    }
    if (!first) {
        int64_t span = max_off - min_off + (int64_t)esize;
        int64_t lo = min_off;
        if (arr->capacity_bytes > 0) {
            if (lo < 0) return MAGR_STATUS_BOUNDS;
            if (span > arr->capacity_bytes) return MAGR_STATUS_BOUNDS;
            if (lo + span > arr->capacity_bytes) return MAGR_STATUS_BOUNDS;
        } else if (arr->rank > 1) {
            /* Strided view with unknown capacity: only safe if dense. */
            if (!magr_array_is_dense(arr)) return MAGR_STATUS_BOUNDS;
        }
    }
    return MAGR_STATUS_OK;
}

magr_status magr_array_validate_all(const magr_array *arrs,
                                    uint32_t count,
                                    const magr_array_role *roles) {
    uint32_t i;
    if (count > 0 && (arrs == NULL || roles == NULL)) return MAGR_STATUS_INVALID_ARGUMENT;
    for (i = 0; i < count; ++i) {
        magr_status st = magr_array_validate(&arrs[i], roles[i]);
        if (st != MAGR_STATUS_OK) return st;
    }
    return MAGR_STATUS_OK;
}

magr_status magr_array_require_same_shape(const magr_array *a, const magr_array *b) {
    uint32_t i;
    if (a == NULL || b == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    if (a->dtype != b->dtype) return MAGR_STATUS_SHAPE_MISMATCH;
    if (a->rank != b->rank) return MAGR_STATUS_SHAPE_MISMATCH;
    for (i = 0; i < a->rank; ++i) {
        if (a->dims[i] != b->dims[i]) return MAGR_STATUS_SHAPE_MISMATCH;
    }
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* BLAS compatibility                                                  */
/* ------------------------------------------------------------------ */

magr_status magr_array_as_blas_matrix(const magr_array *arr, int64_t *ld_out) {
    magr_status st;
    size_t esize;
    int64_t ld;

    if (arr == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = magr_array_validate(arr, MAGR_ROLE_CONSTRAINT);
    if (st != MAGR_STATUS_OK) return st;
    if (arr->rank != 2) return MAGR_STATUS_RANK_UNSUPPORTED;
    if ((magr_dtype)arr->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;

    esize = magr_dtype_size((magr_dtype)arr->dtype);
    if (arr->strides[0] != (int64_t)esize) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    if (arr->strides[1] % (int64_t)esize != 0) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    ld = arr->strides[1] / (int64_t)esize;
    if (ld < arr->dims[0]) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    /* The leading dimension is a claim about the buffer; we check that the
     * declared capacity can actually back ld columns. */
    if (arr->capacity_bytes > 0 &&
        ld * arr->dims[1] * (int64_t)esize > arr->capacity_bytes)
        return MAGR_STATUS_BOUNDS;

    if (ld_out != NULL) *ld_out = ld;
    return MAGR_STATUS_OK;
}

magr_status magr_array_as_blas_vector(const magr_array *arr, int64_t *inc_out) {
    magr_status st;
    size_t esize;
    int64_t inc;

    if (arr == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    st = magr_array_validate(arr, MAGR_ROLE_CONSTRAINT);
    if (st != MAGR_STATUS_OK) return st;
    if (arr->rank != 1) return MAGR_STATUS_RANK_UNSUPPORTED;
    if ((magr_dtype)arr->dtype != MAGR_DTYPE_F64) return MAGR_STATUS_UNSUPPORTED;

    esize = magr_dtype_size((magr_dtype)arr->dtype);
    if (arr->strides[0] % (int64_t)esize != 0) return MAGR_STATUS_STRIDE_UNSUPPORTED;
    inc = arr->strides[0] / (int64_t)esize;
    if (inc <= 0) return MAGR_STATUS_STRIDE_UNSUPPORTED; /* negative inc: see header */
    if (arr->dims[0] > 1) {
        int64_t need = (arr->dims[0] - 1) * inc * (int64_t)esize;
        if (arr->capacity_bytes > 0 && need > arr->capacity_bytes) return MAGR_STATUS_BOUNDS;
    }
    if (inc_out != NULL) *inc_out = inc;
    return MAGR_STATUS_OK;
}
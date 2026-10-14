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
 * magr/sha256.h -- FIPS 180-4 SHA-256, streaming, no dynamic allocation.
 *
 * Self-contained on purpose. merkle-core is the trust anchor of the whole
 * system: if it links a third-party crypto library, then "the Merkle root is
 * trustworthy" quietly becomes "the Merkle root is as trustworthy as that
 * library and its build". A 200-line implementation that can be read in one
 * sitting is worth more here than a dependency.
 *
 * The implementation is verified against the FIPS/NIST test vectors in
 * tests/merkle-integrity/test_sha256.c.
 */

#ifndef MAGR_SHA256_H
#define MAGR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_SHA256_DIGEST_SIZE 32
#define MAGR_SHA256_BLOCK_SIZE 64
/* Worst-case message length encoding: 8 bytes of length. */
#define MAGR_SHA256_MAX_INPUT ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

typedef struct magr_sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[MAGR_SHA256_BLOCK_SIZE];
    size_t buffered;
} magr_sha256;

void magr_sha256_init(magr_sha256 *ctx);
void magr_sha256_update(magr_sha256 *ctx, const void *data, size_t len);
void magr_sha256_final(magr_sha256 *ctx, uint8_t out[MAGR_SHA256_DIGEST_SIZE]);

/* One-shot convenience. */
void magr_sha256(const void *data, size_t len, uint8_t out[MAGR_SHA256_DIGEST_SIZE]);

/* Lowercase hex, writes 65 bytes including the NUL. `hex` may be NULL. */
void magr_sha256_hex(const uint8_t digest[MAGR_SHA256_DIGEST_SIZE], char *hex);

/* Parses 64 lowercase-or-uppercase hex chars. Returns 0 on success. */
int magr_sha256_from_hex(const char *hex, uint8_t out[MAGR_SHA256_DIGEST_SIZE]);

/*
 * File digest helper. Returns MAGR-style status codes without depending on
 * array-core (so merkle-core stays a leaf module with no MAGR dependencies
 * beyond stdint):
 *   0  ok, 1  file open failed, 2  read error
 */
int magr_sha256_file(const char *path, uint8_t out[MAGR_SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_SHA256_H */
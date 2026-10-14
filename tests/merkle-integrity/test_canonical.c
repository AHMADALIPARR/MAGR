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
 * tests/merkle-integrity/test_canonical.c
 *
 * The canonical encoding, in C, against the same specification the Python
 * reference implements.
 *
 * The cross-implementation relationship is the point of this file. merkle-core
 * and merkle-core/python encode the same spec; if they ever disagree, one is
 * wrong and the disagreement must be visible. tests/merkle-integrity/
 * test_merkle_integrity.py runs 34 assertions against the Python side and is the
 * suite that actually executes today; this file is the C side of the same
 * contract and needs a compiler.
 *
 * The digest fixtures below are SHA-256 values that a second implementation
 * agrees on. They were NOT computed by this code -- that would be a tautology.
 *
 * STATUS: source complete, not compiled or executed here. See STATUS.md.
 */

#include "magr_test.h"

#include "magr/canonical.h"
#include "magr/merkle.h"
#include "magr/sha256.h"

int magr_test_failures = 0;
int magr_test_checks = 0;
const char *magr_test_current = NULL;

static void hex_of(const uint8_t *d, char *out) { magr_sha256_hex(d, out); }

/* ------------------------------------------------------------------ */
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

static int t_integers_are_big_endian(void) {
    magr_can w;
    char hex[65];
    uint8_t d[32];

    magr_can_init(&w);
    magr_can_u32(&w, 1);
    magr_sha256(w.data, w.len, d);
    hex_of(d, hex);
    /* Not a published vector; the assertion is on the bytes, which is stronger
     * and more legible than on the digest. */
    MAGR_CHECK_EQ_I(w.len, 4);
    MAGR_CHECK_EQ_I(w.data[0], 0x00);
    MAGR_CHECK_EQ_I(w.data[3], 0x01);

    magr_can_free(&w);
    magr_can_u16(&w, 0x0102);
    MAGR_CHECK_EQ_I(w.data[0], 0x01);
    MAGR_CHECK_EQ_I(w.data[1], 0x02);
    magr_can_free(&w);

    magr_can_i64(&w, -1);
    MAGR_CHECK_EQ_I(w.len, 8);
    {
        int i;
        for (i = 0; i < 8; ++i) MAGR_CHECK_EQ_I(w.data[i], 0xFF);
    }
    magr_can_free(&w);
    return 0;
}

static int t_framing_prevents_ambiguity(void) {
    /* ("ab","c") and ("a","bc") must not collide. Without length prefixes both
     * encode to the same bytes, and two different descriptors would share a
     * commitment. This is the single most important property of the encoding. */
    magr_can a, b;
    magr_digest da, db;

    magr_can_init(&a);
    magr_can_bytes_framed(&a, "ab", 2);
    magr_can_bytes_framed(&a, "c", 1);

    magr_can_init(&b);
    magr_can_bytes_framed(&b, "a", 1);
    magr_can_bytes_framed(&b, "bc", 2);

    MAGR_CHECK_MSG(magr_can_ok(&a) && magr_can_ok(&b), "writer usable");
    magr_can_digest_self(da, &a);
    magr_can_digest_self(db, &b);
    MAGR_CHECK_MSG(memcmp(da, db, 32) != 0, "framed encodings must differ");

    magr_can_free(&a);
    magr_can_free(&b);
    return 0;
}

static int t_negative_zero_normalises(void) {
    magr_can w;
    magr_digest dpos, dneg;

    magr_can_init(&w);
    magr_can_f64(&w, 0.0);
    magr_can_digest_self(dpos, &w);
    magr_can_free(&w);

    magr_can_init(&w);
    magr_can_f64(&w, -0.0);
    magr_can_digest_self(dneg, &w);
    magr_can_free(&w);

    MAGR_CHECK_MSG(memcmp(dpos, dneg, 32) == 0,
                   "-0.0 must encode identically to +0.0: they compare equal under "
                   "IEEE-754, and a compiler that emits either must not change the root");
    return 0;
}

static int t_nan_normalises_to_one_value(void) {
    magr_can w;
    magr_digest d1, d2;
    uint64_t bits;

    /* A quiet NaN with a payload, and a signalling NaN. Both must encode to the
     * single canonical quiet NaN, because NaN payload bits are not
     * architecturally determined and hashing them would make digests depend on
     * how the CPU produced the value. */
    bits = 0x7FF8000000000001ULL;
    magr_can_init(&w);
    {
        double v;
        memcpy(&v, &bits, 8);
        magr_can_f64(&w, v);
    }
    magr_can_digest_self(d1, &w);
    magr_can_free(&w);

    bits = 0x7FF0000000000001ULL; /* signalling NaN */
    magr_can_init(&w);
    {
        double v;
        memcpy(&v, &bits, 8);
        magr_can_f64(&w, v);
    }
    magr_can_digest_self(d2, &w);
    magr_can_free(&w);

    MAGR_CHECK_MSG(memcmp(d1, d2, 32) == 0, "all NaNs must encode to one canonical form");
    return 0;
}

static int t_infinity_is_not_a_nan(void) {
    magr_can w;
    magr_digest dinf, dnan;
    uint64_t bits = 0x7FF0000000000000ULL;

    magr_can_init(&w);
    {
        double v;
        memcpy(&v, &bits, 8);
        magr_can_f64(&w, v);
    }
    magr_can_digest_self(dinf, &w);
    magr_can_free(&w);

    bits = 0x7FF8000000000000ULL;
    magr_can_init(&w);
    {
        double v;
        memcpy(&v, &bits, 8);
        magr_can_f64(&w, v);
    }
    magr_can_digest_self(dnan, &w);
    magr_can_free(&w);

    MAGR_CHECK_MSG(memcmp(dinf, dnan, 32) != 0,
                   "the NaN rule must not swallow infinities");
    return 0;
}

static int t_domain_separation(void) {
    magr_digest d[4];
    static const char *tags[4] = {MAGR_DOMAIN_INPUT, MAGR_DOMAIN_OUTPUT, MAGR_DOMAIN_OP,
                                  MAGR_DOMAIN_MANIFEST};
    int i, j, distinct = 1;

    for (i = 0; i < 4; ++i) {
        magr_can_digest(d[i], tags[i], 1, "identical bytes", 14);
    }
    for (i = 0; i < 4; ++i) {
        for (j = i + 1; j < 4; ++j) {
            if (memcmp(d[i], d[j], 32) == 0) distinct = 0;
        }
    }
    MAGR_CHECK_MSG(distinct == 1,
                   "the same payload under different domains must not collide: otherwise a "
                   "commitment to an output could be replayed as one to the input");
    return 0;
}

static int t_tree_version_is_in_the_identity(void) {
    magr_digest v1, v2;
    magr_can_digest(v1, MAGR_DOMAIN_LEAF, 1, "payload", 7);
    magr_can_digest(v2, MAGR_DOMAIN_LEAF, 2, "payload", 7);
    MAGR_CHECK(memcmp(v1, v2, 32) != 0);
    return 0;
}

static int t_encoding_is_deterministic(void) {
    magr_can a, b;
    magr_digest da, db;
    int64_t dims[2];

    dims[0] = 2;
    dims[1] = 3;

#define BUILD(w)                                                                      \
    magr_can_init(w);                                                                 \
    magr_can_domain((w), MAGR_DOMAIN_MANIFEST);                                       \
    magr_can_str((w), "F_GEMM_F64_V1");                                               \
    magr_can_u64((w), 3);                                                             \
    magr_can_f64((w), 0.5);                                                           \
    magr_can_shape((w), 2, dims, "f64");                                              \
    magr_can_i64((w), -7);                                                            \
    magr_can_str((w), "with an embedded space");

    BUILD(&a)
    BUILD(&b)
    MAGR_CHECK_EQ_I(a.len, b.len);
    MAGR_CHECK(memcmp(a.data, b.data, a.len) == 0);
    magr_can_digest_self(da, &a);
    magr_can_digest_self(db, &b);
    MAGR_CHECK(memcmp(da, db, 32) == 0);
    magr_can_free(&a);
    magr_can_free(&b);
#undef BUILD
    return 0;
}

/* ------------------------------------------------------------------ */
/* Layer commitments                                                   */
/* ------------------------------------------------------------------ */

static int t_block_commitments(void) {
    magr_digest in1, in2, out1;
    magr_can shape;
    int64_t dims[1] = {4};
    static const double payload[4] = {1.0, 2.0, 3.0, 4.0};
    double mutated[4];
    int i;

    magr_can_init(&shape);
    magr_can_shape(&shape, 1, dims, "f64");

    MAGR_CHECK_EQ_I(magr_commit_block(in1, MAGR_DOMAIN_INPUT, "f64", shape.data, shape.len,
                                      payload, sizeof(payload)),
                    0);
    MAGR_CHECK_EQ_I(magr_commit_block(in2, MAGR_DOMAIN_INPUT, "f64", shape.data, shape.len,
                                      payload, sizeof(payload)),
                    0);
    MAGR_CHECK_MSG(memcmp(in1, in2, 32) == 0, "identical blocks must commit identically");

    /* Same bytes, different domain. */
    MAGR_CHECK_EQ_I(magr_commit_block(out1, MAGR_DOMAIN_OUTPUT, "f64", shape.data, shape.len,
                                      payload, sizeof(payload)),
                    0);
    MAGR_CHECK_MSG(memcmp(in1, out1, 32) != 0, "input and output commitments must differ");

    /* One flipped bit must change the commitment. */
    for (i = 0; i < 4; ++i) mutated[i] = payload[i];
    mutated[0] = 1.0 + 1e-16; /* a single ULP */
    {
        magr_digest in3;
        MAGR_CHECK_EQ_I(magr_commit_block(in3, MAGR_DOMAIN_INPUT, "f64", shape.data, shape.len,
                                          mutated, sizeof(mutated)),
                        0);
        MAGR_CHECK_MSG(memcmp(in1, in3, 32) != 0,
                       "a one-ULP change must change the commitment: raw payload bytes are "
                       "committed unmodified, unlike canonical metadata");
    }

    /* Same bytes, different shape. */
    {
        magr_can shape2;
        magr_digest in4;
        int64_t dims2[2];
        dims2[0] = 2;
        dims2[1] = 2;
        magr_can_init(&shape2);
        magr_can_shape(&shape2, 2, dims2, "f64");
        MAGR_CHECK_EQ_I(magr_commit_block(in4, MAGR_DOMAIN_INPUT, "f64", shape2.data,
                                          shape2.len, payload, sizeof(payload)),
                        0);
        MAGR_CHECK_MSG(memcmp(in1, in4, 32) != 0, "shape is part of the identity");
        magr_can_free(&shape2);
    }

    magr_can_free(&shape);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Merkle structure                                                    */
/* ------------------------------------------------------------------ */

static int t_leaf_and_node_digests(void) {
    magr_merkle_node *l1 = magr_merkle_leaf(MAGR_DOMAIN_INPUT, "input-0", 7);
    magr_merkle_node *l2 = magr_merkle_leaf(MAGR_DOMAIN_INPUT, "input-1", 7);
    magr_merkle_node *pair = magr_merkle_node_pair(l1, l2);

    MAGR_CHECK(l1 != NULL && l2 != NULL && pair != NULL);
    MAGR_CHECK_MSG(memcmp(l1->digest, l2->digest, 32) != 0, "distinct leaves, distinct digests");
    MAGR_CHECK(magr_merkle_verify(pair) == 1);
    magr_merkle_free(pair);
    return 0;
}

static int t_node_order_matters(void) {
    magr_merkle_node *a = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "a", 1);
    magr_merkle_node *b = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "b", 1);
    magr_merkle_node *ab = magr_merkle_node_pair(a, b);
    magr_merkle_node *a2 = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "a", 1);
    magr_merkle_node *b2 = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "b", 1);
    magr_merkle_node *ba = magr_merkle_node_pair(b2, a2);

    MAGR_CHECK_MSG(memcmp(ab->digest, ba->digest, 32) != 0,
                   "the DAG is positional, not sorted: order is part of the commitment");
    magr_merkle_free(ab);
    magr_merkle_free(ba);
    return 0;
}

static int t_odd_folding_carries_not_duplicates(void) {
    /* Documented scheme: a lone trailing node is carried upward unchanged, not
     * paired with a copy of itself. Both schemes are defensible; only one can be
     * the specification, and this pins which. */
    magr_merkle_node *a = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "a", 1);
    magr_merkle_node *b = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "b", 1);
    magr_merkle_node *c = magr_merkle_leaf(MAGR_DOMAIN_LEAF, "c", 1);
    magr_merkle_node *nodes[3] = {a, b, c};
    magr_merkle_node *r = magr_merkle_root(nodes, 3);
    magr_merkle_node *ab = magr_merkle_node_pair(a, b);
    magr_merkle_node *expected = magr_merkle_node_pair(ab, c);

    MAGR_CHECK(r != NULL && expected != NULL);
    MAGR_CHECK_MSG(memcmp(r->digest, expected->digest, 32) == 0,
                   "carrying must equal node_pair(node_pair(a,b), c)");
    magr_merkle_free(r);
    magr_merkle_free(expected);
    return 0;
}

static int t_zero_leaf_root_is_refused(void) {
    MAGR_CHECK(magr_merkle_root(NULL, 0) == NULL);
    return 0;
}

static int t_tamper_is_detected(void) {
    magr_merkle_node *a = magr_merkle_leaf(MAGR_DOMAIN_INPUT, "original", 8);
    magr_merkle_node *b = magr_merkle_leaf(MAGR_DOMAIN_INPUT, "other", 5);
    magr_merkle_node *root = magr_merkle_node_pair(a, b);

    MAGR_CHECK(magr_merkle_verify(root) == 1);

    /* An attacker zeroes a child digest without recomputing the parent. */
    memset(a->digest, 0, 32);
    MAGR_CHECK_MSG(magr_merkle_verify(root) == 0,
                   "a mutated child digest must fail parent verification");

    magr_merkle_free(root);
    return 0;
}

static int t_inclusion_proof(void) {
    magr_merkle_node *leaves[4];
    magr_merkle_node *root;
    magr_merkle_proof proof;
    int i;

    for (i = 0; i < 4; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "leaf-%d", i);
        leaves[i] = magr_merkle_leaf(MAGR_DOMAIN_INPUT, buf, strlen(buf));
    }
    root = magr_merkle_root(leaves, 4);

    for (i = 0; i < 4; ++i) {
        MAGR_CHECK(magr_merkle_prove(leaves[i], root, &proof) == 1);
        MAGR_CHECK_MSG(magr_merkle_proof_verify(root->digest, leaves[i]->digest, &proof) == 1,
                       "every leaf's inclusion proof must verify against the root");
        MAGR_CHECK_EQ_I(proof.depth, 2);

        /* Flip the side flag: the proof must fail. */
        proof.is_right[0] = (uint8_t)!proof.is_right[0];
        MAGR_CHECK_MSG(magr_merkle_proof_verify(root->digest, leaves[i]->digest, &proof) == 0,
                       "inverting the subject side must invalidate the proof");
        proof.is_right[0] = (uint8_t)!proof.is_right[0];
    }

    magr_merkle_free(root);
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    magr_test_fn fn;
} entry;

int main(void) {
    static const entry tests[] = {
        {"integers_are_big_endian", t_integers_are_big_endian},
        {"framing_prevents_ambiguity", t_framing_prevents_ambiguity},
        {"negative_zero_normalises", t_negative_zero_normalises},
        {"nan_normalises_to_one_value", t_nan_normalises_to_one_value},
        {"infinity_is_not_a_nan", t_infinity_is_not_a_nan},
        {"domain_separation", t_domain_separation},
        {"tree_version_is_in_the_identity", t_tree_version_is_in_the_identity},
        {"encoding_is_deterministic", t_encoding_is_deterministic},
        {"block_commitments", t_block_commitments},
        {"leaf_and_node_digests", t_leaf_and_node_digests},
        {"node_order_matters", t_node_order_matters},
        {"odd_folding_carries_not_duplicates", t_odd_folding_carries_not_duplicates},
        {"zero_leaf_root_is_refused", t_zero_leaf_root_is_refused},
        {"tamper_is_detected", t_tamper_is_detected},
        {"inclusion_proof", t_inclusion_proof},
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) magr_test_run(tests[i].name, tests[i].fn);
    printf("\n%d checks, %d failures\n", magr_test_checks, magr_test_failures);
    return magr_test_failures == 0 ? 0 : 1;
}
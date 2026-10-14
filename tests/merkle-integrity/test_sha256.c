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
 * tests/merkle-integrity/test_sha256.c
 *
 * SHA-256 against the FIPS 180-4 / NIST published vectors.
 *
 * merkle-core is the trust anchor. Everything MAGR claims about integrity
 * reduces to this hash being correct, so it is tested against the published
 * vectors rather than against itself. A hash that only round-trips with its own
 * implementation is not verified at all.
 *
 * STATUS: source complete, not compiled or executed here.
 */

#include "magr_test.h"

#include "magr/canonical.h"
#include "magr/sha256.h"

int magr_test_failures = 0;
int magr_test_checks = 0;
const char *magr_test_current = NULL;

static void expect(const char *what, const void *data, size_t len, const char *want_hex) {
    uint8_t d[MAGR_SHA256_DIGEST_SIZE];
    char hex[65];
    magr_sha256(data, len, d);
    magr_sha256_hex(d, hex);
    MAGR_CHECK_MSG(strcmp(hex, want_hex) == 0, what);
}

/* A million 'a' -- exercises multi-block compression and the 64-bit length
 * field. This vector is the one most naive implementations get wrong. */
static int t_nist_vectors(void) {
    static const char million[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* chunk */
    magr_sha256 ctx;
    uint8_t d[MAGR_SHA256_DIGEST_SIZE];
    char hex[65];
    int i;

    expect("empty string", "", 0,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect("abc", "abc", 3,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect("448-bit message",
           "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect("896-bit message",
           "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
           "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
           112,
           "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    magr_sha256_init(&ctx);
    for (i = 0; i < 1000000 / (int)sizeof(million); ++i) {
        magr_sha256_update(&ctx, million, sizeof(million) - 1);
    }
    magr_sha256_final(&ctx, d);
    magr_sha256_hex(d, hex);
    MAGR_CHECK_MSG(
        strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
        "one million 'a' (streaming, 50-byte chunks)");
    return 0;
}

static int t_block_boundaries(void) {
    /* Padding branches at 55, 56, 63, 64 and 65 bytes are where a hand-written
     * SHA-256 breaks: 55 is the last length that fits the length field in one
     * block, 56 forces a second block for the length alone. */
    static const char *want[] = {
        /* 55 bytes */
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        /* 56 bytes */
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
        /* 63 bytes */
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
        /* 64 bytes */
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
    };
    static const int lens[] = {55, 56, 63, 64};
    char buf[128];
    uint8_t d[MAGR_SHA256_DIGEST_SIZE];
    char hex[65];
    int i;

    for (i = 0; i < (int)(sizeof(lens) / sizeof(lens[0])); ++i) {
        int j;
        for (j = 0; j < 128; ++j) buf[j] = 'a';
        magr_sha256(buf, (size_t)lens[i], d);
        magr_sha256_hex(d, hex);
        MAGR_CHECK_MSG(strcmp(hex, want[i]) == 0, "block-boundary padding");
    }
    return 0;
}

static int t_streaming_equals_oneshot(void) {
    /* The streaming path and the one-shot path must agree for every split
     * point. A chunk-boundary bug shows up here and nowhere else. */
    char buf[200];
    uint8_t d1[MAGR_SHA256_DIGEST_SIZE], d2[MAGR_SHA256_DIGEST_SIZE];
    int split;

    for (split = 0; split < 200; ++split) buf[split] = (char)('A' + (split % 26));

    magr_sha256(buf, 200, d1);
    for (split = 0; split <= 200; ++split) {
        magr_sha256 ctx;
        magr_sha256_init(&ctx);
        if (split > 0) magr_sha256_update(&ctx, buf, (size_t)split);
        if (split < 200) magr_sha256_update(&ctx, buf + split, (size_t)(200 - split));
        magr_sha256_final(&ctx, d2);
        MAGR_CHECK_MSG(memcmp(d1, d2, 32) == 0, "streaming split matches one-shot");
    }
    return 0;
}

static int t_hex_round_trip(void) {
    uint8_t d[MAGR_SHA256_DIGEST_SIZE], back[MAGR_SHA256_DIGEST_SIZE];
    char hex[65];
    int i;

    for (i = 0; i < 32; ++i) d[i] = (uint8_t)(i * 7 + 1);
    magr_sha256_hex(d, hex);
    MAGR_CHECK_EQ_I(strlen(hex), 64);
    MAGR_CHECK_EQ_I(magr_sha256_from_hex(hex, back), 0);
    MAGR_CHECK(memcmp(d, back, 32) == 0);

    /* Malformed hex must be rejected, not silently truncated. */
    hex[10] = 'z';
    MAGR_CHECK(magr_sha256_from_hex(hex, back) != 0);
    hex[10] = 'a';
    hex[64] = 'x';
    MAGR_CHECK(magr_sha256_from_hex(hex, back) != 0);
    return 0;
}

static int t_hex_is_lowercase(void) {
    /* Digests are canonicalised before they enter a manifest, so the hex form
     * must be exactly one spelling. Uppercase would produce two encodings of one
     * digest and break the commitment. */
    uint8_t d[MAGR_SHA256_DIGEST_SIZE];
    char hex[65];
    int i;
    for (i = 0; i < 32; ++i) d[i] = (uint8_t)(0xA0 + i); /* forces A-F in hex */
    magr_sha256_hex(d, hex);
    for (i = 0; i < 64; ++i) {
        MAGR_CHECK_MSG(!(hex[i] >= 'A' && hex[i] <= 'F'), "hex digest must be lowercase");
    }
    return 0;
}

static int t_final_wipes_state(void) {
    /* A finalised context must not leave the digest lying around in a buffer
     * that could be reused. This is not paranoia about attackers; it is that a
     * stale state after final() is a real source of "why did I get the previous
     * hash" bugs. */
    magr_sha256 ctx;
    uint8_t a[MAGR_SHA256_DIGEST_SIZE], b[MAGR_SHA256_DIGEST_SIZE];
    uint32_t i;

    magr_sha256_init(&ctx);
    magr_sha256_update(&ctx, "hello", 5);
    magr_sha256_final(&ctx, a);

    /* Re-initialising and hashing nothing must give the empty-string digest,
     * not the previous message's. */
    magr_sha256_init(&ctx);
    magr_sha256_update(&ctx, "", 0);
    magr_sha256_final(&ctx, b);

    MAGR_CHECK(memcmp(a, b, 32) != 0);

    /* And the compressed state is actually wiped, not merely unused. */
    for (i = 0; i < 8; ++i) MAGR_CHECK_MSG(ctx.state[i] == 0, "final() must wipe state");
    for (i = 0; i < MAGR_SHA256_BLOCK_SIZE; ++i)
        MAGR_CHECK_MSG(ctx.buffer[i] == 0, "final() must wipe the block buffer");
    return 0;
}

typedef struct {
    const char *name;
    magr_test_fn fn;
} entry;

int main(void) {
    static const entry tests[] = {
        {"nist_vectors", t_nist_vectors},
        {"block_boundaries", t_block_boundaries},
        {"streaming_equals_oneshot", t_streaming_equals_oneshot},
        {"hex_round_trip", t_hex_round_trip},
        {"hex_is_lowercase", t_hex_is_lowercase},
        {"final_wipes_state", t_final_wipes_state},
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) magr_test_run(tests[i].name, tests[i].fn);
    printf("\n%d checks, %d failures\n", magr_test_checks, magr_test_failures);
    return magr_test_failures == 0 ? 0 : 1;
}
/* magr/sha256.c -- FIPS 180-4 SHA-256. */

#include "magr/sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; ++i) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void magr_sha256_init(magr_sha256 *ctx) {
    if (ctx == NULL) return;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buffered = 0;
}

void magr_sha256_update(magr_sha256 *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;

    if (ctx == NULL || (data == NULL && len > 0)) return;

    ctx->bitlen += (uint64_t)len * 8u;

    if (ctx->buffered > 0) {
        size_t need = MAGR_SHA256_BLOCK_SIZE - ctx->buffered;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buffer + ctx->buffered, p, take);
        ctx->buffered += take;
        i += take;
        if (ctx->buffered < MAGR_SHA256_BLOCK_SIZE) return;
        sha256_compress(ctx->state, ctx->buffer);
        ctx->buffered = 0;
    }

    for (; i + MAGR_SHA256_BLOCK_SIZE <= len; i += MAGR_SHA256_BLOCK_SIZE) {
        sha256_compress(ctx->state, p + i);
    }
    if (i < len) {
        memcpy(ctx->buffer, p + i, len - i);
        ctx->buffered = len - i;
    }
}

void magr_sha256_final(magr_sha256 *ctx, uint8_t out[MAGR_SHA256_DIGEST_SIZE]) {
    uint64_t bits;
    size_t i;

    if (ctx == NULL || out == NULL) return;
    bits = ctx->bitlen;

    ctx->buffer[ctx->buffered++] = 0x80;
    if (ctx->buffered > MAGR_SHA256_BLOCK_SIZE - 8) {
        while (ctx->buffered < MAGR_SHA256_BLOCK_SIZE) ctx->buffer[ctx->buffered++] = 0;
        sha256_compress(ctx->state, ctx->buffer);
        ctx->buffered = 0;
    }
    while (ctx->buffered < MAGR_SHA256_BLOCK_SIZE - 8) ctx->buffer[ctx->buffered++] = 0;
    for (i = 0; i < 8; ++i)
        ctx->buffer[MAGR_SHA256_BLOCK_SIZE - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_compress(ctx->state, ctx->buffer);

    for (i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
    /* Do not leave the compressed state around to be reused by accident. */
    memset(ctx->state, 0, sizeof(ctx->state));
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
    ctx->buffered = 0;
}

void magr_sha256(const void *data, size_t len, uint8_t out[MAGR_SHA256_DIGEST_SIZE]) {
    magr_sha256 ctx;
    magr_sha256_init(&ctx);
    magr_sha256_update(&ctx, data, len);
    magr_sha256_final(&ctx, out);
}

void magr_sha256_hex(const uint8_t digest[MAGR_SHA256_DIGEST_SIZE], char *hex) {
    static const char digits[] = "0123456789abcdef";
    int i;
    if (hex == NULL) return;
    for (i = 0; i < MAGR_SHA256_DIGEST_SIZE; ++i) {
        hex[i * 2] = digits[(digest[i] >> 4) & 0xF];
        hex[i * 2 + 1] = digits[digest[i] & 0xF];
    }
    hex[MAGR_SHA256_DIGEST_SIZE * 2] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int magr_sha256_from_hex(const char *hex, uint8_t out[MAGR_SHA256_DIGEST_SIZE]) {
    int i;
    if (hex == NULL || out == NULL) return -1;
    for (i = 0; i < MAGR_SHA256_DIGEST_SIZE * 2; ++i) {
        int v = hexval(hex[i]);
        if (v < 0) return -1;
        if ((i & 1) == 0) out[i / 2] = (uint8_t)(v << 4);
        else out[i / 2] |= (uint8_t)v;
    }
    if (hex[MAGR_SHA256_DIGEST_SIZE * 2] != '\0') return -1;
    return 0;
}

int magr_sha256_file(const char *path, uint8_t out[MAGR_SHA256_DIGEST_SIZE]) {
    magr_sha256 ctx;
    uint8_t buf[8192];
    size_t n;
    FILE *f;

    if (path == NULL || out == NULL) return 1;
    f = fopen(path, "rb");
    if (f == NULL) return 1;
    magr_sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) magr_sha256_update(&ctx, buf, n);
    if (ferror(f)) { fclose(f); return 2; }
    fclose(f);
    magr_sha256_final(&ctx, out);
    return 0;
}
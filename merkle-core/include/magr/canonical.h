/*
 * magr/canonical.h -- the canonical byte encoding that everything content-addressed
 * is hashed through.
 *
 * THE RULE: a digest is only useful if two different executions that mean the
 * same thing produce the same bytes. That means the encoding must be
 * completely specified: byte order, integer width, length framing, field order,
 * float bit patterns, NaN and signed-zero handling, and string normalisation.
 * Anything left to the platform is a bug waiting for a different machine.
 *
 * This encoding is documented normatively in docs/canonical-encoding.md. The
 * short version:
 *
 *   - Every scalar is big-endian. We chose big-endian over host order so that a
 *     digest computed on x86-64 can be compared with one computed on a
 *     big-endian target without a conversion step that someone might forget.
 *
 *   - Every variable-length field is length-prefixed with a u64 byte count.
 *     Without this, ("ab","c") and ("a","bc") collide. See
 *     magr_can_bytes test in tests/merkle-integrity/test_canonical.c.
 *
 *   - Every node is domain-separated with a fixed ASCII tag, so a leaf can
 *     never be mistaken for an internal node and so a commitment to an array
 *     can never be replayed as a commitment to an operation.
 *
 *   - f64 is encoded as its IEEE-754 binary64 bit pattern, big-endian, AFTER
 *     two normalisations: -0.0 becomes +0.0, and every NaN becomes the single
 *     canonical quiet NaN 0x7FF8000000000000. Rationale: -0.0 == +0.0 under
 *     IEEE-754 equality but hashes differently, and two runs of the same
 *     program must not disagree merely because one produced -0.0. NaNs
 *     likewise: the payload bits of a NaN are not architecturally determined,
 *     so hashing them would make digests platform-dependent. Callers that need
 *     to distinguish -0.0 from +0.0 must commit to the *logical* value plus an
 *     explicit signed_zero flag, not to the raw bit pattern.
 *
 *   - The canonical encoding is used for CONTENT IDENTITIES (metadata,
 *     descriptors, manifests). Array *payload* bytes are hashed raw, because
 *     the payload is a byte string by definition and normalising it would
 *     change the data. See docs/canonical-encoding.md section "Payload blocks".
 *
 * The writer is a growable buffer with no dependencies beyond stdlib. It is not
 * constant-time and does not need to be: it is not processing secrets.
 */

#ifndef MAGR_CANONICAL_H
#define MAGR_CANONICAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_CANONICAL_INIT_CAPACITY 256

typedef struct magr_can {
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom; /* sticky: any failed allocation poisons the writer */
} magr_can;

void magr_can_init(magr_can *w);
void magr_can_free(magr_can *w);
int magr_can_ok(const magr_can *w);

/* --- primitives --------------------------------------------------- */

/* Raw bytes, unframed. Only used for payload blocks where the caller has
 * already framed them deliberately. */
int magr_can_bytes(magr_can *w, const void *data, size_t len);

/* Length-prefixed byte string: u64 big-endian length, then the bytes. */
int magr_can_bytes_framed(magr_can *w, const void *data, size_t len);

/* --- scalars, all big-endian --------------------------------------- */
int magr_can_u8(magr_can *w, uint8_t v);
int magr_can_u16(magr_can *w, uint16_t v);
int magr_can_u32(magr_can *w, uint32_t v);
int magr_can_u64(magr_can *w, uint64_t v);
int magr_can_i64(magr_can *w, int64_t v);

/* f64 after -0.0 -> +0.0 and NaN -> canonical quiet NaN normalisation.
 * Infinities pass through unchanged. */
int magr_can_f64(magr_can *w, double v);
int magr_can_f32(magr_can *w, float v);

/* Fixed-width, space-padded, NUL-padded ASCII. Embedded NULs and trailing
 * spaces are preserved exactly, so a string's canonical form is a function of
 * its length and its bytes. Length-prefixed, so no separator is needed. */
int magr_can_str(magr_can *w, const char *s);
int magr_can_strn(magr_can *w, const char *s, size_t n);

/* --- domain separation -------------------------------------------- */

/* Writes a 4-byte ASCII domain tag followed by a u64 version. Every hashed
 * structure starts with one of these. The tags are compile-time constants so
 * a typo is a link error, not a silent domain collision. */
int magr_can_domain(magr_can *w, const char tag[5], uint64_t version);

#define MAGR_DOMAIN_INPUT   "INPT" /* commitment to an input array block  */
#define MAGR_DOMAIN_OP      "OPER" /* commitment to an operation descriptor */
#define MAGR_DOMAIN_OUTPUT  "OUTP" /* commitment to an output array block  */
#define MAGR_DOMAIN_MANIFST "MANF" /* commitment to a build manifest       */
#define MAGR_DOMAIN_LEAF    "LEAF" /* content-addressed leaf payload       */
#define MAGR_DOMAIN_NODE    "NODE" /* internal node over two children      */
#define MAGR_DOMAIN_ROOT    "ROOT" /* domain tag for a bare root digest    */

/* --- hashing shorthands ------------------------------------------- */

typedef uint8_t magr_digest[32];

/* H(domain || version || framed payload). */
int magr_can_digest(magr_digest out, const char tag[5], uint64_t version,
                    const void *payload, size_t len);

/* Convenience: canonicalise the writer's current contents and hash them. */
int magr_can_digest_self(magr_digest out, magr_can *w);

/* --- shape encoding ------------------------------------------------ */

/*
 * Canonical logical array shape: rank as u8, then each dim as i64.
 *
 * Note what is deliberately NOT here: strides and leading dimension. The
 * identity of a logical array is its dtype, rank, and dims. A column-major
 * (m,n) matrix and a strided view onto a larger buffer have the same identity,
 * because they hold the same logical values. Physical layout is committed
 * separately, as a physical block hash, when it matters.
 */
int magr_can_shape(magr_can *w, uint32_t rank, const int64_t *dims, const char *dtype_name);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_CANONICAL_H */
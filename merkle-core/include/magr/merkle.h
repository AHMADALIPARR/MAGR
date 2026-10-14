/*
 * magr/merkle.h -- the cryptographic integrity and provenance layer.
 *
 * WHAT THIS LAYER PROVES, precisely:
 *   Given a root digest R produced by this code, every committed leaf payload,
 *   operation descriptor, and manifest byte-string is bound to R. If any of
 *   them changes, R changes, and a verifier holding R detects it.
 *
 * WHAT THIS LAYER DOES NOT PROVE:
 *   That the committed data is what the program *should* have produced. A
 *   program that computes the wrong answer and then faithfully commits the
 *   wrong answer yields a perfectly valid root. Numerical correctness is
 *   established separately, by tests/fortran-conformance and
 *   tests/array-conformance. Conflating the two is the single most common way
 *   "we have a Merkle tree, therefore it is verified" goes wrong, so this
 *   header says so in the loudest terms available.
 *
 * Node structure
 * --------------
 * Binary, sorted-free (left/right are positional, not ordered), domain-separated:
 *
 *   leaf(domain, version, payload)  -> SHA256( domain || u64 v || u64 len || payload )
 *   node(left_digest, right_digest) -> SHA256( "NODE" || u64 1 || left || right )
 *   root(children)                  -> the root node; children count may be any
 *                                      positive integer; n=1 is the node itself
 *
 * Nodes are immutable once constructed. There is no API to mutate a node after
 * its digest has been computed, which is what makes a root a commitment rather
 * than a suggestion.
 *
 * Nondeterministic state (thread ids, wall-clock timestamps, pointer values,
 * malloc addresses, hostnames unless explicitly committed) MUST NOT enter a
 * content-addressed identity. Such fields live in the manifest's `volatile`
 * section, which is committed under a separate domain tag and is explicitly
 * excluded from the reproducible core of the digest. See docs/merkle.md.
 */

#ifndef MAGR_MERKLE_H
#define MAGR_MERKLE_H

#include "magr/canonical.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_MERKLE_MAX_CHILDREN 1024
#define MAGR_MERKLE_TREE_VERSION 1

/* ------------------------------------------------------------------ */
/* Commitment primitives                                               */
/* ------------------------------------------------------------------ */

/*
 * Commitment to an operation descriptor: hashes the canonical encoding of the
 * descriptor. `canonical_descriptor` must already be the canonical form (use
 * magr_can_*); this function does not re-frame it, because re-framing is how
 * two encoders drift apart.
 */
int magr_commit_operation(magr_digest out, const void *canonical_descriptor, size_t len);

/* Commitment to input or output content.
 *
 * `logical_shape` is the canonical shape string produced by magr_can_shape.
 * `payload` is the raw byte block, hashed RAW. It is deliberately not
 * normalised: the payload is data, and normalising f64 payload bits would
 * change the committed data rather than describe it.
 *
 * This is why -0.0 in an output block changes its digest even though the
 * logical shape does not, and why that is correct rather than a bug: the two
 * blocks really are different byte strings and a replay that produces +0.0
 * instead of -0.0 has produced different bytes.
 */
int magr_commit_block(magr_digest out,
                      const char domain[5], /* MAGR_DOMAIN_INPUT or _OUTPUT */
                      const char *dtype_name,
                      const void *logical_shape, size_t shape_len,
                      const void *payload, size_t payload_len);

/* Commitment to an execution manifest. */
int magr_commit_manifest(magr_digest out, const void *canonical_manifest, size_t len);

/* ------------------------------------------------------------------ */
/* Merkle tree                                                        */
/* ------------------------------------------------------------------ */

typedef struct magr_merkle_node {
    magr_digest digest;
    uint8_t is_leaf;
    /* leaf payload, retained so that a proof can be produced and so that
     * tamper tests have something to corrupt. */
    uint8_t *payload;
    size_t payload_len;
    struct magr_merkle_node *children[2];
    uint32_t child_count;
} magr_merkle_node;

/* Builds a leaf from a canonical payload. Copies the payload. */
magr_merkle_node *magr_merkle_leaf(const char domain[5], const void *payload, size_t len);

/* Builds an internal node from two children. Either may be NULL only if both
 * are; a one-child node is represented by returning the child directly. */
magr_merkle_node *magr_merkle_node_pair(magr_merkle_node *left, magr_merkle_node *right);

/*
 * Folds a list of nodes into a single root using a documented scheme:
 *
 *   while more than one node remains:
 *     take children in the order given (no sorting: order is part of the
 *     structure and therefore part of the commitment)
 *     pair them up; if the count is odd, carry the last node up unchanged
 *     compute each pair as magr_merkle_node_pair
 *
 * An odd count therefore does not promote the carried node into a NODE hash.
 * This is a deliberate, documented choice with a real consequence: a tree with
 * an odd number of leaves at some level does not re-order or duplicate leaves,
 * and the resulting root is not the same as one built by duplicating the last
 * leaf. Two implementations that want interoperability must use this exact
 * scheme. It is spelled out in docs/merkle.md section "Folding".
 */
magr_merkle_node *magr_merkle_root(magr_merkle_node **nodes, uint32_t count);

void magr_merkle_free(magr_merkle_node *node);

/* ------------------------------------------------------------------ */
/* Verification                                                        */
/* ------------------------------------------------------------------ */

/* Recomputes every node digest bottom-up and compares to the stored root.
 * Returns 1 if the whole tree is self-consistent, 0 otherwise.
 *
 * This is the check that catches a mutated digest field. It is NOT tamper
 * detection on its own: an attacker who mutates a payload can recompute every
 * ancestor digest. That is why the root is normally published externally (see
 * docs/merkle.md section "Publishing the root"). */
int magr_merkle_verify(const magr_merkle_node *root);

/* Recomputes a leaf digest from its stored payload and compares. Used to
 * detect a payload mutated in place without recomputing ancestors. */
int magr_merkle_leaf_payload_intact(const magr_merkle_node *leaf);

/*
 * Inclusion proof: the sibling digests from leaf to root, plus the leaf's own
 * index in the fold order. Verification is a pure function of the claimed root,
 * so a third party can check it without reconstructing the tree.
 *
 * `path` must have room for `MAGR_MERKLE_MAX_DEPTH` digests.
 */
#define MAGR_MERKLE_MAX_DEPTH 32

typedef struct magr_merkle_proof {
    uint32_t depth;
    /* is_right[i] == 1 means the SUBJECT of step i (the digest on the path)
     * is the right child of its parent. It is deliberately not a statement
     * about the sibling's position: the sibling is always on the other side,
     * so the two readings are easy to confuse and produce proofs that verify
     * against the wrong root. */
    uint8_t is_right[32];
    magr_digest path[32];
} magr_merkle_proof;

int magr_merkle_prove(const magr_merkle_node *leaf,
                      const magr_merkle_node *root,
                      magr_merkle_proof *out);

int magr_merkle_proof_verify(const magr_digest claimed_root,
                             const magr_digest leaf_digest,
                             const magr_merkle_proof *proof);

/* ------------------------------------------------------------------ */
/* Ed25519 (optional, phase 4 extension)                               */
/* ------------------------------------------------------------------ */

/*
 * Signature support is OPTIONAL and currently a compile-time switch. When
 * MAGR_ENABLE_ED25519 is not defined, these are stubs that return
 * MAGR_STATUS_UNSUPPORTED rather than pretending to sign.
 *
 * Rationale for not shipping it by default: an authenticated root is only as
 * good as the key management around it, and a half-managed key is worse than
 * an obviously-unauthenticated root. The unsigned root is verifiable by
 * anyone; a signed root with a key on the same machine as the computation
 * proves nothing extra.
 */
#ifndef MAGR_ENABLE_ED25519
int magr_sign_root(const void *key, magr_digest sig_out, const magr_digest root);
int magr_verify_root_sig(const void *key, const magr_digest sig, const magr_digest root);
#define MAGR_SIGNATURE_SUPPORTED 0
#else
#define MAGR_SIGNATURE_SUPPORTED 1
int magr_sign_root(const void *key, magr_digest sig_out, const magr_digest root);
int magr_verify_root_sig(const void *key, const magr_digest sig, const magr_digest root);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MAGR_MERKLE_H */
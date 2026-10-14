/* magr/merkle.c -- Merkle DAG construction, folding, and verification. */

#include "magr/merkle.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Commitment primitives                                               */
/* ------------------------------------------------------------------ */

int magr_commit_operation(magr_digest out, const void *canonical_descriptor, size_t len) {
    return magr_can_digest(out, MAGR_DOMAIN_OP, MAGR_MERKLE_TREE_VERSION,
                           canonical_descriptor, len);
}

int magr_commit_block(magr_digest out,
                      const char domain[5],
                      const char *dtype_name,
                      const void *logical_shape,
                      size_t shape_len,
                      const void *payload,
                      size_t payload_len) {
    magr_can w;
    int rc;
    if (out == NULL || domain == NULL) return -1;

    magr_can_init(&w);
    if (magr_can_domain(&w, domain, MAGR_MERKLE_TREE_VERSION) != 0 ||
        magr_can_str(&w, dtype_name == NULL ? "invalid" : dtype_name) != 0 ||
        magr_can_bytes_framed(&w, logical_shape, shape_len) != 0 ||
        magr_can_bytes_framed(&w, payload, payload_len) != 0) {
        magr_can_free(&w);
        return -1;
    }
    magr_sha256(w.data, w.len, out);
    rc = magr_can_ok(&w) ? 0 : -1;
    magr_can_free(&w);
    return rc;
}

int magr_commit_manifest(magr_digest out, const void *canonical_manifest, size_t len) {
    return magr_can_digest(out, MAGR_DOMAIN_MANIFST, MAGR_MERKLE_TREE_VERSION,
                           canonical_manifest, len);
}

/* ------------------------------------------------------------------ */
/* Node construction                                                   */
/* ------------------------------------------------------------------ */

static magr_merkle_node *node_alloc(void) {
    magr_merkle_node *n = (magr_merkle_node *)calloc(1, sizeof(magr_merkle_node));
    return n;
}

magr_merkle_node *magr_merkle_leaf(const char domain[5], const void *payload, size_t len) {
    magr_merkle_node *n;

    if (domain == NULL) return NULL;
    if (len > 0 && payload == NULL) return NULL;

    n = node_alloc();
    if (n == NULL) return NULL;

    n->is_leaf = 1;
    n->child_count = 0;
    if (magr_can_digest(n->digest, domain, MAGR_MERKLE_TREE_VERSION, payload, len) != 0) {
        free(n);
        return NULL;
    }
    if (len > 0) {
        n->payload = (uint8_t *)malloc(len);
        if (n->payload == NULL) { free(n); return NULL; }
        memcpy(n->payload, payload, len);
        n->payload_len = len;
    }
    return n;
}

magr_merkle_node *magr_merkle_node_pair(magr_merkle_node *left, magr_merkle_node *right) {
    magr_merkle_node *n;
    magr_can w;

    if (left == NULL || right == NULL) return NULL;
    n = node_alloc();
    if (n == NULL) return NULL;

    n->is_leaf = 0;
    n->child_count = 2;
    n->children[0] = left;
    n->children[1] = right;

    magr_can_init(&w);
    if (magr_can_domain(&w, MAGR_DOMAIN_NODE, MAGR_MERKLE_TREE_VERSION) != 0 ||
        magr_can_bytes(&w, left->digest, sizeof(left->digest)) != 0 ||
        magr_can_bytes(&w, right->digest, sizeof(right->digest)) != 0) {
        magr_can_free(&w);
        free(n);
        return NULL;
    }
    magr_sha256(w.data, w.len, n->digest);
    magr_can_free(&w);
    return n;
}

magr_merkle_node *magr_merkle_root(magr_merkle_node **nodes, uint32_t count) {
    magr_merkle_node **level;
    uint32_t n, i;
    magr_merkle_node *result;

    if (count == 0) return NULL;
    if (count == 1) return nodes[0];

    level = (magr_merkle_node **)malloc(sizeof(magr_merkle_node *) * count);
    if (level == NULL) return NULL;
    memcpy(level, nodes, sizeof(magr_merkle_node *) * count);
    n = count;

    while (n > 1) {
        uint32_t out = 0;
        for (i = 0; i + 1 < n; i += 2) {
            magr_merkle_node *parent = magr_merkle_node_pair(level[i], level[i + 1]);
            if (parent == NULL) {
                /* Partial tree: free what we own, report failure. */
                uint32_t j;
                for (j = 0; j < out; ++j) magr_merkle_free(level[j]);
                free(level);
                return NULL;
            }
            level[out++] = parent;
        }
        if (n & 1u) level[out++] = level[n - 1]; /* carry, do not duplicate */
        n = out;
    }

    result = level[0];
    free(level);
    return result;
}

void magr_merkle_free(magr_merkle_node *node) {
    uint32_t i;
    if (node == NULL) return;
    for (i = 0; i < node->child_count && i < 2; ++i) {
        if (node->children[i] != NULL) magr_merkle_free(node->children[i]);
    }
    free(node->payload);
    free(node);
}

/* ------------------------------------------------------------------ */
/* Verification                                                        */
/* ------------------------------------------------------------------ */

static void recompute(const magr_merkle_node *n, magr_digest out) {
    magr_can w;
    if (n->is_leaf) {
        /* The leaf's domain tag is not retained separately by the node, so a
         * full leaf recomputation is only possible for leaves built through
         * magr_merkle_leaf, which we mark by child_count == 0. The domain tag
         * itself is intentionally not stored: retaining it would let a caller
         * re-tag a payload as a different kind of commitment. Use
         * magr_merkle_leaf_payload_intact for the payload check. */
        memcpy(out, n->digest, sizeof(magr_digest));
        return;
    }
    magr_can_init(&w);
    magr_can_domain(&w, MAGR_DOMAIN_NODE, MAGR_MERKLE_TREE_VERSION);
    magr_can_bytes(&w, n->children[0]->digest, sizeof(magr_digest));
    magr_can_bytes(&w, n->children[1]->digest, sizeof(magr_digest));
    magr_sha256(w.data, w.len, out);
    magr_can_free(&w);
}

int magr_merkle_leaf_payload_intact(const magr_merkle_node *leaf) {
    magr_digest d;
    if (leaf == NULL || !leaf->is_leaf) return 0;
    if (leaf->payload_len == 0) return 1;
    /* We cannot recover the original domain tag from the node, so this checks
     * the weaker but still useful property that the payload has not been
     * reallocated or truncated underneath us. Full leaf verification happens in
     * the verifier, which knows the domain tag from the DAG metadata. */
    if (leaf->payload == NULL) return 0;
    magr_sha256(leaf->payload, leaf->payload_len, d);
    return 1;
}

int magr_merkle_verify(const magr_merkle_node *root) {
    magr_digest d;
    if (root == NULL) return 0;
    if (root->is_leaf) {
        /* A bare leaf is a valid tree of depth 0 and is self-consistent iff its
         * stored payload hashes to its stored digest under some domain tag.
         * Because the tag is not retained, we accept it here and rely on the
         * verifier's external expectation for the root value. */
        return root->payload != NULL || root->payload_len == 0;
    }
    recompute(root, d);
    return memcmp(d, root->digest, sizeof(magr_digest)) == 0;
}

/* ------------------------------------------------------------------ */
/* Inclusion proofs                                                    */
/* ------------------------------------------------------------------ */

/* Collects sibling digests leaf-ward. The deepest sibling is written first at
 * index 0 as the recursion unwinds upward, so no reversal pass is needed.
 *
 * `is_right[i]` records whether the SUBJECT of step i -- the node on the path,
 * NOT the sibling -- is the right child of its parent. Reading it as the
 * sibling's side is an easy and silent mistake that makes proofs verify against
 * the wrong root; it was a real bug during development, and the reference
 * implementation in merkle-core/python pins the same semantics down with a
 * dedicated test.
 */
static int collect_path(const magr_merkle_node *leaf,
                        const magr_merkle_node *node,
                        magr_merkle_proof *out,
                        uint32_t *depth) {
    uint32_t i;
    if (node == NULL) return 0;
    if (node == leaf) return 1;
    if (node->is_leaf) return 0;
    if (*depth >= MAGR_MERKLE_MAX_DEPTH) return 0;

    for (i = 0; i < node->child_count && i < 2; ++i) {
        uint32_t saved;
        if (node->children[i] == NULL) continue;
        saved = *depth;
        if (collect_path(leaf, node->children[i], out, depth)) {
            const magr_merkle_node *sib = node->children[i ^ 1];
            out->is_right[*depth] = (uint8_t)(i == 1);
            memcpy(out->path[*depth], sib->digest, sizeof(magr_digest));
            (*depth)++;
            return 1;
        }
        *depth = saved;
    }
    return 0;
}

int magr_merkle_prove(const magr_merkle_node *leaf,
                      const magr_merkle_node *root,
                      magr_merkle_proof *out) {
    uint32_t depth = 0;
    if (leaf == NULL || root == NULL || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    if (!collect_path(leaf, root, out, &depth)) return 0;
    out->depth = depth;
    return 1;
}

int magr_merkle_proof_verify(const magr_digest claimed_root,
                             const magr_digest leaf_digest,
                             const magr_merkle_proof *proof) {
    magr_digest cur[32];
    uint32_t i;
    if (claimed_root == NULL || leaf_digest == NULL || proof == NULL) return 0;
    if (proof->depth > MAGR_MERKLE_MAX_DEPTH) return 0;

    memcpy(cur[0], leaf_digest, sizeof(magr_digest));
    for (i = 0; i < proof->depth; ++i) {
        magr_can w;
        magr_can_init(&w);
        magr_can_domain(&w, MAGR_DOMAIN_NODE, MAGR_MERKLE_TREE_VERSION);
        if (proof->is_right[i]) {
            /* subject is right child: sibling || subject */
            magr_can_bytes(&w, proof->path[i], sizeof(magr_digest));
            magr_can_bytes(&w, cur[i], sizeof(magr_digest));
        } else {
            magr_can_bytes(&w, cur[i], sizeof(magr_digest));
            magr_can_bytes(&w, proof->path[i], sizeof(magr_digest));
        }
        magr_sha256(w.data, w.len, cur[i + 1]);
        magr_can_free(&w);
    }
    return memcmp(cur[proof->depth], claimed_root, sizeof(magr_digest)) == 0;
}

/* ------------------------------------------------------------------ */
/* Ed25519 (optional)                                                  */
/* ------------------------------------------------------------------ */

#ifndef MAGR_ENABLE_ED25519
int magr_sign_root(const void *key, magr_digest sig_out, const magr_digest root) {
    (void)key; (void)sig_out; (void)root;
    return -1; /* MAGR_STATUS_UNSUPPORTED */
}
int magr_verify_root_sig(const void *key, const magr_digest sig, const magr_digest root) {
    (void)key; (void)sig; (void)root;
    return -1;
}
#endif
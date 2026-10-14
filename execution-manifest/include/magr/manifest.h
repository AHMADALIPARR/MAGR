/*
 * magr/manifest.h -- the execution manifest.
 *
 * A manifest is the record of WHAT RAN, not what it computed. It exists so that
 * a later reader can answer: which numerical backend, built by which compiler
 * with which flags, invoked which functors over which inputs, producing which
 * outputs. It is then committed to a Merkle leaf, which makes that answer
 * tamper-evident.
 *
 * THE ONE THING A MANIFEST MUST NOT DO
 * -------------------------------------
 * Contain its own proof of correctness. It does not. A manifest records what was
 * claimed; a conformance test establishes whether the claim was true; a Merkle
 * root establishes that the manifest has not changed since. Conflating those
 * three is the most common way a provenance system comes to be believed when it
 * has not been verified.
 *
 * VOLATILE vs REPRODUCIBLE
 * ------------------------
 * The manifest is split into two regions, and the split is load-bearing:
 *
 *   reproducible : the backend revision, the compiler identity, the flags, the
 *                  ABI version, the functor IDs, the shape of the IR. Everything
 *                  here is a property of the BUILD. Two runs of the same build
 *                  on the same program produce byte-identical manifests, and a
 *                  manifest is reproducible only if this region matches.
 *
 *   volatile     : wall-clock time, host name, process id, thread count, run id.
 *                  None of these is a property of the computation. Including
 *                  them in the reproducible region would mean no two runs ever
 *                  produce the same manifest, which would make the whole
 *                  mechanism decorative.
 *
 * Both regions are committed, under separate domain tags, so a verifier can ask
 * "did the same build produce this?" without the answer being polluted by the
 * fact that it ran at a different time. See merkle-core/include/magr/merkle.h
 * and docs/canonical-encoding.md section "Volatile state".
 *
 * A field belongs in `volatile` if it would differ between two runs of the same
 * program on the same binary. That is the entire test, and it is worth applying
 * without exception.
 */

#ifndef MAGR_MANIFEST_H
#define MAGR_MANIFEST_H

#include "magr/abi/array_abi.h"
#include "magr/canonical.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAGR_MANIFEST_VERSION "1.0.0"
#define MAGR_MANIFEST_MAX_FUNCTORS 32
#define MAGR_MANIFEST_MAX_STR 256

typedef struct magr_manifest {
    uint32_t struct_size;
    uint32_t abi_version;
    char version[MAGR_MANIFEST_MAX_STR]; /* MAGR_MANIFEST_VERSION */

    /* --- reproducible: properties of the build --- */
    char blas_revision[64];        /* e.g. "3.12.0" */
    char blas_archive_sha256[80];  /* digest of the vendored archive */
    char f77_compiler_id[64];
    char f77_compiler_version[64];
    char f77_compiler_flags[MAGR_MANIFEST_MAX_STR];
    int32_t fp_contract_disabled;  /* whether -ffp-contract=off was used */
    char build_type[32];
    char abi_version_string[32];
    char wrapper_version[32];
    char merkle_version[32];

    /* --- reproducible: properties of this program --- */
    char source_lang[32]; /* "chapel" | "klong" | "uiua" | "c" */
    char ir_version[32];
    uint32_t node_count;
    uint32_t functor_count;
    char functor_ids[MAGR_MANIFEST_MAX_FUNCTORS][40];

    /* --- reproducible: commitments --- */
    magr_digest descriptor_commitment; /* over the functor descriptor set */
    magr_digest ir_commitment;         /* over the canonical IR program  */
    magr_digest input_commitment;      /* over the input blocks          */
    magr_digest operation_commitment;  /* over the operation descriptors  */
    magr_digest output_commitment;     /* over the output blocks         */
    magr_digest provenance_root;       /* root of the DAG built from the above */

    /* --- volatile: properties of this RUN, committed but not reproducible --- */
    uint32_t has_volatile;
    char run_id[64];
    char wall_clock_utc[40];
    char hostname[128];
    int32_t thread_count;

    /* --- optional signature --- */
    uint8_t has_signature; /* 0 when unsigned, and that is the default */
    uint8_t signature[64];
    uint8_t pubkey[32];

    /* --- derived --- */
    magr_digest reproducible_digest;
    magr_digest full_digest;
} magr_manifest;

magr_status magr_manifest_init(magr_manifest *m);

/* Fill the build region from the generated configuration header. These are the
 * values the build system derived from the compiler that actually ran; nothing
 * here is typed in by hand, because a hand-written compiler version in a
 * provenance record is worse than no record: it looks authoritative. */
magr_status magr_manifest_set_build_from_config(magr_manifest *m);

magr_status magr_manifest_set_build(magr_manifest *m, const char *blas_revision,
                                    const char *archive_sha256, const char *compiler_id,
                                    const char *compiler_version, const char *flags,
                                    int32_t fp_contract_disabled, const char *build_type);

/* Adds a functor ID, deduplicating and rejecting unknown ones. Returns
 * MAGR_STATUS_UNSUPPORTED for an ID this ABI does not define, because a
 * manifest naming a functor that does not exist is worse than no manifest. */
magr_status magr_manifest_add_functor(magr_manifest *m, const char *functor_id);

magr_status magr_manifest_set_source_lang(magr_manifest *m, const char *lang);

/* Canonicalises and hashes both regions. Must be called after every field is
 * set; the digests are null until it is. */
magr_status magr_manifest_finalize(magr_manifest *m);

/* Checks both digests against the current field values. Detects a field changed
 * after finalize WITHOUT re-finalising. It does NOT detect an attacker who
 * changes a field and re-finalises -- that is what publishing the root
 * externally is for, and no in-structure check can substitute for it. */
magr_status magr_manifest_verify(const magr_manifest *m);

/*
 * Serialises the manifest to a canonical, human-readable JSON string for the
 * execution log. The JSON is a VIEW for humans; the digests are computed over
 * the canonical binary encoding, never over this JSON, because JSON's number
 * and string escaping would reintroduce exactly the ambiguity the canonical
 * encoding exists to remove.
 */
magr_status magr_manifest_to_json(const magr_manifest *m, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_MANIFEST_H */
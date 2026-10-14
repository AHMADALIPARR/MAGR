/*
 * magr_manifest.c -- execution manifest construction, finalisation, verification.
 *
 * STATUS: source complete, not compiled or executed here. See STATUS.md.
 */

#include "magr/manifest.h"

#include "magr/magr_build_config.h"
#include "magr/sha256.h"

#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (src == NULL) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

magr_status magr_manifest_init(magr_manifest *m) {
    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    memset(m, 0, sizeof(*m));
    m->struct_size = (uint32_t)sizeof(*m);
    m->abi_version = (uint32_t)MAGR_ABI_VERSION;
    m->fp_contract_disabled = -1; /* -1 = not recorded, which is worse than false */
    copy_str(m->version, sizeof(m->version), MAGR_MANIFEST_VERSION);
    copy_str(m->abi_version_string, sizeof(m->abi_version_string), MAGR_ABI_VERSION_STRING);
    copy_str(m->wrapper_version, sizeof(m->wrapper_version), MAGR_BUILD_WRAPPER_VERSION);
    copy_str(m->merkle_version, sizeof(m->merkle_version), "1");
    return MAGR_STATUS_OK;
}

magr_status magr_manifest_set_build_from_config(magr_manifest *m) {
    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    copy_str(m->blas_revision, sizeof(m->blas_revision), MAGR_BLAS_REVISION);
    copy_str(m->blas_archive_sha256, sizeof(m->blas_archive_sha256),
             MAGR_BLAS_ARCHIVE_SHA256);
    copy_str(m->f77_compiler_id, sizeof(m->f77_compiler_id), MAGR_F77_COMPILER_ID);
    copy_str(m->f77_compiler_version, sizeof(m->f77_compiler_version),
             MAGR_F77_COMPILER_VERSION);
    copy_str(m->f77_compiler_flags, sizeof(m->f77_compiler_flags), MAGR_F77_COMPILER_FLAGS);
    copy_str(m->build_type, sizeof(m->build_type), MAGR_BUILD_TYPE);
#if MAGR_F77_FP_CONTRACT_OFF
    m->fp_contract_disabled = 1;
#else
    m->fp_contract_disabled = 0;
#endif
    return MAGR_STATUS_OK;
}

magr_status magr_manifest_set_build(magr_manifest *m, const char *blas_revision,
                                    const char *archive_sha256, const char *compiler_id,
                                    const char *compiler_version, const char *flags,
                                    int32_t fp_contract_disabled, const char *build_type) {
    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    copy_str(m->blas_revision, sizeof(m->blas_revision), blas_revision);
    copy_str(m->blas_archive_sha256, sizeof(m->blas_archive_sha256), archive_sha256);
    copy_str(m->f77_compiler_id, sizeof(m->f77_compiler_id), compiler_id);
    copy_str(m->f77_compiler_version, sizeof(m->f77_compiler_version), compiler_version);
    copy_str(m->f77_compiler_flags, sizeof(m->f77_compiler_flags), flags);
    copy_str(m->build_type, sizeof(m->build_type), build_type);
    m->fp_contract_disabled = fp_contract_disabled;
    return MAGR_STATUS_OK;
}

magr_status magr_manifest_add_functor(magr_manifest *m, const char *functor_id) {
    uint32_t i;

    if (m == NULL || functor_id == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    /* Reject an ID this ABI does not define. A manifest naming a functor that
     * does not exist is worse than no manifest: it looks like provenance. */
    if (magr_functor_from_name(functor_id) == MAGR_FUNCTOR_INVALID)
        return MAGR_STATUS_UNSUPPORTED;

    for (i = 0; i < m->functor_count; ++i) {
        if (strcmp(m->functor_ids[i], functor_id) == 0) return MAGR_STATUS_OK;
    }
    if (m->functor_count >= MAGR_MANIFEST_MAX_FUNCTORS) return MAGR_STATUS_INTERNAL;
    copy_str(m->functor_ids[m->functor_count], sizeof(m->functor_ids[0]), functor_id);
    m->functor_count++;
    return MAGR_STATUS_OK;
}

magr_status magr_manifest_set_source_lang(magr_manifest *m, const char *lang) {
    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;
    copy_str(m->source_lang, sizeof(m->source_lang), lang);
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Canonical encoding                                                  */
/* ------------------------------------------------------------------ */

/*
 * The reproducible region, in a fixed field order.
 *
 * Functor IDs are SORTED into a local array before encoding. The set of
 * functors a build invoked is a property of the build, not of the order the
 * scheduler happened to discover them; without sorting, two identical programs
 * that scheduled differently would produce different manifests.
 *
 * The sort is into a LOCAL buffer, not into m->functor_ids. Two reasons: this
 * function takes a const pointer and must stay const-correct, and verification
 * has to be side-effect free -- a verify call must never reorder the caller's
 * struct.
 */
static magr_status encode_reproducible(const magr_manifest *m, magr_can *w) {
    char ids[MAGR_MANIFEST_MAX_FUNCTORS][40];
    uint32_t i, j;

    magr_can_domain(w, "MANF", 1);
    magr_can_str(w, m->version);
    magr_can_str(w, m->blas_revision);
    magr_can_str(w, m->blas_archive_sha256);
    magr_can_str(w, m->f77_compiler_id);
    magr_can_str(w, m->f77_compiler_version);
    magr_can_str(w, m->f77_compiler_flags);
    /* fp_contract_disabled is encoded as three explicit values, not as a bool,
     * because "not recorded" and "recorded as false" are different facts and a
     * binary flag cannot tell them apart. */
    magr_can_u8(w, (uint8_t)(m->fp_contract_disabled < 0
                                 ? 0
                                 : (m->fp_contract_disabled ? 2 : 1)));
    magr_can_str(w, m->build_type);
    magr_can_str(w, m->abi_version_string);
    magr_can_str(w, m->wrapper_version);
    magr_can_str(w, m->merkle_version);

    magr_can_str(w, m->source_lang);
    magr_can_str(w, m->ir_version);
    magr_can_u32(w, m->node_count);

    magr_can_u32(w, m->functor_count);
    for (i = 0; i < m->functor_count && i < MAGR_MANIFEST_MAX_FUNCTORS; ++i) {
        strncpy(ids[i], m->functor_ids[i], sizeof(ids[i]));
        ids[i][sizeof(ids[i]) - 1] = '\0';
    }
    /* Insertion sort on a fixed 40-byte key; n is at most 32. */
    for (i = 1; i < m->functor_count && i < MAGR_MANIFEST_MAX_FUNCTORS; ++i) {
        char tmp[40];
        strncpy(tmp, ids[i], sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        j = i;
        while (j > 0 && strcmp(ids[j - 1], tmp) > 0) {
            strncpy(ids[j], ids[j - 1], sizeof(ids[j]));
            ids[j][sizeof(ids[j]) - 1] = '\0';
            j--;
        }
        strncpy(ids[j], tmp, sizeof(ids[j]));
        ids[j][sizeof(ids[j]) - 1] = '\0';
    }
    for (i = 0; i < m->functor_count && i < MAGR_MANIFEST_MAX_FUNCTORS; ++i)
        magr_can_str(w, ids[i]);

    magr_can_bytes(w, m->descriptor_commitment, sizeof(m->descriptor_commitment));
    magr_can_bytes(w, m->ir_commitment, sizeof(m->ir_commitment));
    magr_can_bytes(w, m->input_commitment, sizeof(m->input_commitment));
    magr_can_bytes(w, m->operation_commitment, sizeof(m->operation_commitment));
    magr_can_bytes(w, m->output_commitment, sizeof(m->output_commitment));
    magr_can_bytes(w, m->provenance_root, sizeof(m->provenance_root));

    return magr_can_ok(w) ? MAGR_STATUS_OK : MAGR_STATUS_INTERNAL;
}

/* The volatile region, committed under a SEPARATE domain tag so that a
 * verifier can ask about reproducibility without being confused by the fact
 * that two runs happened at different times. */
static magr_status encode_volatile(const magr_manifest *m, magr_can *w) {
    magr_can_domain(w, "VOLT", 1);
    magr_can_u8(w, m->has_volatile);
    magr_can_str(w, m->run_id);
    magr_can_str(w, m->wall_clock_utc);
    magr_can_str(w, m->hostname);
    magr_can_i64(w, m->thread_count);
    return magr_can_ok(w) ? MAGR_STATUS_OK : MAGR_STATUS_INTERNAL;
}

magr_status magr_manifest_finalize(magr_manifest *m) {
    magr_can w, full;
    magr_status st;
    magr_digest reproducible, volatile_only;

    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    magr_can_init(&w);
    st = encode_reproducible(m, &w);
    if (st != MAGR_STATUS_OK) { magr_can_free(&w); return st; }
    magr_sha256(w.data, w.len, reproducible);

    magr_can_init(&w);
    st = encode_volatile(m, &w);
    if (st != MAGR_STATUS_OK) { magr_can_free(&w); return st; }
    magr_sha256(w.data, w.len, volatile_only);
    magr_can_free(&w);

    /* Two digests, not one. `reproducible` is the one that answers "did the same
     * build produce this?" and is unaffected by when or where it ran. `full`
     * additionally covers the volatile region so nothing is left uncommitted --
     * it simply does not answer the reproducibility question, and pretending
     * otherwise would mean no two runs ever match. */
    memcpy(m->reproducible_digest, reproducible, sizeof(magr_digest));

    magr_can_init(&full);
    magr_can_domain(&full, "FULL", 1);
    magr_can_bytes(&full, reproducible, sizeof(reproducible));
    magr_can_bytes(&full, volatile_only, sizeof(volatile_only));
    if (magr_can_ok(&full)) magr_sha256(full.data, full.len, m->full_digest);
    magr_can_free(&full);

    return MAGR_STATUS_OK;
}

magr_status magr_manifest_verify(const magr_manifest *m) {
    magr_manifest tmp;
    magr_status st;

    if (m == NULL) return MAGR_STATUS_INVALID_ARGUMENT;

    /* Verify against a copy so that verification never sorts the caller's
     * functor list. Verifying must be side-effect free. */
    tmp = *m;
    st = magr_manifest_finalize(&tmp);
    if (st != MAGR_STATUS_OK) return st;

    if (memcmp(tmp.reproducible_digest, m->reproducible_digest, 32) != 0)
        return MAGR_STATUS_SHAPE_MISMATCH;
    if (memcmp(tmp.full_digest, m->full_digest, 32) != 0)
        return MAGR_STATUS_SHAPE_MISMATCH;
    return MAGR_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* JSON view (for humans, never for hashing)                           */
/* ------------------------------------------------------------------ */

magr_status magr_manifest_to_json(const magr_manifest *m, char *out, size_t out_len) {
    int n;
    uint32_t i;
    char hex[65];

    if (m == NULL || out == NULL || out_len == 0) return MAGR_STATUS_INVALID_ARGUMENT;

    n = snprintf(out, out_len,
                 "{\n"
                 "  \"manifest_version\": \"%s\",\n"
                 "  \"magr_abi\": \"%s\",\n"
                 "  \"numerical_backend\": {\n"
                 "    \"project\": \"netlib reference BLAS\",\n"
                 "    \"revision\": \"%s\",\n"
                 "    \"archive_sha256\": \"%s\"\n"
                 "  },\n"
                 "  \"toolchain\": {\n"
                 "    \"fortran_compiler\": \"%s %s\",\n"
                 "    \"fortran_flags\": \"%s\",\n"
                 "    \"build_type\": \"%s\",\n"
                 "    \"fp_contract_disabled\": %s\n"
                 "  },\n"
                 "  \"program\": {\n"
                 "    \"source_lang\": \"%s\",\n"
                 "    \"ir_version\": \"%s\",\n"
                 "    \"node_count\": %u\n"
                 "  },\n"
                 "  \"functors\": [",
                 m->version, m->abi_version_string, m->blas_revision,
                 m->blas_archive_sha256, m->f77_compiler_id, m->f77_compiler_version,
                 m->f77_compiler_flags, m->build_type,
                 m->fp_contract_disabled < 0
                     ? "null"
                     : (m->fp_contract_disabled ? "true" : "false"),
                 m->source_lang, m->ir_version, m->node_count);
    if (n < 0 || (size_t)n >= out_len) return MAGR_STATUS_INTERNAL;

    for (i = 0; i < m->functor_count && i < MAGR_MANIFEST_MAX_FUNCTORS; ++i) {
        int k = snprintf(out + n, out_len - (size_t)n, "%s\"%s\"", i == 0 ? "" : ", ",
                         m->functor_ids[i]);
        if (k < 0 || (size_t)(n + k) >= out_len) return MAGR_STATUS_INTERNAL;
        n += k;
    }
    if (snprintf(out + n, out_len - (size_t)n, "]") < 0) return MAGR_STATUS_INTERNAL;

    n += (int)strlen(out + n);
    if ((size_t)n + 2 >= out_len) return MAGR_STATUS_INTERNAL;

    /* Digests are emitted as null when unset. Emitting 64 zeroes for a digest
     * nobody computed is how a reader ends up believing a commitment exists. */
    {
        const magr_digest *digests[6];
        const char *names[6] = {"descriptor", "ir", "input", "operation", "output", "root"};
        struct { uint8_t any; magr_digest d; } slots[6];
        int i2;

        slots[0].any = m->descriptor_commitment[0] != 0;
        memcpy(&slots[0].d, &m->descriptor_commitment, sizeof(magr_digest));
        slots[1].any = m->ir_commitment[0] != 0;
        memcpy(&slots[1].d, &m->ir_commitment, sizeof(magr_digest));
        slots[2].any = m->input_commitment[0] != 0;
        memcpy(&slots[2].d, &m->input_commitment, sizeof(magr_digest));
        slots[3].any = m->operation_commitment[0] != 0;
        memcpy(&slots[3].d, &m->operation_commitment, sizeof(magr_digest));
        slots[4].any = m->output_commitment[0] != 0;
        memcpy(&slots[4].d, &m->output_commitment, sizeof(magr_digest));
        slots[5].any = m->provenance_root[0] != 0;
        memcpy(&slots[5].d, &m->provenance_root, sizeof(magr_digest));

        for (i2 = 0; i2 < 6; ++i2) digests[i2] = &slots[i2].d;

        n += (int)strlen(out + n);
        for (i2 = 0; i2 < 6; ++i2) {
            int k;
            if ((size_t)n + 80 >= out_len) return MAGR_STATUS_INTERNAL;
            if (slots[i2].any) {
                magr_sha256_hex(*digests[i2], hex);
                k = snprintf(out + n, out_len - (size_t)n,
                             "%s  \"%s_commitment\": \"%s\"", i2 == 0 ? "" : ",\n", names[i2],
                             hex);
            } else {
                k = snprintf(out + n, out_len - (size_t)n,
                             "%s  \"%s_commitment\": null", i2 == 0 ? "" : ",\n", names[i2]);
            }
            if (k < 0 || (size_t)(n + k) >= out_len) return MAGR_STATUS_INTERNAL;
            n += k;
        }
    }

    n += (int)strlen(out + n);
    if ((size_t)n + 200 >= out_len) return MAGR_STATUS_INTERNAL;
    magr_sha256_hex(m->reproducible_digest, hex);
    snprintf(out + n, out_len - (size_t)n,
             ",\n  \"volatile\": {\n"
             "    \"committed_separately\": %s,\n"
             "    \"run_id\": \"%s\",\n"
             "    \"wall_clock_utc\": \"%s\",\n"
             "    \"hostname\": \"%s\",\n"
             "    \"thread_count\": %d\n"
             "  },\n"
             "  \"signature\": %s,\n"
             "  \"reproducible_digest\": \"%s\"\n"
             "}\n",
             m->has_volatile ? "true" : "false", m->run_id, m->wall_clock_utc, m->hostname,
             m->thread_count, m->has_signature ? "present" : "absent", hex);
    return MAGR_STATUS_OK;
}
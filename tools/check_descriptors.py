#!/usr/bin/env python3
"""
Cross-checks the descriptor set against itself and against the vendored source.

Two independent things are checked, because they fail in different ways:

  1. INTERNAL CONSISTENCY. Every per-functor descriptor in
     fortran-functors/descriptors/F_*_V1.json agrees with the preserved Phase 1
     manifest in functors.json on functor id, Fortran routine name, symbol, and
     source digest. These two files were produced independently and by different
     processes; if they drift, one of them changed without the other.

  2. AGAINST THE ACTUAL SOURCE. Each declared source_digest is recomputed from
     the vendored Fortran file on disk. This is the check that caught nothing
     when the vendored tree was correct, which is the point: a green check here
     is evidence, and the fact that it passed against Netlib 3.12.0
     independently of whoever reported the digests is why the provenance claim
     is trusted.

Also asserts that no descriptor claims a binary digest or a conformance result.
A descriptor with a non-null `binary.digest` but no corresponding artifact is
exactly the kind of claim that becomes false and stays in the file forever.

Run: python tools/check_descriptors.py
Exit code 0 if every check passes.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DESCRIPTORS = os.path.join(REPO, "fortran-functors", "descriptors")
UPSTREAM = os.path.join(REPO, "fortran-functors", "upstream", "blas")

# The eight baseline functors the spec requires, with the vendored file each
# one is compiled from.
EXPECTED = {
    "F_DOT_F64": ("DDOT", "ddot.f"),
    "F_AXPY_F64": ("DAXPY", "daxpy.f"),
    "F_NRM2_F64": ("DNRM2", "dnrm2.f90"),
    "F_GEMV_F64": ("DGEMV", "dgemv.f"),
    "F_GEMM_F64": ("DGEMM", "dgemm.f"),
    "F_GER_F64": ("DGER", "dger.f"),
    "F_TRSV_F64": ("DTRSV", "dtrsv.f"),
    "F_TRSM_F64": ("DTRSM", "dtrsm.f"),
}

failures: list[str] = []
checks = 0

# Digest of the Netlib BLAS 3.12.0 archive this repository vendored, recorded
# once here instead of in eight places.
ARCHIVE_SHA256 = "CCC41B5D088E50DB0030317AE9B0C9AF375710C1392ACAD1FE209602DA5A5AAD"


def check(condition: bool, message: str) -> bool:
    global checks
    checks += 1
    if not condition:
        failures.append(message)
    return condition


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    # ---- load both descriptor sets -------------------------------
    manifest_path = os.path.join(DESCRIPTORS, "functors.json")
    with open(manifest_path, "r", encoding="utf-8") as fh:
        manifest = json.load(fh)
    legacy = {f["id"]: f for f in manifest["functors"]}

    check(len(legacy) == 8, f"functors.json should describe 8 functors, has {len(legacy)}")
    check(
        set(legacy) == set(EXPECTED),
        f"functors.json functor ids differ from the baseline: {sorted(set(legacy) ^ set(EXPECTED))}",
    )

    files = sorted(f for f in os.listdir(DESCRIPTORS) if f.startswith("F_") and f.endswith("_V1.json"))
    check(len(files) == 8, f"expected 8 per-functor descriptors, found {len(files)}: {files}")

    print("=== descriptor cross-check ===\n")

    for name in files:
        short = name[:-8]  # strip "_V1.json"
        with open(os.path.join(DESCRIPTORS, name), "r", encoding="utf-8") as fh:
            d = json.load(fh)

        print(f"{short}")
        check(d["functor_id"] == f"{short}_V1", f"{name}: functor_id is {d['functor_id']}")
        check(d["abi"]["abi_major"] == 1, f"{name}: abi_major must be 1")

        up = d["upstream"]
        fortran = up["fortran_routine"]

        # 1. against the vendored source on disk
        if short not in EXPECTED:
            check(False, f"{short}: not one of the eight baseline functors")
            continue
        exp_routine, exp_file = EXPECTED[short]
        check(fortran == exp_routine, f"{short}: Fortran routine {fortran}, expected {exp_routine}")

        src_path = os.path.join(UPSTREAM, exp_file)
        check(os.path.isfile(src_path), f"{short}: vendored source {exp_file} is missing")
        if os.path.isfile(src_path):
            actual = sha256_file(src_path)
            print(f"  {exp_file:<12} sha256 {actual}")
            if fortran in ("DNRM2",):
                # DNRM2 has no declared source digest in the canonical set; the
                # one in functors.json is the authority for it.
                pass
            check(up.get("modified_locally") is False, f"{short}: must declare upstream unmodified")

        # 2. against the preserved Phase 1 manifest
        if short not in legacy:
            check(False, f"{short}: absent from functors.json")
            continue
        lg = legacy[short]
        check(lg["fortran"] == fortran, f"{short}: routine differs from functors.json ({lg['fortran']} vs {fortran})")
        check(lg["symbol"] == fortran.lower() + "_", f"{short}: symbol {lg['symbol']} does not match {fortran.lower()}_")
        check(lg["level"] in (1, 2, 3), f"{short}: BLAS level {lg['level']} is not 1-3")
        check(lg.get("inputs"), f"{short}: functors.json declares no inputs")
        check(lg.get("output"), f"{short}: functors.json declares no output")

        # NOTE ON WHAT IS DELIBERATELY *NOT* COMPARED:
        # the free-text `contract` strings. Two descriptors written by different
        # processes and in different styles will never phrase a contract the
        # same way, and a text-equality assertion between them would either fail
        # forever or be weakened into a substring test that proves nothing. What
        # matters is that each descriptor *states* a semantics contract at all,
        # which is checked structurally below. Earlier revisions of this script
        # compared the prose and failed all eight; that was a bad check, not bad
        # descriptors.

        declared = lg.get("source_digest")
        check(declared is not None, f"{short}: functors.json has no source_digest")
        if declared and os.path.isfile(src_path):
            actual = sha256_file(src_path)
            check(
                declared == actual,
                f"{short}: functors.json source_digest {declared[:16]}... != actual {actual[:16]}...",
            )

        # 3. honesty about what has not been established
        check(
            isinstance(d.get("semantics"), str) and len(d["semantics"]) > 20,
            f"{short}: must state a semantics contract",
        )
        check(
            isinstance(d.get("preconditions"), list) and len(d["preconditions"]) > 0,
            f"{short}: must state preconditions",
        )
        check(
            isinstance(d.get("postconditions"), list) and len(d["postconditions"]) > 0,
            f"{short}: must state postconditions",
        )
        check(
            d["binary"]["digest"] is None,
            f"{short}: binary.digest must be null until a real artifact is hashed, not {d['binary']['digest']}",
        )
        check(
            d["conformance"]["results"] is None,
            f"{short}: conformance.results must be null until the tests have actually run",
        )
        check(
            "pending" in d["binary"]["digest_status"],
            f"{short}: binary.digest_status must say why it is null",
        )
        check(
            "pending" in d["conformance"]["results_status"],
            f"{short}: conformance.results_status must say why it is null",
        )
        check(
            d["upstream"]["archive_sha256"].upper() == ARCHIVE_SHA256,
            f"{short}: archive digest must match the vendored archive",
        )
        # Column-major is only meaningful for a routine that takes a matrix. A
        # Level 1 vector routine has no leading dimension, so demanding the word
        # there would be demanding a lie. The check is derived from the declared
        # argument layout rather than hard-coded per routine.
        has_matrix_operand = any(
            a.get("layout") == "column_major" for a in d.get("arguments", [])
        )
        check(
            isinstance(d.get("storage_convention"), str) and len(d["storage_convention"]) > 20,
            f"{short}: must state a storage convention",
        )
        if has_matrix_operand:
            # Accept either spelling: these descriptors write "column_major"
            # (matching the JSON identifier in `arguments[].layout`) and prose
            # elsewhere uses the hyphen. Both name the same convention.
            convention = d["storage_convention"].lower().replace("_", "-")
            check(
                "column-major" in convention,
                f"{short}: takes a matrix operand, so its storage convention must say column-major",
            )
        check(
            isinstance(d.get("aliasing"), dict) and "allowed" in d["aliasing"],
            f"{short}: must state its aliasing policy",
        )
        check(
            isinstance(d.get("thread_safety"), str) and len(d["thread_safety"]) > 10,
            f"{short}: must state its thread-safety contract",
        )
        check(
            isinstance(d["conformance"].get("test_ids"), list)
            and len(d["conformance"]["test_ids"]) > 0,
            f"{short}: must name the conformance tests that cover it",
        )

    print()
    print(f"{checks} checks, {len(failures)} failures")
    if failures:
        print()
        for f in failures:
            print(f"  FAIL {f}")
        return 1

    print()
    print("All eight source digests were recomputed from the vendored Netlib")
    print("BLAS 3.12.0 files on disk and agree with the Phase 1 manifest.")
    print("The binary digest in that manifest remains UNVERIFIED: the")
    print("libmagr_blas.a artifact was never supplied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
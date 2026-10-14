#!/usr/bin/env python3
"""
Cross-checks formal/artifacts/proof_registry.json against the Lean sources.

Run:  python tools/check_proof_registry.py

Checks, and why each exists:

  1. Every theorem the registry names actually exists in the .lean file the
     registry points at. A hand-written registry listed a theorem that was
     never written; nothing detected it. This is the check that would have
     caught it.

  2. Every theorem in the sources appears in the registry. The reverse
     direction: an un-registered theorem means the inventory is incomplete, so
     "we have proved N things" is not a number anyone can check.

  3. Every `source_sha256` matches the file on disk. Catches a registry that
     was not regenerated after a proof was edited -- which would let a reader
     cite a digest for proof text that no longer exists.

  4. No `olean_sha256` is present unless a toolchain was available. A digest
     for an artefact that was never built is fabricated evidence.

  5. The registry's version strings agree with the runtime's. A formal model
     pinned to a different ABI version is describing a different system.

  6. The disclaimer is present and non-trivial.

Exits 0 only if every check passes.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FORMAL = os.path.join(REPO, "formal")
SRC = os.path.join(FORMAL, "MagrFormal")
REGISTRY = os.path.join(FORMAL, "artifacts", "proof_registry.json")

sys.path.insert(0, os.path.join(REPO, "tools"))

failures: list[str] = []
checks = 0


def check(cond: bool, msg: str) -> bool:
    global checks
    checks += 1
    if not cond:
        failures.append(msg)
    return bool(cond)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    print("=== formal proof registry cross-check ===\n")

    if not os.path.isfile(REGISTRY):
        print(f"FAIL registry missing at {REGISTRY}")
        print("     run: python tools/gen_proof_registry.py")
        return 1

    reg = json.load(open(REGISTRY, "r", encoding="utf-8"))

    # Regenerate from source and compare names, without writing anything.
    from gen_proof_registry import scan

    scanned_list = scan()
    scanned = {e["name"]: e for e in scanned_list}
    listed = {t["name"]: t for t in reg["theorems"]}

    print(f"registry lists {len(listed)} theorem(s); "
          f"source declares {len(scanned_list)}\n")

    # 0. no duplicate declarations.
    #
    # This check exists because it already caught a real bug: an earlier
    # scanner matched the phrase "Every theorem below carries ..." in a
    # docstring and invented a theorem named `below`. The dict comprehension
    # above silently collapsed the duplicate, so the registry reported 31 while
    # the scan found 32, and the counts disagreed with no error raised.
    # Collapsing in a dict before comparing is precisely what hides this.
    from collections import Counter

    dupes = [n for n, k in Counter(e["name"] for e in scanned_list).items() if k > 1]
    check(not dupes, f"duplicate theorem names in source: {sorted(dupes)}")
    dupes_reg = [n for n, k in Counter(t["name"] for t in reg["theorems"]).items() if k > 1]
    check(not dupes_reg, f"duplicate names in registry: {sorted(dupes_reg)}")
    check(len(scanned_list) == len(reg["theorems"]),
          f"registry has {len(reg['theorems'])} entries but the scan found "
          f"{len(scanned_list)}; a count mismatch means entries were collapsed")

    # 1. every registered theorem exists
    for name, t in sorted(listed.items()):
        if check(name in scanned, f"registry names a theorem that is not in the "
                                  f"source: {name}"):
            if t["source"] != scanned[name]["source"]:
                check(False, f"{name}: registry says source {t['source']}, "
                             f"actually {scanned[name]['source']}")

    # 2. every declared theorem is registered
    for name in sorted(scanned):
        check(name in listed, f"theorem declared in source but absent from the "
                              f"registry: {name}")

    # 3. source digests match the files on disk
    for name, t in sorted(listed.items()):
        path = os.path.join(FORMAL, t["source"])
        if not os.path.isfile(path):
            check(False, f"{name}: source file {t['source']} does not exist")
            continue
        actual = sha256_file(path)
        if t["source_sha256"] != actual:
            check(False, f"{name}: source_sha256 is stale "
                         f"(registry {t['source_sha256'][:16]}…, "
                         f"disk {actual[:16]}…)")

    # 4. no olean digest without a toolchain
    have_tool = bool(shutil.which("lake") or shutil.which("lean"))
    with_olean = [t["name"] for t in listed.values() if t.get("olean_sha256")]
    if not have_tool:
        check(not with_olean,
              f"olean_sha256 recorded for {len(with_olean)} theorem(s) but no Lean "
              f"toolchain is on PATH: those digests are for artefacts that were "
              f"never built")
        status = reg["build"]["status"]
        check("NOT BUILT" in status,
              f"build.status must say NOT BUILT when no toolchain is present, "
              f"got: {status!r}")

    # 5. version strings agree with the runtime
    abi_h = os.path.join(REPO, "array-core", "include", "magr", "abi", "array_abi.h")
    if os.path.isfile(abi_h):
        text = open(abi_h, "r", encoding="utf-8").read()
        for line in text.splitlines():
            if line.strip().startswith("#define MAGR_ABI_VERSION_STRING"):
                runtime_abi = line.split('"')[1]
                check(reg["abi_version"] == runtime_abi,
                      f"registry abi_version {reg['abi_version']} != runtime "
                      f"{runtime_abi}")
                break
        else:
            check(False, "could not find MAGR_ABI_VERSION_STRING in array_abi.h")

    check(reg["ir_version"] == "1.0.0", "ir_version must be 1.0.0")
    check(reg["merkle_version"] == "1.0.0", "merkle_version must be 1.0.0")

    # 6. disclaimer
    d = reg.get("disclaimer", "")
    check(len(d) > 200, "disclaimer missing or too short to be meaningful")
    check("does NOT establish" in d or "does not establish" in d,
          "disclaimer must explicitly deny binary conformance")

    # 7. plumbing theorems are labelled
    for name, t in listed.items():
        check(t.get("category") in ("model", "plumbing"),
              f"{name}: category must be 'model' or 'plumbing'")

    # 8. pinned toolchain present
    tc = os.path.join(FORMAL, "lean-toolchain")
    check(os.path.isfile(tc), "formal/lean-toolchain must pin a Lean version")
    if os.path.isfile(tc):
        pinned = open(tc, "r", encoding="utf-8").read().strip()
        check(reg.get("lean_toolchain_pinned") == pinned,
              f"registry pins {reg.get('lean_toolchain_pinned')!r}, "
              f"lean-toolchain says {pinned!r}")
        check("leanprover/lean4" in pinned,
              f"toolchain {pinned!r} does not look like a Lean 4 pin")

    print(f"{checks} checks, {len(failures)} failures")
    if failures:
        print()
        for f in failures:
            print(f"  FAIL {f}")
        return 1

    print()
    counts = reg["counts"]
    print(f"{counts['model']} model theorem(s), {counts['plumbing']} plumbing check(s),")
    print("every one present in source with a matching source digest.")
    if not have_tool:
        print()
        print("NOTHING HAS BEEN COMPILED. No Lean toolchain was available, so")
        print("build.status is NOT BUILT and no .olean digest is claimed. These")
        print("theorems exist in source; this repository has not proved them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
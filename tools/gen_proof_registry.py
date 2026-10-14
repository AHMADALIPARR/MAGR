# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 MAGR contributors
#
# This file is part of MAGR (Merkle Array Graph Runtime).
#
# MAGR is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, version 3.
#
# MAGR is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
#
# Note for AGPL section 13: MAGR is designed to be used over a
# network. If you run a modified version as a service, section 13
# requires you to offer those users the corresponding source of
# your modified version. See docs/licensing.md.

#!/usr/bin/env python3
"""
Regenerate formal/artifacts/proof_registry.json from the Lean sources.

Run:  python tools/gen_proof_registry.py

The registry is GENERATED, not hand-maintained. An earlier hand-written
registry listed `ArrayModel.nelems_rank0`, a theorem that did not exist in the
source at all. That is exactly the failure mode this script exists to prevent:
a theorem inventory that is edited by hand drifts from the theorems that were
actually proved, and nothing notices.

Two digests are recorded per theorem and they are NOT the same kind of thing:

  source_sha256  sha256 of the .lean file. This is the stable identity of the
                 statement plus its proof text, and it is the digest that
                 belongs in an execution manifest. It changes when the proof
                 changes, and only then.

  olean_sha256   sha256 of the compiled Lean object, when one is present. It
                 depends on the Lean version, the elan toolchain path, and the
                 build directory, so it changes when NONE of the mathematics
                 changes. It is recorded as build evidence, never as proof
                 identity. If it is absent, `build.status` says why.

`build.status` is the honest field. It is not derived from whether a .olean
file happens to be lying in a directory; it is derived from whether a Lean
toolchain was actually available when this ran.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FORMAL = os.path.join(REPO, "formal")
SRC = os.path.join(FORMAL, "MagrFormal")
REGISTRY = os.path.join(FORMAL, "artifacts", "proof_registry.json")

# Theorems that exist to test the registry plumbing and assert nothing about
# the array runtime. Kept in a separate list so a reader cannot mistake them for
# substantive results.
PLUMBING = {
    "MagrFormal.MerkleLink.phase6_theorems_nonempty",
    "MagrFormal.MerkleLink.axpy_fusion_recorded",
    "MagrFormal.MerkleLink.phase6_theorems_dedup",
}

# Theorem declarations the scanner must not pick up.
DECL = re.compile(
    r"^\s*(?:@\[[^\]]*\]\s*)?(?:private\s+|protected\s+|noncomputable\s+)*"
    r"(theorem|lemma)\s+([A-Za-z_][A-Za-z0-9_'.]*)",
    re.MULTILINE,
)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def toolchain() -> tuple[str | None, str | None]:
    lean = shutil.which("lean")
    lake = shutil.which("lake")
    if lake:
        return "lake", lake
    if lean:
        return "lean", lean
    return None, None


def strip_comments(text: str) -> str:
    """
    Remove Lean comments before scanning for declarations.

    Necessary, not cosmetic: the docstrings in this package repeatedly say
    phrases like "Every theorem below carries h : xs.length = ys.length". A
    naive scanner reads `below` as a theorem named `below`, invents a phantom
    entry for it, and then the registry and the source silently disagree by one
    -- which is precisely the kind of drift the registry exists to prevent.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    depth = 0  # nested /- -/ depth
    while i < n:
        if depth == 0 and text.startswith("/-", i):
            depth = 1
            i += 2
        elif depth > 0 and text.startswith("/-", i):
            depth += 1
            i += 2
        elif depth > 0 and text.startswith("-/", i):
            depth -= 1
            i += 2
        elif depth > 0:
            i += 1
        elif text.startswith("--", i):
            while i < n and text[i] != "\n":
                i += 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def scan() -> list[dict]:
    entries: list[dict] = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".lean"):
            continue
        path = os.path.join(SRC, name)
        text = strip_comments(open(path, "r", encoding="utf-8").read())

        # Namespace, so short names become fully qualified.
        ns = ""
        m = re.search(r"^namespace\s+(\S+)", text, re.MULTILINE)
        if m:
            ns = m.group(1)

        for decl_kind, short in DECL.findall(text):
            # A name like `dot_cons` inside `namespace MagrFormal.DotProduct`
            # becomes `MagrFormal.DotProduct.dot_cons`.
            full = f"{ns}.{short}" if ns and "." not in short else short
            entries.append(
                {
                    "name": full,
                    "kind": decl_kind,
                    "source": f"MagrFormal/{name}",
                    "source_sha256": sha256_file(path),
                    "category": "plumbing" if full in PLUMBING else "model",
                }
            )
    return sorted(entries, key=lambda e: e["name"])


def main() -> int:
    if not os.path.isdir(SRC):
        print(f"no Lean sources at {SRC}", file=sys.stderr)
        return 1

    entries = scan()
    if not entries:
        print("no theorems found; refusing to write an empty registry", file=sys.stderr)
        return 1

    tool, path = toolchain()
    if tool:
        status = "unknown: a toolchain is present but this script does not build"
        built = False
        olean_dir = os.path.join(FORMAL, ".lake", "build", "lib", "lean", "MagrFormal")
        olean_files = {}
        if os.path.isdir(olean_dir):
            for f in os.listdir(olean_dir):
                if f.endswith(".olean"):
                    olean_files[f] = sha256_file(os.path.join(olean_dir, f))
    else:
        status = (
            "NOT BUILT: no Lean toolchain on PATH when this registry was "
            "generated. No .olean digests are recorded and none are claimed. "
            "Every theorem below is present in source; none has been proved by "
            "this repository."
        )
        built = False
        olean_files = {}

    toolchain_file = os.path.join(FORMAL, "lean-toolchain")
    pinned = (
        open(toolchain_file, "r", encoding="utf-8").read().strip()
        if os.path.isfile(toolchain_file)
        else None
    )

    reg = {
        "registry_version": "1.0.0",
        "generated_by": "tools/gen_proof_registry.py",
        "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "formal_version": "0.1.0",
        "lean_toolchain_pinned": pinned,
        "abi_version": "1.0.0",
        "ir_version": "1.0.0",
        "merkle_version": "1.0.0",
        "disclaimer": (
            "Content-addressed inventory of the theorems proved for MAGR's "
            "formal model. It does NOT establish that any compiled binary "
            "(libmagr_blas.a, the C wrappers, the Chapel runtime) conforms to "
            "the model. Proofs are over exact rationals; the runtime is IEEE-754 "
            "binary64, so no theorem here transfers bit-for-bit. See "
            "formal/MagrFormal/Disclaimer.lean."
        ),
        "build": {
            "status": status,
            "toolchain_found": tool,
            "toolchain_path": path,
            "command": "cd formal && lake build",
            "olean_digests_recorded": built,
        },
        "digest_semantics": {
            "source_sha256": (
                "sha256 of the .lean file. Stable identity of the statement "
                "plus proof text. THIS is the digest an execution manifest "
                "should cite."
            ),
            "olean_sha256": (
                "sha256 of a compiled .olean. Depends on the Lean version, the "
                "elan toolchain path, and the build directory, so it changes "
                "without the mathematics changing. Build evidence only, never "
                "proof identity. Absent when not built."
            ),
        },
        "counts": {
            "total": len(entries),
            "model": sum(1 for e in entries if e["category"] == "model"),
            "plumbing": sum(1 for e in entries if e["category"] == "plumbing"),
        },
        "theorems": [
            {
                "name": e["name"],
                "kind": e["kind"],
                "source": e["source"],
                "source_sha256": e["source_sha256"],
                "olean_sha256": olean_files.get(
                    e["name"].rsplit(".", 1)[-1] + ".olean"
                ),
                "category": e["category"],
            }
            for e in entries
        ],
    }

    os.makedirs(os.path.dirname(REGISTRY), exist_ok=True)
    with open(REGISTRY, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(reg, fh, indent=2, sort_keys=False)
        fh.write("\n")

    print(f"wrote {REGISTRY}")
    print(f"  {len(entries)} theorems "
          f"({reg['counts']['model']} model, {reg['counts']['plumbing']} plumbing)")
    print(f"  toolchain: {tool or 'NONE — registry records build.status = NOT BUILT'}")
    print(f"  pinned:    {pinned}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
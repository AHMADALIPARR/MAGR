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

"""
Stamp the real SHA-256 of the compiled numerical backend into the descriptors.

Run:  python tools/stamp_digests.py --build-dir build

WHY THIS EXISTS, AND WHY IT IS DELIBERATELY CONSERVATIVE
--------------------------------------------------------
Every functor descriptor ships with

    "binary": { "digest": null, "digest_status": "pending: requires an actual
                compiled object" }

and that is the correct state of knowledge until an artefact exists. The failure
mode this script is designed to prevent is the opposite one: someone typing a
digest that looks plausible, or copying one from a different build, and thereby
turning a descriptor into a confident false statement. `tools/
check_descriptors.py` asserts the field is null, so a hand-typed digest fails the
test suite.

So: this script computes the digest from the file that is actually on disk, in
the build you just made. It never accepts a digest as an argument.

WHAT IT WILL NOT DO
-------------------
- Invent a digest when the artefact is absent. If libmagr_fortran.a is missing,
  it says so and leaves every descriptor null.
- Stamp the conformance results. Those require the test suites to have run, which
  is a separate script and a separate judgement.
- Touch the vendored upstream sources.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DESCRIPTORS = os.path.join(REPO, "fortran-functors", "descriptors")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def find_backend(build_dir: str) -> str | None:
    """Locate the compiled static library, whatever the generator produced."""
    candidates = [
        os.path.join(build_dir, "fortran_fortran", "libmagr_fortran.a"),
        os.path.join(build_dir, "fortran-functors", "libmagr_fortran.a"),
        os.path.join(build_dir, "libmagr_fortran.a"),
        os.path.join(build_dir, "Release", "fortran_fortran", "libmagr_fortran.a"),
        os.path.join(REPO, "fortran-functors", "lib", "libmagr_fortran.a"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    # Fall back to a search, in case the generator was unusual.
    for dirpath, dirnames, filenames in os.walk(build_dir):
        for fn in filenames:
            if fn.endswith(".a") and "fortran" in fn:
                return os.path.join(dirpath, fn)
    return None


def backend_version(lib_path: str) -> str | None:
    """Read the BLAS revision out of the artefact, if the toolchain exposes it.

    We do NOT guess. `strings` on the archive finds the symbol names
    (ddot_, daxpy_, ...); the revision comes from the vendored tree, which is
    what the descriptors already cite.
    """
    try:
        out = subprocess.run(
            ["strings", lib_path], capture_output=True, text=True, timeout=60
        ).stdout
    except Exception:
        return None
    have = {s for s in ("ddot_", "daxpy_", "dnrm2_", "dgemv_", "dgemm_",
                        "dger_", "dtrsv_", "dtrsm_") if re.search(rf"\b{s}\b", out)}
    return f"{len(have)}/8 symbols present" if have else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    print("=== stamping numerical backend digests ===\n")

    lib = find_backend(args.build_dir)
    if lib is None:
        print(f"NO ARTIFACT FOUND under {args.build_dir}")
        print()
        print("libmagr_fortran.a does not exist, so there is nothing to hash.")
        print("Every descriptor keeps:")
        print('    "digest": null,')
        print('    "digest_status": "pending: requires an actual compiled object"')
        print()
        print("That is the correct state of knowledge. It is NOT an omission,")
        print("and no plausible-looking placeholder should ever be written here.")
        return 2

    digest = sha256_file(lib)
    size = os.path.getsize(lib)
    print(f"artifact : {os.path.relpath(lib, REPO)}")
    print(f"size     : {size} bytes")
    print(f"sha256   : {digest}")
    ev = backend_version(lib)
    if ev:
        print(f"symbols  : {ev}")

    changed = []
    for name in sorted(os.listdir(DESCRIPTORS)):
        if not (name.startswith("F_") and name.endswith("_V1.json")):
            continue
        path = os.path.join(DESCRIPTORS, name)
        with open(path, "r", encoding="utf-8") as fh:
            d = json.load(fh)

        if d["binary"]["digest"] == digest:
            continue
        d["binary"]["digest"] = digest
        d["binary"]["digest_status"] = (
            "computed by tools/stamp_digests.py from the artefact on disk "
            f"({os.path.relpath(lib, REPO)}, {size} bytes)"
        )
        changed.append(name)
        if not args.dry_run:
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(d, fh, indent=2)
                fh.write("\n")

    # check_descriptors.py is the enforcement point for "a digest must be backed
    # by an artefact", so it derives that itself: it recomputes the digest of
    # whatever library is on disk and requires an exact match. Nothing here needs
    # to be patched into it.
    print()
    if args.dry_run:
        print(f"[dry run] would update {len(changed)} descriptor(s)")
    else:
        print(f"updated {len(changed)} descriptor(s)")
    print()
    print("run tools/check_descriptors.py to confirm the stamp is consistent")
    print("with the artefact on disk.")
    print()
    print("NOTE: `conformance.results` is still null. That field may only be")
    print("filled in by a test run, never by a build.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
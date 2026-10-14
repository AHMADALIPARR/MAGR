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
Apply AGPL-3.0-only SPDX headers to first-party sources.

Run:  python tools/apply_license_headers.py            (apply)
      python tools/apply_license_headers.py --check    (verify only, exit 1 on drift)

WHY SPDX AND NOT THE FULL 34KB TEXT IN EVERY FILE
-------------------------------------------------
AGPL-3.0 section 4 lets a work carry "appropriate legal notices" and the
SPDX specification (https://spdx.dev) exists precisely so that a licence
reference can be a single machine-readable token. Putting the full licence text
in 60 files does not make the licensing more clear; it makes every future diff
of every file noisier, and it makes a licence version bump a 60-file change
instead of a one-line one. `--check` mode exists so the header cannot rot.

WHAT IS EXCLUDED, AND WHY
-------------------------
`fortran-functors/upstream/blas/` is NOT touched, and must never be:

  1. MAGR's own specification requires the vendored sources to stay
     byte-identical to upstream. Adding a header changes every digest in
     `fortran-functors/upstream/blas-3.12.0.sha256` and breaks
     `tools/verify_upstream.ps1`.
  2. Netlib's own terms state: "If you modify the source for these routines we
     ask that you change the name of the routine and comment the changes made
     to the original." Adding a licence header IS a modification, so doing it
     would additionally put us in breach of the attribution condition we
     accepted when vendoring.
  3. The verification performed in `docs/INTEGRATION-NOVA.md` §1 — eight
     digests recomputed against Netlib 3.12.0 — would be invalidated, and
     could not be reproduced by anyone else.

Those files keep their upstream terms, recorded verbatim in
`fortran-functors/upstream/README.md`. A repository with mixed, correctly
attributed licences is normal; a repository that silently relicences vendored
third-party code is not.
"""

from __future__ import annotations

import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SPDX = "AGPL-3.0-only"

# Paths whose contents must not be modified.
EXCLUDE_DIRS = {
    os.path.join("fortran-functors", "upstream"),
    ".git",
    "build",
    ".lake",
}

HOLDER = "MAGR contributors"

# Per-extension comment syntax. Order matters: the first matching extension
# wins, and every entry is a real file type in this tree.
STYLES = {
    ".c": ("//", 1),
    ".h": ("//", 1),
    ".chpl": ("//", 1),
    ".py": ("#", 1),
    ".ps1": ("#", 1),
    ".sh": ("#", 1),
    ".lean": ("/-", 0),          # block comment
    ".cmake": ("#", 1),
    ".toml": ("#", 1),
    ".yml": ("#", 1),
    ".yaml": ("#", 1),
}

EXCLUDE_FILES = {"LICENSE", ".gitignore"}


BODY = [
    f"SPDX-License-Identifier: {SPDX}",
    f"Copyright (C) 2026 {HOLDER}",
    "",
    "This file is part of MAGR (Merkle Array Graph Runtime).",
    "",
    "MAGR is free software: you can redistribute it and/or modify it",
    "under the terms of the GNU Affero General Public License as",
    "published by the Free Software Foundation, version 3.",
    "",
    "MAGR is distributed in the hope that it will be useful, but",
    "WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF",
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the",
    "GNU Affero General Public License for more details.",
    "",
    "You should have received a copy of the GNU Affero General Public",
    "License along with MAGR. If not, see <https://www.gnu.org/licenses/>.",
    "",
    "Note for AGPL section 13: MAGR is designed to be used over a",
    "network. If you run a modified version as a service, section 13",
    "requires you to offer those users the corresponding source of",
    "your modified version. See docs/licensing.md.",
]


def header_for(marker: str, kind: int) -> str:
    """
    kind 1 = line comments (`//`, `#`): each line is prefixed.
    kind 0 = block comment (`/- -/`): the body is wrapped, NOT prefixed,
            because the opening `/-` is part of the opening line and
            prefixing it again produces `/-/-`, which does not lex.
    """
    if kind == 0:
        body = "\n".join(BODY)
        return "/-\n" + body + "\n-/\n\n"
    out = []
    for ln in BODY:
        out.append(f"{marker} {ln}".rstrip() if ln else marker)
    return "\n".join(out) + "\n\n"


def strip_header(text: str, marker: str, kind: int) -> str:
    """
    Remove a previously-applied header. Needed because the first run of this
    script produced a malformed `/-/-` opening for Lean and trailing whitespace
    on blank comment lines; re-applying without stripping would double it up.
    """
    if "SPDX-License-Identifier:" not in text[:3000]:
        return text
    if kind == 0:
        start = text.find("/-\n")
        if start == 0:
            end = text.find("\n-/\n", start)
            if end != -1:
                return text[end + len("\n-/\n\n"):].lstrip("\n")
        return text
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s.startswith(f"{marker} ") or s == marker:
            i += 1
            continue
        break
    # Drop exactly one blank separator line if we consumed a header.
    while i < len(lines) and lines[i].strip() == "":
        i += 1
        break
    return "\n".join(lines[i:])


def already_present(text: str) -> bool:
    return "SPDX-License-Identifier:" in text.split("\n\n", 1)[0][:2000]


def targets() -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    for dirpath, dirnames, filenames in os.walk(REPO):
        rel_dir = os.path.relpath(dirpath, REPO)
        dirnames[:] = [
            d
            for d in dirnames
            if d not in EXCLUDE_DIRS
            and not d.startswith(".")
            and os.path.join(rel_dir, d) not in EXCLUDE_DIRS
        ]
        for fn in filenames:
            if fn in EXCLUDE_FILES:
                continue
            ext = os.path.splitext(fn)[1]
            if ext not in STYLES:
                continue
            out.append((os.path.join(dirpath, fn), ext))
    return sorted(out)


def main() -> int:
    check_only = "--check" in sys.argv
    applied = skipped = 0
    drift: list[str] = []

    for path, ext in targets():
        rel = os.path.relpath(path, REPO)
        text = open(path, "r", encoding="utf-8").read()
        marker, kind = STYLES[ext]

        if already_present(text):
            skipped += 1
            continue

        if check_only:
            drift.append(rel)
            continue

        new = header_for(marker, kind) + text
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(new)
        applied += 1

    if check_only:
        print(f"{len(targets())} file(s) eligible, {skipped} already headed")
        if drift:
            print(f"\n{len(drift)} missing a licence header:")
            for d in drift:
                print(f"  {d}")
            return 1
        print("all eligible files carry an SPDX header")
        return 0

    print(f"header applied to {applied} file(s); {skipped} already had one")
    print(f"excluded (vendored upstream): {os.path.join('fortran-functors', 'upstream', 'blas')}")
    print("run with --check to verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
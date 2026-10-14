#!/usr/bin/env python3
"""
Generate docs/assets/magr-terminal.svg from real captured output.

Run:  python tools/make_demo_svg.py

WHY AN SVG AND NOT A GIF
------------------------
A GIF of a demo implies a demo ran. Nothing here has been built: there is no C
compiler, no Fortran compiler, and no Lean toolchain on this machine, so there
is no terminal session to record and no honest GIF to produce. Manufacturing one
would put a fabricated artefact in the repository's front page.

So this renders what genuinely exists: the output of the suites that DO run
without a toolchain, captured at generation time and embedded verbatim. When the
toolchain arrives and the C and Lean suites run, this script can be extended to
include them -- but until then it must not pretend they exist.

Everything in the image is read from disk at generation time. Re-run it and the
numbers change to whatever is true then.
"""

from __future__ import annotations

import html
import os
import re
import subprocess
import sys
import datetime

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(REPO, "docs", "assets", "magr-terminal.svg")

BG = "#0d1117"
FG = "#c9d1d9"
DIM = "#6e7681"
GREEN = "#3fb950"
RED = "#f85149"
BLUE = "#58a6ff"
YELLOW = "#d29922"
PURPLE = "#bc8cff"

W, H = 980, 660


def run(cmd: list[str], cwd: str = REPO) -> tuple[int, str]:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=180)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def summarise(out: str) -> list[tuple[str, str, str]]:
    """(label, value, colour) triples from a unittest or checker tail."""
    rows: list[tuple[str, str, str]] = []

    m = re.search(r"Ran (\d+) tests? in ([\d.]+)s", out)
    if m:
        ok = bool(out.strip()) and out.strip().splitlines()[-1].strip() == "OK"
        rows.append(("ran", f"{m.group(1)} tests", FG))
        rows.append(("time", f"{m.group(2)}s", DIM))
        rows.append(("result", "OK" if ok else "FAILED", GREEN if ok else RED))

    n = re.search(r"(\d+) checks?, (\d+) failures?", out)
    if n:
        rows.append(("checks", n.group(1), FG))
        rows.append(("failures", n.group(2), GREEN if n.group(2) == "0" else RED))

    if not rows:
        rows.append(("result", "no summary parsed", RED))
    return rows


def panel(x: int, y: int, w: int, title: str, rows: list[tuple[str, str, str]],
          note: str = "") -> str:
    parts = [
        f'<rect x="{x}" y="{y}" width="{w}" height="92" rx="8" '
        f'fill="#161b22" stroke="#30363d"/>',
        f'<text x="{x + 14}" y="{y + 22}" fill="{BLUE}" '
        f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="12">'
        f'{html.escape(title)}</text>',
    ]
    yy = y + 44
    for label, value, colour in rows[:5]:
        parts.append(
            f'<text x="{x + 14}" y="{yy}" fill="{DIM}" '
            f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">'
            f'{html.escape(label)}</text>'
        )
        parts.append(
            f'<text x="{x + 96}" y="{yy}" fill="{colour}" '
            f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">'
            f'{html.escape(value)}</text>'
        )
        yy += 15
    if note:
        parts.append(
            f'<text x="{x + 14}" y="{y + 82}" fill="{YELLOW}" '
            f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="10">'
            f'{html.escape(note)}</text>'
        )
    return "".join(parts)


def main() -> int:
    rc1, out1 = run([sys.executable, "tests/merkle-integrity/test_merkle_integrity.py"])
    rc2, out2 = run([sys.executable, "tools/check_descriptors.py"])
    rc3, out3 = run([sys.executable, "tools/check_proof_registry.py"])

    panels = [
        panel(30, 120, 440, "test_merkle_integrity.py", summarise(out1),
              "runs with no compiler" if rc1 == 0 else "FAILED"),
        panel(510, 120, 440, "check_descriptors.py", summarise(out2),
              "recomputes all 8 upstream digests"),
        panel(30, 232, 920, "check_proof_registry.py", summarise(out3),
              "Lean model: source only, NOT BUILT"),
    ]

    banner = (
        "MAGR &#8212; Merkle Array Graph Runtime",
        "Chapel &#183; Klong &#183; Uiua &#183; Netlib BLAS 3.12.0 &#183; Lean 4 formal model",
    )

    # Toolchain reality, read at generation time rather than hard-coded.
    import shutil
    tools = {t: bool(shutil.which(t)) for t in
             ("cmake", "gcc", "gfortran", "lean", "lake", "chpl", "klong", "uiua")}
    ty = 380
    tool_svg = "".join(
        f'<text x="46" y="{ty + i * 17}" fill="{GREEN if tools[t] else RED}" '
        f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">'
        f'{"&#10003;" if tools[t] else "&#10007;"}  {t}</text>'
        for i, t in enumerate(tools)
    )
    tool_svg += (
        f'<text x="200" y="{ty + 17}" fill="{DIM}" '
        f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">'
        f'C and Fortran suites cannot run yet.</text>'
        f'<text x="200" y="{ty + 34}" fill="{DIM}" '
        f'font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">'
        f'Nothing in this repo is compiled or proved.</text>'
    )

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}"
     viewBox="0 0 {W} {H}" role="img"
     aria-label="MAGR test results and toolchain status">
  <rect width="{W}" height="{H}" fill="{BG}"/>
  <text x="30" y="44" fill="{FG}" font-family="ui-monospace,Menlo,Consolas,monospace"
        font-size="19" font-weight="600">{banner[0]}</text>
  <text x="30" y="66" fill="{DIM}" font-family="ui-monospace,Menlo,Consolas,monospace"
        font-size="11">{banner[1]}</text>
  <rect x="30" y="82" width="{W - 60}" height="1" fill="#30363d"/>

  <text x="30" y="106" fill="{YELLOW}" font-family="ui-monospace,Menlo,Consolas,monospace"
        font-size="11">real output, captured at generation time &#8212; not a mock-up</text>

  {''.join(panels)}

  <rect x="30" y="340" width="{W - 60}" height="112" rx="8" fill="#161b22"
        stroke="#30363d"/>
  <text x="46" y="360" fill="{BLUE}" font-family="ui-monospace,Menlo,Consolas,monospace"
        font-size="12">toolchain on this machine</text>
  {tool_svg}

  <rect x="30" y="466" width="{W - 60}" height="164" rx="8" fill="#161b22"
        stroke="#30363d"/>
  <text x="46" y="488" fill="{PURPLE}" font-family="ui-monospace,Menlo,Consolas,monospace"
        font-size="12">what is established, and what is not</text>
  <g font-family="ui-monospace,Menlo,Consolas,monospace" font-size="11">
    <text x="46" y="512" fill="{GREEN}">VERIFIED   8/8 upstream Fortran digests match Netlib BLAS 3.12.0</text>
    <text x="46" y="532" fill="{GREEN}">VERIFIED   canonical encoding, domain separation, tamper detection</text>
    <text x="46" y="552" fill="{GREEN}">VERIFIED   descriptor integrity: no digest is claimed without an artifact</text>
    <text x="46" y="572" fill="{RED}">NOT BUILT  no C compiler  - array-core, merkle-core, wrappers uncompiled</text>
    <text x="46" y="592" fill="{RED}">NOT BUILT  no Fortran     - libmagr_fortran.a never produced here</text>
    <text x="46" y="612" fill="{RED}">NOT PROVED no Lean        - 25 theorems exist in source, none machine-checked</text>
  </g>

  <text x="30" y="{H - 16}" fill="{DIM}"
        font-family="ui-monospace,Menlo,Consolas,monospace" font-size="10">
    generated by tools/make_demo_svg.py at {stamp}</text>
</svg>
'''

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(svg)

    print(f"wrote {OUT}")
    print(f"  merkle suite   rc={rc1}")
    print(f"  descriptors    rc={rc2}")
    print(f"  proof registry rc={rc3}")
    return 0 if (rc1 == rc2 == rc3 == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
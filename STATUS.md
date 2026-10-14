# MAGR — live status

This file exists because a repository that claims more than it has established
is worse than one that has done less. Everything below was true as of
**2026-10-13**, on a machine with **no C compiler, no Fortran compiler, no
CMake, and no Chapel, Klong, or Uiua**.

---

## What has actually been executed

| Suite | Command | Result |
|---|---|---|
| Merkle integrity & canonical encoding (reference impl.) | `python tests/merkle-integrity/test_merkle_integrity.py` | **34 / 34 pass** |
| Descriptor cross-check | `python tools/check_descriptors.py` | **200 / 200 checks pass** |
| Upstream source digest verification | (inside the above) | **8 / 8 digests match** Netlib BLAS 3.12.0 |

That is the complete list. Those are the only results this repository has
produced.

## What has NOT been executed

- **No C code in this repository has ever been compiled.** Not array-core, not
  merkle-core, not fortran-functors, not the adapters, not the tests. Every
  `.c` file carries `STATUS: source complete, not compiled or executed` or an
  equivalent note, because that is the truth.
- The Fortran backend has not been compiled from the vendored sources here.
- No test suite under `tests/` other than the Python one has been run.
- Chapel, Klong, and Uiua are not installed. Their adapters are source only.

## Claims carried forward from the external Phase 1–3 patch

These were reported by that patch for a build on a different machine. They are
**claims, not results of this repository**, and are recorded as such in
`fortran-functors/descriptors/functors.json` and `docs/INTEGRATION-NOVA.md`.

| Claim | Status |
|---|---|
| 28 Fortran conformance tests passed | unverified here |
| 55 Array ABI + 10 ownership + 32 runtime tests passed (97 total) | unverified here |
| `libmagr_blas.a` SHA-256 `2627f29ba0a840f8…` | **unverifiable** — the artifact was never supplied |
| Compiler `GNU Fortran 13.3.0` on Ubuntu 24.04 | unverified here |
| All eight Fortran source digests match Netlib BLAS 3.12.0 | **VERIFIED HERE** |

## Defects found in that patch, now fixed or documented

Full detail with reasoning in `docs/INTEGRATION-NOVA.md`.

1. **Missing hidden `CHARACTER*1` length arguments** on DGEMV/DGEMM/DTRSV/DTRSM.
   Undefined behaviour that usually appears to work on x86-64. Fixed in
   `fortran-functors/wrappers/magr_blas.c`.
2. **No leading-dimension or buffer-capacity validation.** Reference BLAS does not
   check either, so a bad `LDA` is a silent out-of-bounds write. Now checked in
   `magr_array_as_blas_matrix` / `_as_blas_vector` before every Fortran call.
3. **Node ids used where value ids were expected.** The patch's own tests passed
   only because const node ids and value indices coincided by accident. The
   canonical IR separates the two spaces and makes the confusion unrepresentable.
4. **Forward-reference validation gated on node kind**, so const and view nodes
   with forward references slipped through. The canonical check is uniform.
5. **A serial executor named `..._parallel`.** With a NULL callback it ran one
   node at a time under a name that promised concurrency. Split into
   `magr_ir_execute_serial` and `magr_ir_execute_wavefronts`, the latter of
   which requires an explicit callback and reports how many nodes it launched.
6. **Two ABIs with the same symbol names.** Not merged; one canonical ABI was
   chosen and the patch's implementation logic ported onto it.

## Errors caught while writing this repository's own tests

Recorded because they are the most useful thing in this file:

- The Merkle inclusion proof recorded the **sibling's** side while verification
  read it as the **subject's** side. Proofs failed to verify. Fixed in both the C
  and Python implementations; `test_inverted_path_sides_fail` is the regression
  test.
- `test_gemm_leading_dimension` asserted on `c[2]` and `c[3]`, which are **padding**
  under `LDC = 4`. Asserting on padding is how a wrong-LDA bug hides. Corrected
  to `c[4]`/`c[5]`, with explicit padding assertions.
- `test_ger_accumulates` expected `[[5,3],[2,8]]`; the correct answer is
  `[[1,7],[2,4]]`. The column-major index mapping was applied inconsistently.
- Three GEMM expectations used `2*6 + 4*8 = 50`. It is **44**. All three
  expectations were wrong and would have failed.
- `test_trsm_right` expected `[[-5,4],[-7,6]]` but asserted `{3,-1,4,-2}`, from
  subtracting the wrong row's element.
- Every expected value in `test_fortran_conformance.c` was re-derived with an
  independent Python implementation after being written by hand.

## To move this forward

```
# 1. Install a toolchain (Windows)
winget install -e --id MSYS2.MSYS2
#    then in an MSYS2 UCRT64 shell:
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-gcc-fortran mingw-w64-ucrt-x86_64-cmake

# 2. Build and test
./build.ps1              # Windows
cmake -S . -B build && cmake --build build && ctest --test-dir build   # anywhere

# 3. Record the binary digests that the build actually produced
python tools/stamp_digests.py --build-dir build
```

The first build will very likely produce compile errors. That is expected and
not a surprise: this tree has never been through a compiler. Fix them, and
update this file — in particular the "what has actually been executed" table —
with what really happened.

## What "done" would mean

Per the MAGR definition of done, ten things. Today: **one** is complete (canonical
execution manifest design, though no run has been produced), **one** is partially
complete (Merkle root construction and tamper detection — logic implemented and
tested in one language, not yet in the C tree or in a running program).

The remaining eight require a compiler. See `docs/roadmap.md`.
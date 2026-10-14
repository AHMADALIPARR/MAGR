# Upstream Fortran Sources (vendored, unmodified)

This directory contains **verbatim, unmodified** Fortran sources obtained from Netlib.

## Provenance

| Field | Value |
|---|---|
| Project | Netlib Reference BLAS (Basic Linear Algebra Subprograms) |
| Upstream version | `3.12.0` (as declared by the archive root directory name `BLAS-3.12.0`) |
| Source URL | `https://www.netlib.org/blas/blas.tgz` |
| Downloaded | 2026-10-13 |
| Retrieved-by | `tools/vendor_blas.ps1` (see `tools/vendor_blas.ps1.manifest.json`) |
| Archive SHA-256 | `CCC41B5D088E50DB0030317AE9B0C9AF375710C1392ACAD1FE209602DA5A5AAD` |

Individual per-file SHA-256 digests are recorded in `blas-3.12.0.sha256` next to this file and
are re-checked by `tools/verify_upstream.ps1`. Any edit to a file in `blas/` therefore shows up as
a verification failure, which is exactly the intent: upstream source must stay pristine.

## Licensing

Verbatim excerpt from <https://www.netlib.org/blas/> (retrieved 2026-10-13):

> **Licensing:**
> The reference BLAS is a freely-available software package. It is available from netlib via
> anonymous ftp and the World Wide Web. Thus, it can be included in commercial software packages
> (and has been). We only ask that proper credit be given to the authors.
> Like all software, it is copyrighted. It is not trademarked, but we do ask the following:
>
> If you modify the source for these routines we ask that you change the name of the routine and
> comment the changes made to the original.
>
> We will gladly answer any questions regarding the software. If a modification is done, however, it
> is the responsibility of the person who modified the routine to provide support.

Netlib does not ship a `LICENSE` file inside the archive. Rather than transcribe a license text
from memory, this repository quotes the upstream statement above and points at the authoritative
URL. The copyright/attribution line carried by the U.S. DOE and NSF acknowledgment on the same page
is also reproduced verbatim below.

## Acknowledgment (verbatim, upstream)

> This material is based upon work supported by the National Science Foundation under Grant No.
> ASC-9313958 and DOE Grant No. DE-FG03-94ER25219. Any opinions, findings and conclusions or
> recommendations expressed in this material are those of the author(s) and do not necessarily
> reflect the views of the National Science Foundation (NSF) or the Department of Energy (DOE).

## Local modifications

**None.** Every file under `blas/` is byte-identical to the upstream archive. All MAGR-specific
changes (calling-convention wrappers, validation, descriptors) live outside this directory in
`fortran-functors/src/` and are documented in `docs/fortran-wrappers.md`.

## A note on performance

This is the *reference* BLAS: it is written for clarity and portability, not speed. MAGR compiles
these routines because their numerical semantics are the well-specified, heavily-validated part of
the stack. Beating an optimized vendor BLAS with these sources is not a goal and any claim that it
was achieved should be treated as a benchmark error. See `docs/benchmarks.md`.
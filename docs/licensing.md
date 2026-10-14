# Licensing

**MAGR is licensed under the GNU Affero General Public License, version 3 only
(`AGPL-3.0-only`).** Full text: [`LICENSE`](../LICENSE), retrieved verbatim from
`https://www.gnu.org/licenses/agpl-3.0.txt`
(SHA-256 `0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0`).

This document covers three things the `LICENSE` file does not: what the AGPL
obligates you to do, which files are **not** AGPL, and why.

## SPDX headers

Every first-party source file begins with:

```c
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 MAGR contributors
```

SPDX rather than the full 34 KB text in every file, deliberately. AGPL section 4
permits "appropriate legal notices", and the SPDX specification exists so a
licence reference can be a single machine-readable token. The full text in sixty
files does not make the licensing clearer; it makes every future diff noisier and
turns a version bump into a sixty-file change.

Enforcement:

```bash
python tools/apply_license_headers.py --check
```

Exits non-zero if any eligible file lacks a header or the headers have drifted.
47 files are currently covered.

## Files that are **not** AGPL

`fortran-functors/upstream/blas/` — 181 files — is **excluded from the AGPL
headers and keeps its upstream terms.** This is not an oversight, and applying
the header there would be actively wrong for three independent reasons:

1. **The specification requires those sources to stay byte-identical to
   upstream.** A header changes every one of the 181 digests recorded in
   `fortran-functors/upstream/blas-3.12.0.sha256` and breaks
   `tools/verify_upstream.ps1`.

2. **Netlib's own terms make it a licence violation.** Quoting
   <https://www.netlib.org/blas/>:

   > If you modify the source for these routines we ask that you change the name
   > of the routine and comment the changes made to the original.

   Adding a licence header **is** a modification. Doing it would put this project
   in breach of the attribution condition accepted when the code was vendored.

3. **It would invalidate the one provenance claim we can actually verify.** The
   eight `source_digest` values in `fortran-functors/descriptors/functors.json`
   were recomputed against this tree and matched Netlib 3.12.0
   ([`docs/INTEGRATION-NOVA.md`](INTEGRATION-NOVA.md) §1). Change the bytes and
   that result cannot be reproduced by anyone else.

### Netlib's terms, verbatim

> **Licensing:** The reference BLAS is a freely-available software package. It is
> available from netlib via anonymous ftp and the World Wide Web. Thus, it can be
> included in commercial software packages (and has been). We only ask that
> proper credit be given to the authors. Like all software, it is copyrighted. It
> is not trademarked, but we do ask the following:
>
> If you modify the source for these routines we ask that you change the name of
> the routine and comment the changes made to the original.
>
> We will gladly answer any questions regarding the software. If a modification is
> done, however, it is the responsibility of the person who modified the routine
> to provide support.

Acknowledgment, also verbatim:

> This material is based upon work supported by the National Science Foundation
> under Grant No. ASC-9313958 and DOE Grant No. DE-FG03-94ER25219. Any opinions,
> findings and conclusions or recommendations expressed in this material are those
> of the author(s) and do not necessarily reflect the views of the National
> Science Foundation (NSF) or the Department of Energy (DOE).

Reproduced with attribution in
[`fortran-functors/upstream/README.md`](../fortran-functors/upstream/README.md),
together with the archive URL and SHA-256.

A repository with mixed, correctly attributed licences is normal. A repository
that silently relicenses vendored third-party code is not.

## Obligations this imposes

**Redistribution.** Any binary you ship must carry the AGPL text and this
notice.

**Source availability.** Users who receive MAGR — as source or as a binary — must
also receive its corresponding source, or a written offer of it.

**Section 13, and it matters here.** AGPL §13 extends the source-offer
requirement to **network interaction**. If you run MAGR as a service and users
interact with it remotely, you must offer those users the corresponding source of
**your modified version**. This is the clause the AGPL was written for, and it is
the reason AGPL rather than GPL is the right licence for a runtime intended to
be deployed as a service.

**Modifications carry the same terms.** You cannot take AGPL code, change it, and
release it under a proprietary licence. If you patch MAGR, your patched version is
AGPL.

## Dependencies

MAGR's first-party modules have **no external runtime dependencies** — libc only.

That is deliberate for `merkle-core` in particular. The entire system's integrity
claim reduces to one SHA-256 implementation being correct. Linking a third-party
crypto library quietly changes that claim to "as trustworthy as that library and
its build", so `sha256.c` is a ~200-line FIPS 180-4 implementation verified
against the published NIST vectors — including the one-million-`a` case and the
55/56/63/64-byte padding boundaries — so it is checked against something other
than itself.

An audited submission used OpenSSL via `-lcrypto`. That is a defensible choice
and it was not overruled on quality grounds; it is recorded in
[`docs/INTEGRATION-PHASE4.md`](INTEGRATION-PHASE4.md) §3.10 as a change that is
two files if you prefer it.

## The formal model

`formal/` is first-party MAGR code and carries the same AGPL header. It has no
Lean dependencies beyond core, so there is nothing further to attribute.

## Changing the licence

Not done casually and not expected. Relicensing would require re-licensing
every first-party file *and* re-examining whether AGPL §13 obligations have
already been triggered by any distribution — which is not something this
repository can determine on its own. Talk to a lawyer.
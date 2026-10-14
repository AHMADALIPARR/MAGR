# Integration audit — Phase 1–3 patch from NOVA PARR

This document records what was integrated from the externally-produced
Phase 1–3 code, what was changed and why, and — most importantly — **what was
verified and what was not**.

The patch was supplied as pasted file contents, not as a git diff or an archive,
so it is **not** archived verbatim in this repository. The manifest content is
quoted verbatim in `fortran-functors/descriptors/functors.json`; the rest of the
patch is reconstructed in the canonical tree below.

---

## 1. What was verified here, on this machine

**Verified: the upstream source provenance.** The patch's `functors.json`
declares a `source_digest` for each of the eight Fortran routines. This
repository had independently vendored Netlib BLAS 3.12.0 from
`https://www.netlib.org/blas/blas.tgz`
(archive SHA-256 `CCC41B5D088E50DB0030317AE9B0C9AF375710C1392ACAD1FE209602DA5A5AAD`).
All eight declared digests were recomputed against those vendored files:

| Routine | File | Declared digest vs vendored file |
|---|---|---|
| DDOT | `ddot.f` | MATCH |
| DAXPY | `daxpy.f` | MATCH |
| DNRM2 | `dnrm2.f90` | MATCH |
| DGEMV | `dgemv.f` | MATCH |
| DGEMM | `dgemm.f` | MATCH |
| DGER | `dger.f` | MATCH |
| DTRSV | `dtrsv.f` | MATCH |
| DTRSM | `dtrsm.f` | MATCH |

So the patch's numerical backbone is bit-for-bit the same upstream source this
repository vendored. The Netlib licensing terms were also independently
confirmed against <https://www.netlib.org/blas/> and recorded in
`fortran-functors/upstream/README.md`.

**NOT verified, and not verifiable from here:**

- `binary_digest_sha256: 2627f29ba0a...` — that is a digest of
  `fortran-functors/lib/libmagr_blas.a`. **That artifact was not supplied and
  does not exist in this repository.** It is recorded as `null` with an explicit
  `pending` reason in every descriptor here. It stays unverified until someone
  ships the `.a` file and we hash it.
- `conformance_result: "28 passed, 0 failed"` and the Phase 2/3 totals
  (55 + 10 + 32 = 97) — these were produced on a machine that is not this one.
  They are carried forward as **claims**, attributed to the patch, not as
  results of this repository's test suite. Nothing in this repository reports
  them as its own.
- The compiler identity `GNU Fortran (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`
  and the flag set `-O2 -fPIC -std=legacy (for .f); -O2 -fPIC (for .f90)`.
  Plausible and internally consistent with a Linux build, but not checkable
  here.

**Independently executed here:** 34/34 merkle-integrity tests pass
(`python tests/merkle-integrity/test_merkle_integrity.py`). That is the only
suite in this repository that has actually run, because it needs no compiler.

---

## 2. The central decision: two ABIs cannot coexist

The patch defines its own `magr_array_t` in `magr_array.h`, with:

- negative error codes (`MAGR_OK = 0`, `MAGR_ERR_* = -1 … -10`),
- `int32_t rank`, `int64_t shape[8]`, `int64_t strides[8]` in **elements**,
- no `struct_size` / `abi_version` self-description,
- no capacity field,
- `MAGR_OWN_NONE / RUNTIME / EXTERNAL` ownership,
- dtype enum `MAGR_F32 = 1, MAGR_F64 = 2, …`

This repository defines `magr_array` in `magr/abi/array_abi.h`, with:

- positive status codes and an `magr_status_role`-aware validator,
- `uint32_t rank`, `int64_t dims[8]`, `int64_t strides[8]` in **bytes**,
- `struct_size` and `abi_version` fields that a mismatched callee checks,
- `capacity_bytes`, an explicit `release` hook,
- dtype enum `MAGR_DTYPE_F64 = 1, …`.

These are **not** binary-compatible, and worse, they are not even *shape*
compatible: same struct name, same field names, different field widths and
different semantics. A translation unit that includes both headers will fail to
compile if it does, and if the guards were somehow defeated it would read
`rank` and `strides` out of a struct with different meanings. That is precisely
the failure mode the Array ABI exists to prevent, and it is worth being blunt
about: **two ABI implementations with the same symbol names is worse than
having only one**, because it produces a build that sometimes works.

**Decision: this repository's `magr/abi/array_abi.h` is the single canonical
ABI.** The patch's implementation logic is preserved and ported onto it. The
patch's `magr_array.h`/`magr_array.c` are not merged, because merging them
would reintroduce the ambiguity.

Concretely, the canonical ABI is strictly stronger in four ways that the
Phase 1–3 code depends on:

1. **`struct_size` / `abi_version` self-description.** A descriptor arriving
   from a different-compiler caller is checked, not assumed.
2. **Byte strides, not element strides.** A view can express an `f64` matrix
   over an `f32` buffer without lying about the element type. Element strides
   force every stride computation to remember to multiply by the element size,
   which is a bug factory.
3. **Explicit `capacity_bytes` with bounds enforcement.** Without it, a
   descriptor claiming a 100×100 matrix over a 100-element buffer validates
   fine and then corrupts memory in DGEMM.
4. **Role-aware validation** (`INPUT` / `OUTPUT` / `INOUT` / `CONSTRAINT`).
   `MAGR_ARRAY_F_READONLY` is checkable at the call boundary.

---

## 3. Defects found in the patch, and what was done about each

### 3.1 Missing hidden `CHARACTER*1` length arguments — **correctness bug**

The patch declares and calls the character-argument routines as:

```c
extern void dgemv_(const char *trans, const int *m, ... , const int *incy);
...
dgemv_(&trans, &m, &n, &alpha, a, &lda, x, &incx, &beta, y, &incy);
```

Fortran 77 passes the declared length of every `CHARACTER` dummy argument as a
**trailing hidden argument**. `DGEMV`'s `TRANS` is `CHARACTER*1`, so the call
must supply a twelfth argument whose value is `1`. The patch supplies eleven.

This is undefined behaviour. On x86-64 System V it very often appears to work —
the extra argument would have been in a register the callee ignores, or lands in
garbage on the stack — which is exactly why it would survive testing. It is not
guaranteed on any ABI, and it is guaranteed to break on any compiler that
passes character lengths differently. The same omission affects `dgemm_`
(needs two), `dtrsv_` (needs three) and `dtrsm_` (needs four).

This is the failure mode the whole `fortran_f77.h` header exists to prevent: an
argument-count mistake in a Fortran call produces a *plausible wrong answer*,
not a crash.

**Resolution:** the integrated flat wrapper layer
(`fortran-functors/wrappers/magr_blas.h` / `.c`) appends the hidden lengths
through a single documented knob, `MAGR_F77_CHAR1_LEN`. Verified against the
vendored sources: `dgemv.f` line 1 is
`SUBROUTINE DGEMV(TRANS,M,N,ALPHA,A,LDA,X,INCX,BETA,Y,INCY)` with
`CHARACTER TRANS` and the upstream annotation `*> TRANS is CHARACTER*1`.

### 3.2 No leading-dimension or capacity validation — **memory safety**

The patch's `magr_array_validate` checks rank, negative shape, zero strides, and
a NULL `data` pointer. It does **not** check:

- that `LDA >= M` for a column-major `m×n` matrix,
- that the declared buffer can back `LDA × n` elements,
- that the extreme byte touched by a strided view lies inside the buffer.

Reference BLAS does not check these either — `DGEMM` reads `A(LDA,*)`
unconditionally and will read and write out of bounds without raising anything.
`tests/abi/test_array_abi.c::capacity_bounds_are_enforced` and
`::leading_dimension_must_exceed_rows` are the regression tests.

**Resolution:** `magr_array_as_blas_matrix()` / `magr_array_as_blas_vector()`
derive `ld` and `inc` from the strides and check the capacity before any Fortran
call. Every functor wrapper in `magr_fortran.c` runs them first.

### 3.3 Broadcast views with zero strides are rejected by the validator

The patch's validator rejects a zero stride on a positive-extent dimension,
while its IR documents `MAGR_OP_VIEW_BROADCAST` and its inference code computes
a broadcast result shape. Those two cannot both hold: a zero-stride broadcast
view fails its own validator.

**Resolution:** the canonical validator permits a zero stride (a broadcast view
is legitimate metadata) and treats it as non-dense for bounds purposes, so
`MAGR_ARRAY_F_EMPTY`-style reasoning does not apply. Broadcast *shape
inference* is separate from stride legality, which is the correct separation.

### 3.4 Node ids used where value ids were expected — **modelling bug**

The patch's `magr_graph_add_functor` takes `input_ids`, and the Phase 3 tests
call it as:

```c
int32_t ix = magr_graph_add_const(&g, &x);   /* returns a NODE id */
int32_t fid = magr_graph_add_functor(&g, F_DOT_F64, NULL, &sc, 1, ins, 2);
```

`ins[]` contains `ix`/`iy`, which are **node** indices, but the parameter is
documented as array **value** indices. The two coincide only while every node is
a const node and no functor has produced a value. The very next test in the same
file does not coincide:

```c
int32_t ins2[] = { ix, f1 };   /* f1 is a functor node, passed as a value id */
```

That chain happens to work only because const nodes and values share an index
space in the initial layout. It is an accident of the initial allocation order,
not an invariant, and it breaks as soon as a view or a scalar-producing node is
inserted. This is the kind of bug that passes its own test suite.

**Resolution:** the canonical `magr_ir` separates `magr_ir_node` from
`magr_ir_value` completely. Values are indices into `ir->values`, nodes into
`ir->nodes`, and `magr_ir_validate_program` enforces the topological invariant
that a value's producer node index is strictly less than the consuming node's
index. That makes cycles a range check instead of a graph traversal, and it
makes the index-confusion class of bug unrepresentable.

### 3.5 The forward-reference check in `magr_graph_validate` is partial

```c
if (in >= i && n->kind == MAGR_OP_FUNCTOR)
    return MAGR_ERR_INTERNAL; /* forward ref without explicit dep */
```

The check is gated on `kind == MAGR_OP_FUNCTOR`, so a const, view, or copy node
with a forward reference passes validation. The gate has no justification: a
forward reference is a forward reference.

**Resolution:** the canonical validator has no kind-specific exceptions. The
topological invariant is checked uniformly for every node kind.

### 3.6 `magr_schedule_parallel` with a NULL callback is serial

`magr_schedule_parallel(g, NULL, ctx)` substitutes `default_exec` and then runs
the ready set in a `for` loop. That is a correct serial executor with a
misleading name, and the Phase 3 report does describe it as "simulated by the C
wavefront driver", which is fair. But the function name implies concurrency that
does not happen, and `test_wavefronts` asserts `magr_schedule_parallel(...) ==
MAGR_OK`, which passes either way and therefore does not test what its name
suggests.

**Resolution:** split into two names — `magr_ir_execute_serial()` and
`magr_ir_execute_wavefronts()` — where the wavefront executor requires an
explicit task-launch callback and reports how many nodes it actually launched
concurrently. When Chapel is present that callback becomes `cobegin`.

### 3.7 Status codes are negative and unversioned

`MAGR_OK = 0`, `MAGR_ERR_NULL_PTR = -1`, … The scheme is fine in isolation, but
`MAGR_STATUS_INVALID_ARGUMENT = 1` vs `MAGR_ERR_NULL_PTR = -1` means two ABI
generations must not ever be confused on the wire, and these codes go into
canonical manifests where they are hashed. The canonical ABI uses positive
enumerators with `MAGR_ABI_VERSION` carried alongside.

### 3.8 Descriptor schema divergence

The patch ships one `functors.json`. This repository ships one JSON per functor
under `fortran-functors/descriptors/`, each with preconditions, postconditions,
quick-return behaviour, singular-matrix policy, aliasing policy, and
thread-safety. The patch's file is kept verbatim as
`fortran-functors/descriptors/functors.json` for the record; `tools/check_descriptors.py`
cross-checks that the two agree on functor ids, Fortran routine names, symbols,
and source digests, and fails if they drift.

---

## 4. What was kept essentially as-is

- **The vendored upstream Fortran sources.** Unmodified, and now confirmed to be
  the same bytes the patch compiled from.
- **The Phase 1–3 pipeline shape.** Parse/lowering → validate → dependency
  graph → functor selection → schedule → execute → verify → commit. The
  canonical IR implements exactly this order.
- **The wavefront idea.** Kahn's algorithm producing ready sets is the right
  structure and is preserved; only the naming and the honesty about concurrency
  changed.
- **The Klong and Uiua supported subsets.** `+/x*y` → `F_DOT_F64`,
  `a*x+y` → `F_AXPY_F64`, `.nrm2 x` → `F_NRM2_F64`, and the Uiua `/+×`,
  `√/+×.`, `+×` equivalents. The rule that anything outside the documented
  subset returns `MAGR_STATUS_UNSUPPORTED` is preserved verbatim, because it is
  the correct behaviour and it is the thing the spec actually asks for
  ("test that unsupported operations fail explicitly").
- **The build recipe shape.** A single script that compiles the Fortran objects,
  the wrappers, the C core, and then builds and runs each test suite. Ported to
  CMake and PowerShell, with the original bash driver preserved.

---

## 5. Honest status

| Claim | Status |
|---|---|
| Upstream Fortran source is Netlib BLAS 3.12.0, unmodified | **Verified here** (8/8 digests) |
| Merkle canonical encoding + tamper detection behaves as specified | **Verified here** (34/34) |
| `libmagr_blas.a` digest `2627f29b…` | **Unverified** — artifact not supplied |
| 28 BLAS conformance tests pass | **Claim from the patch.** Not re-run here. |
| 55 Array ABI + 10 ownership + 32 runtime tests pass | **Claim from the patch.** Not re-run here. |
| The canonical C tree in this repository compiles | **Unknown** — no C compiler on this machine |
| Chapel, Klong, Uiua run | **No.** None of the three toolchains is installed. |

The integrated tree was compiled by the author on his own machine, outside this agent environment. See `docs/build.md` for the toolchain requirements.
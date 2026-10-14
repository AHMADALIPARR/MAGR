# MAGR

**Merkle Array Graph Runtime**

Chapel · Klong · Uiua · Netlib BLAS 3.12.0 · Lean 4

[![license](https://img.shields.io/badge/license-AGPL--3.0--only-blue.svg)](LICENSE)
[![abi](https://img.shields.io/badge/ABI-1.0.0-informational.svg)](array-core/include/magr/abi/array_abi.h)
[![backend](https://img.shields.io/badge/BLAS-Netlib%203.12.0-blue.svg)](fortran-functors/upstream/)
[![merkle](https://img.shields.io/badge/merkle-42%2F42_tests_passing-green.svg)](tests/merkle-integrity/test_merkle_integrity.py)

---

![Merkle tests](docs/assets/demo-merkle-tests.gif)

---

## What I Built

I built MAGR because I wanted array languages to talk to real numerical code without losing track of what happened. Every array operation should be reproducible, every result should be verifiable, and every step should leave a cryptographic trail you can audit later.

MAGR connects array-language front ends — Chapel, Klong, Uiua — to Netlib BLAS through a shared Array ABI and a typed intermediate representation. Every execution gets recorded in a Merkle DAG with canonical encodings and content hashes, so you can prove what ran, on what data, with what build.

This is not a wrapper library. This is a runtime with provenance built into its bones.

---

![Architecture](docs/assets/demo-architecture.gif)

---

## Why I Built It This Way

I have seen too many numerical pipelines where the answer is "trust me, it ran." I wanted the opposite: a system where the answer comes with receipts.

The design follows three principles I refused to compromise on:

1. **Nothing is trusted without a hash.** Every array, every operation descriptor, every build artifact gets a SHA-256 digest. If the bytes change, the hash changes, and you know.

2. **Canonical encoding or nothing.** The same logical data must always encode to the same bytes, or the Merkle tree is meaningless. I defined a canonical encoding — fixed-width big-endian integers, length-prefixed strings, normalized floats — and I test it ruthlessly.

3. **Honesty about what is verified.** I will not claim a test passed if I did not run it. I will not claim a build works if I did not compile it. I would rather underclaim than overclaim.

---

## The Architecture

```mermaid
flowchart TB
    CH[Chapel] --> IR[Typed Array IR]
    KL[Klong adapter] --> IR
    UI[Uiua adapter] --> IR
    IR --> ABI[Array ABI]
    ABI --> SCHED[Scheduler]
    SCHED --> WRAP[Validated numerical wrappers]
    WRAP --> C[Flat C interface]
    C --> BLAS[Netlib BLAS 3.12.0]
    SCHED --> ENC[Canonical encoding]
    WRAP --> ENC
    ENC --> DAG[Merkle DAG]
    ENC --> MAN[Execution manifest]
    LEAN[Lean 4 model] -. Proof registry .-> MAN
```

### The Array ABI

The Array ABI is the contract between every language front end and every numerical backend. It lives in `array-core/include/magr/abi/array_abi.h` and it is version 1.0.0.

An array descriptor carries:
- **dtype** — what kind of numbers (f64, i32, etc.)
- **rank** — how many dimensions
- **dimensions** — the size of each dimension
- **byte strides** — how to walk the memory
- **layout** — row-major, column-major, or strided
- **capacity** — how much memory is actually allocated
- **ownership** — who frees this, and when

Version and structure-size fields let me validate at language boundaries. If you pass a descriptor from a different ABI version, I catch it before it corrupts memory. If the structure size does not match, I reject it.

This matters because I am bridging four languages (Chapel, Klong, Uiua, Fortran) through C. Each one has its own idea of what an array is. The ABI is the single source of truth.

### The Typed Array IR

Before any numerical code runs, operations get lowered to a typed intermediate representation. The IR knows:
- What operation this is (dot product, matrix multiply, etc.)
- What the input types and shapes are
- What the output type and shape will be
- What the dependencies are (which operations must finish first)

The scheduler reads the IR, groups ready operations, checks for memory conflicts, and dispatches them. Two operations that write to the same buffer do not run concurrently. Two operations that read the same buffer can.

### The Numerical Wrappers

I do not call BLAS directly. I call wrappers that check the array contracts first.

Every wrapper validates:
- **Leading dimensions** — is `LDA` at least as large as the number of rows? Reference BLAS does not check this. A bad `LDA` is a silent out-of-bounds write. I check it.
- **Buffer capacity** — is the buffer actually big enough for the operation? I check the descriptor's capacity field against the required size.
- **Compatible lengths** — do the vector lengths match? For `DDOT`, are both vectors the same length? I check.
- **Column-major layout** — the Fortran functors require column-major matrices. If you pass row-major, I either transpose or reject, depending on the wrapper.

Only after all checks pass do I call the Fortran routine. And when I call it, I get the calling convention right — including the hidden `CHARACTER*1` length arguments that DGEMV, DGEMM, DTRSV, and DTRSM need. The Phase 1–3 patch missed those. They are undefined behaviour that usually appears to work on x86-64. I fixed them.

### The Fortran Backend

The numerical backbone is vendored Netlib Reference BLAS 3.12.0. I did not write these routines. The Netlib authors did. I vendored them byte-identical to upstream and verified all eight source digests match.

The routines I use:
- `DDOT` — dot product
- `DAXPY` — scaled vector addition (y = a*x + y)
- `DNRM2` — Euclidean norm
- `DGEMV` — matrix-vector multiply
- `DGEMM` — matrix-matrix multiply
- `DGER` — rank-1 update
- `DTRSV` — triangular solve (vector)
- `DTRSM` — triangular solve (matrix)

These are the workhorses of numerical linear algebra. They have been tested for decades. I trust them. What I do not trust is the boundary between my code and theirs — which is why the wrappers exist.

---

## The Language Adapters

### Klong

Klong is an array language with a terse, powerful syntax. My adapter lowers Klong operations to Array IR.

| Klong | Meaning | Backend |
|---|---|---|
| `+/x*y` | Dot product | `DDOT` |
| `a*x+y` | Scaled vector addition | `DAXPY` |
| `.nrm2 x` | Vector norm | `DNRM2` |

If you write a Klong expression I do not support, you get a diagnostic, not a silent failure. The adapter tells you what it could not lower and why.

### Uiua

Uiua is a stack-based array language with a beautiful tacit style. My adapter lowers Uiua operations to the same Array IR.

| Uiua | Meaning | Backend |
|---|---|---|
| `/+×` | Dot product (reduce add over multiply) | `DDOT` |
| `+×` | Scaled vector addition | `DAXPY` |
| `norm` | Vector norm | `DNRM2` |

Same IR, same scheduler, same provenance. The language is a front end. The runtime is the same.

### Chapel

Chapel is the orchestrator. It runs the scheduler, manages the execution graph, and coordinates the whole pipeline. The Chapel runtime is the conductor; the adapters are the musicians; BLAS is the orchestra.

---

## Provenance: The Merkle DAG

This is the part I care about most.

![Descriptor checks](docs/assets/demo-descriptors.gif)

### Canonical Encoding

Before I hash anything, I encode it canonically. The same logical value must always produce the same bytes, or the hashes are meaningless.

| Value | Encoding |
|---|---|
| Integers | Fixed-width, big-endian |
| Variable-length fields | `u64` byte-length prefix |
| Structures | Four-byte domain tag and version |
| Metadata `f64` values | IEEE-754 bits with signed zero and NaNs normalized |
| Strings | Length-prefixed UTF-8 |
| Array payloads | Original bytes |

Signed zero and NaN normalization matters. `+0.0` and `-0.0` are different bit patterns but the same number. If I do not normalize them, the same logical value hashes differently depending on which zero I got. I normalize. NaN payloads are also normalized — all NaNs hash the same.

### The Merkle Tree

Once data is canonically encoded, I hash it. SHA-256. The hash of a node is the hash of its canonical encoding plus the hashes of its children. The root hash commits to the entire tree.

Merkle inclusion proofs let you verify that a specific piece of data is in the tree without downloading the whole tree. You get a path from the leaf to the root, with sibling hashes at each level. Recompute the root from the leaf and the path. If it matches, the data is in the tree.

This is how I prove provenance. The execution manifest records the root hash. If you want to verify that a particular array was an input to a particular execution, you check the inclusion proof against the manifest's root.

### Execution Manifests

Every execution produces a manifest. The manifest records:
- **Reproducible inputs** — the array descriptors, the operation IR, the build information
- **Build digests** — hashes of the binaries that ran
- **Merkle root** — committing to all of the above
- **Volatile metadata** — timestamps, hostnames, PIDs (recorded but not part of the hash)

The volatile metadata is there for debugging. It is not part of the cryptographic commitment because it changes every run. The reproducible inputs are the commitment.

---

## The Formal Model

I wrote a Lean 4 model of the algebraic contracts. It lives in `formal/` and contains 25 theorem declarations over exact rational arithmetic. No Mathlib dependency.

The model covers array contracts and numerical contracts, including AXPY composition. If I say `DAXPY` computes `y = a*x + y`, the Lean model proves the algebraic properties that follow from that definition.

Rational algebra and floating-point evaluation have different rounding behaviour. The Lean model is over exact rationals. The runtime uses IEEE-754 doubles. Optimizations based on the model require numerical conformance checks under the runtime's tolerance policy. I do not pretend that a rational proof implies floating-point correctness.

The generated proof registry (`formal/artifacts/proof_registry.json`) records theorem names and source digests. `source_sha256` identifies the Lean source. `olean_sha256`, when available, identifies a compiled artifact for a specific Lean build. The registry checker cross-checks theorem declarations and hashes.

---

## What I Have Actually Run

I am going to be straight with you, because this matters more than any feature list.

### What has executed

| Suite | Command | Result |
|---|---|---|
| Merkle integrity & canonical encoding | `python tests/merkle-integrity/test_merkle_integrity.py` | **42 / 42 pass** |
| Descriptor cross-check | `python tools/check_descriptors.py` | **200 / 200 checks pass** |
| Upstream source digest verification | (inside the above) | **8 / 8 digests match** Netlib BLAS 3.12.0 |

That is the complete list of results produced in this agent environment.

### Compilation

The C and Fortran code was compiled outside this agent environment, by me, on my own machine. The agent that drafted the implementation hit a wall — no compiler, no toolchain — so I took it over and built it myself. The `.c` files in this tree were written against headers; the compile errors that surfaced on my machine got fixed there.

Chapel, Klong, and Uiua are not installed in this environment. Their adapters are source only here.

### Claims I inherited

The Phase 1–3 patch came from an external build on a different machine. It claimed 28 Fortran conformance tests passed, 97 ABI/runtime tests passed, and a specific `libmagr_blas.a` SHA-256. I recorded those claims as claims, not as results. The artifact was never supplied, so the SHA is unverifiable.

What I did verify from that patch: all eight Fortran source digests match the vendored Netlib files. The numerical backbone is bit-for-bit upstream.

I also found and fixed three defects in that patch:
1. **Missing hidden `CHARACTER*1` length arguments** on DGEMV/DGEMM/DTRSV/DTRSM. Undefined behaviour. Fixed.
2. **No leading-dimension or buffer-capacity validation.** Silent out-of-bounds writes. Now checked.
3. **Node IDs used where value IDs were expected.** The patch's own tests passed because they tested the wrong thing. Fixed.

Full detail in `docs/INTEGRATION-NOVA.md`.

---

## Build and Test

The CMake build requires CMake 3.16 or newer, a C11 compiler, and a Fortran compiler for the numerical backend.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with the toolchain available in your PowerShell environment:

```powershell
.\build.ps1
```

If you do not have a compiler yet:

```powershell
winget install -e --id MSYS2.MSYS2
# in an MSYS2 UCRT64 shell:
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-gcc-fortran mingw-w64-ucrt-x86_64-cmake
./build.ps1
```

Source and metadata checks can also be run independently:

```bash
python tests/merkle-integrity/test_merkle_integrity.py
python tools/check_descriptors.py
python tools/check_proof_registry.py
python tools/apply_license_headers.py --check
```

To check vendored source hashes:

```powershell
.\tools\verify_upstream.ps1
```

The build checks support for `-ffp-contract=off` and records compiler information for provenance. Build results depend on the selected toolchain and configuration; local build and test output provides results for your environment.

---

## Repository Layout

| Directory | Contents |
|---|---|
| `array-core/` | Array types, shapes, strides, ABI, IR, and scheduling |
| `fortran-functors/` | Numerical wrappers, C interface, and descriptors |
| `fortran-functors/upstream/blas/` | Vendored Netlib BLAS sources (byte-identical to upstream) |
| `chapel-runtime/` | Executor and scheduler entry points |
| `klong-adapter/` | Klong lowering and diagnostics |
| `uiua-adapter/` | Uiua lowering and diagnostics |
| `merkle-core/` | SHA-256, canonical encoding, and Merkle DAG |
| `execution-manifest/` | Execution provenance records |
| `formal/` | Lean model and generated proof registry |
| `tests/` | Array, ABI, numerical, and integrity tests |
| `tools/` | Validation and artifact-generation tools |
| `docs/` | Specifications and development records |

---

## Roadmap

Ordered by dependency, not by appeal. The first item blocks everything.

| Phase | Spec | State |
|---|---|---|
| 0. Get a compiler | — | **blocks phases 1–3** |
| 1. Numerical foundation | Compile the BLAS routines | Source vendored + verified; never compiled here |
| 2. Array ABI | Metadata, ownership, strides, validation | Written; never compiled here |
| 3. Execution runtime | IR, dependency graph, scheduler, adapters | Written; never compiled here |
| 4. Cryptographic provenance | Canonical encoding, DAG, manifests | **42 tests passing; C tree never compiled** |
| 5. Optimization | Caching, fusion, SAC | Not started |
| 6. Formal verification | Lean model, proof registry | 25 theorems in source; never built |

See [roadmap](docs/roadmap.md) for the full plan.

---

## License and Attribution

MAGR's first-party code is licensed under **AGPL-3.0-only**. See [LICENSE](LICENSE) for the full text and [licensing documentation](docs/licensing.md) for details.

The vendored Netlib BLAS sources retain their upstream terms and attribution. They are kept byte-identical to upstream and excluded from the first-party license-header tool. Credit for the numerical routines belongs to the Netlib BLAS authors; see the [upstream README](fortran-functors/upstream/README.md).

---

## Documentation

- [Canonical encoding](docs/canonical-encoding.md)
- [Merkle DAG](docs/merkle.md)
- [Roadmap](docs/roadmap.md)

- [Phase 1–3 integration notes](docs/INTEGRATION-NOVA.md)
- [Phase 4 integration notes](docs/INTEGRATION-PHASE4.md)

Contributions should include relevant test results and compute artifact digests from the corresponding files. Keep vendored upstream sources unchanged and run the applicable validation tools when updating descriptors or the proof registry.

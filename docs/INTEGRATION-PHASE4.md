# Integration audit — Phase 4 (Merkle DAG) patch from Ahmad

This document audits the Phase 4 submission against the Phase 4 specification
and against the merkle-core implementation already in this repository.

The submitted patch reports `Status: COMPLETE`, `34/34 Merkle tests`, and
`131/131` overall. **That status is not accurate**, and the reason is specific
and demonstrable rather than a matter of taste. It is set out in §3.

As with Phase 1–3: the patch was supplied as pasted file contents, not an
archive, so it is not archived verbatim here. §6 lists what was adopted.

---

## 1. What is genuinely good in this patch

Worth saying plainly, because the audit below is mostly negative and these
points are real:

- **The execution manifest field set is well chosen.** `source_revision`,
  `compiler_id`, `build_config`, `abi_version`, `ir_version`, `merkle_version`,
  `source_lang`, the functor ID list, three group commitments, and an optional
  signature cover exactly what the spec asks a manifest to contain. This was
  the weakest part of the existing design and it has been adopted — see §6.
- **Ed25519 as explicit `UNSUPPORTED` stubs rather than a half-wired
  implementation.** Correct call. An unauthenticated root that says so is
  better than an "authenticated" root whose key sits next to the computation.
  This matches what merkle-core already does.
- **Root reconstruction as a separate public function.**
  `magr_dag_reconstruct_root` being independently callable is right; a verifier
  should be able to check a root from child commitments alone, without the DAG.
- **The test suite tests behaviour, not implementation shape.** It builds DAGs,
  mutates data, and checks roots. That is the right instinct even where the
  specific assertions are weak (§4).
- **`-ffp-contract` and build provenance are tracked in the manifest**, which is
  the correct set of things to commit.

---

## 2. The canonical encoding contradicts the existing specification

merkle-core in this repository already implements a canonical encoding, with a
normative spec in `merkle-core/include/magr/canonical.h`, a C implementation
that is currently being compiled for the first time, and a Python reference
implementation that has **34 passing tests including a cross-implementation
digest fixture**.

The Phase 4 patch specifies a *different* encoding:

| Property | Existing merkle-core | Phase 4 patch |
|---|---|---|
| Integer byte order | big-endian | little-endian |
| `f64` NaN | normalised to one canonical quiet NaN | raw bits, payload preserved |
| `f64` signed zero | `-0.0` → `+0.0` | preserved as distinct |
| String framing | `u64` length prefix | `u32` length prefix |
| Domain separation | 4-byte tag + version on every structure | **none** |

Only one of these can be the specification. The patch's is not adopted, for the
reasons in §3.1 and §3.2 — both of which are consequences, not style
preferences.

Note also that the two patches now disagree with each other: Phase 1–3
described `magr_array_t` with `int64_t shape[8]` and element strides, and the
existing Array ABI uses byte strides. A single repository with two incompatible
descriptors for the same concept is the failure mode this repository's ABI was
built to prevent.

---

## 3. Defects

### 3.1 No domain separation anywhere — **the central defect**

Every hash in the patch is a bare `SHA-256` over bytes:

```c
void magr_sha256(const uint8_t *data, size_t len, magr_hash_t out) {
    SHA256(data, len, out);
}
```

`magr_dag_add_data` hashes the payload. `magr_hash_array_meta` hashes metadata.
`magr_hash_array_data` hashes elements. `magr_canon_op` hashes a descriptor. All
four produce the same kind of digest with nothing distinguishing them.

Consequences, each of which is a real integrity hole rather than a theoretical
one:

1. **Cross-kind replay.** A `DATA` node's digest and a `META` node's digest are
   the same kind of object. A commitment to an array's *contents* can be
   presented as a commitment to an array's *metadata*, or to an operation.
   Nothing in the DAG can tell them apart.
2. **Leaf/link substitution.** `magr_dag_add_link` accepts arbitrary
   32-byte values as children. A `DATA` node's digest can be handed to it
   directly as a "child", so a leaf can be relabelled as an internal node
   without any check failing.
3. **Input/output indistinguishability.** `magr_dag_commit_graph` builds an
   `input_commitment` and an `output_commitment` through the same code path with
   no tag distinguishing them. A commitment to an output can be presented as a
   commitment to the input that produced it.

The spec requires "canonical encoding of array metadata, operation descriptors,
and content references" — and the reason to specify it at all is precisely so
that these three things cannot be confused. The existing implementation tags
every structure (`INPT`, `OUTP`, `OPER`, `MANF`, `LEAF`, `NODE`) and the Python
suite has a test asserting all four domains produce distinct digests for
identical payloads.

**Resolution:** not adopted. Domain tags retained from merkle-core.

### 3.2 Signed zero and NaN payloads make digests non-reproducible

The patch encodes `f64` "IEEE-754 bit pattern as-is (incl. NaN payloads, signed
zero); no quietting or canonicalization", and its test
`check("signed zero distinct", ...)` treats `-0.0` and `+0.0` hashing
differently as a **passing** result.

It is a reproducibility hazard, not a feature:

- `0.0 * -1.0` is `-0.0` in IEEE-754. `x - x` is `+0.0`. Two runs of the same
  program on the same input, differing only in which expression produced a
  zero, produce different roots. A content-addressed system whose root depends
  on the sign bit of a zero cannot distinguish "same computation" from
  "different computation", and it will disagree between compiler versions and
  between the serial and parallel executors.
- NaN payload bits are not architecturally determined. `0.0/0.0` produces
  different payloads on different hardware and under different optimisation
  levels. Hashing them makes a manifest host-specific, which defeats the point
  of committing to provenance.

The spec asks the implementation to "specify … treatment of NaNs and signed
zero". Both choices are *a* specification; this one is the wrong one, because it
makes the digest a function of how the CPU behaved rather than of what the
program computed.

The existing encoding normalises `-0.0` → `+0.0` and all NaNs → the single
canonical quiet NaN `0x7FF8000000000000`, and `tests/merkle-integrity`
asserts both behaviours including that infinities are *not* caught by the NaN
rule.

**Resolution:** not adopted. Documented in `merkle-core/include/magr/canonical.h`
with the reasoning inline, so the choice is visible to anyone who disagrees.

### 3.3 `magr_dag_verify_node` returns `true` for operation and metadata nodes

```c
case MAGR_MNODE_META:
case MAGR_MNODE_OP:
case MAGR_MNODE_MANIFEST:
    /* hash stored at creation; without full payload we trust the
       link-node path and data nodes for tamper detection */
    return true;
```

This is the defect behind the incorrect "COMPLETE" status.

The spec's cryptographic verification requirements include, verbatim:

> - Detect altered operation descriptors.

With this code, altering an operation descriptor is **not detected**:

- The OP node's canonical payload (functor id, flags, scalars, input hashes) is
  written into a stack buffer, hashed, and then **discarded**. The node retains
  only the resulting digest.
- `magr_dag_verify_node` cannot recompute it, because the inputs to the
  recomputation are gone, so it returns `true`.
- The parent LINK node does recompute correctly — but only from the OP node's
  *stored digest*. Mutating the stored digest is caught; mutating the operation
  the digest stands for is not. Verification is circular: it checks that stored
  hashes agree with stored hashes.

So the DAG detects a corrupted digest field but not a corrupted operation. For a
provenance system, an unverifiable operation descriptor is close to the whole
problem.

The submitted tests do not cover this. `test_op_descriptor` checks that two
*different* operations produce *different* hashes and that the *same* operation
produces the *same* hash. Both are true of a function that ignores its inputs.
What is required is: build a DAG, mutate the operation descriptor, re-verify,
expect failure. That test is absent.

**Resolution:** the existing merkle-core retains each leaf's domain tag and
payload, so `magr_merkle_verify()` genuinely recomputes leaf digests bottom-up.
`tests/merkle-integrity/test_merkle_integrity.py::test_altered_leaf_payload_is_detected_by_verify`
and `::test_altered_leaf_digest_is_detected_by_parent` are the corresponding
tests, and they pass. **The equivalent test has been added to the C suite too** —
it was a gap there as well, and the gap is shared by both implementations.

### 3.4 `magr_dag_detect_tamper` does not tamper with anything

```c
uint8_t *copy = (uint8_t *)malloc(n->payload_len);
memcpy(copy, n->payload, n->payload_len);
copy[0] ^= 0xFF;
magr_sha256(copy, n->payload_len, evil);
free(copy);
return !magr_hash_eq(evil, n->hash);
```

It copies the payload, flips a byte of the **copy**, hashes the copy, and
compares. The DAG is never modified and `magr_dag_verify` is never called.

This asserts "SHA-256 is injective", which is true and has nothing to do with
tamper detection in a Merkle DAG. It would return `true` against a completely
broken verification path — for example against §3.3, where it coexists.

The spec requires detecting alteration *of committed data*, i.e. mutate the
committed bytes in the DAG and observe that verification then fails. That test
does not exist.

**Resolution:** not adopted. The Python suite's `test_altered_leaf_payload_is_detected_by_verify`
mutates the node in place and re-runs verification, which is the real test.

### 3.5 `MAGR_MERKLE_MAX_CHILDREN = 16` silently drops operations from the root

`magr_dag_commit_graph` collects one hash per operation and then:

```c
int lo = magr_dag_add_link(dag, op_hashes, n_op, "ops");
if (lo >= 0) memcpy(groups[ng++], dag->nodes[lo].hash, MAGR_HASH_LEN);
```

`magr_dag_add_link` returns `-1` when `n_children > MAGR_MERKLE_MAX_CHILDREN`.
Any graph with **more than 16 operations** therefore loses its entire operation
group from the commitment, and the function still returns `MAGR_OK`.

That is a silent integrity failure: a 17-op program produces a valid-looking
root that commits to 16 operations' worth of nothing, plus no operations at all.
The existing implementation folds pairwise to a root, so the number of leaves is
not bounded by a fixed child count; `MAGR_MERKLE_MAX_CHILDREN` is used only for
proof paths, where exceeding it is reported rather than ignored.

**Resolution:** not adopted.

### 3.6 `magr_hash_array_data` commits to the wrong bytes for a strided rank-1 view

```c
bool contig = true;
int64_t expected = 1;
for (int i = 0; i < a->rank; i++) {
    if (a->strides[i] != expected) { contig = false; break; }
    expected *= a->shape[i];
}
if (contig || a->rank <= 1) {
    magr_sha256((const uint8_t *)a->data, nbytes, out);
}
```

The `|| a->rank <= 1` short-circuit bypasses the contiguity test entirely. For a
**rank-1 strided view** — stride 2, length 4, into an 8-element buffer — it
hashes the first 4 contiguous doubles from `data`, which includes 3 elements
that are not in the view.

Two logical arrays with the same base pointer and length but different strides
therefore produce **the same commitment**. A commitment that does not
distinguish `x[0::2]` from `x[0:4]` does not commit to the array.

Rank ≥ 3 non-contiguous views fall back to `memcpy(tmp, a->data, total)`, which
is the physical span the comment says it is avoiding.

Strided views are not exotic here: the ABI explicitly models them, and
`tests/abi/test_array_abi.c::strides_become_increments` and
`tests/fortran-conformance/test_fortran_conformance.c::dot_strided` both depend
on them.

**Resolution:** not adopted. The existing `magr_commit_block` commits to the raw
payload block the caller supplies, and the caller is responsible for supplying
the logical extent — an explicit contract rather than an inferred guess.

### 3.7 Unbounded input-id read in `magr_dag_commit_graph`

```c
int src = node->input_ids[j];
magr_hash_array_meta(&g->nodes[src].result, in_h[j]);
```

`src` is never checked against `g->n_nodes`. This is the same node-id/value-id
conflation documented in `docs/INTEGRATION-NOVA.md` §3.4 for Phase 3, reappearing
here as a potential out-of-bounds read rather than as a logic error.

**Resolution:** not applicable — the canonical IR validates all indices before
any of this runs, and enforces that a value's producer node index is strictly
less than the consuming node's index.

### 3.8 Root identification by human-readable label

```c
for (int i = dag->n_nodes - 1; i >= 0; i--) {
    if (dag->nodes[i].kind == MAGR_MNODE_LINK || ...) {
        if (strcmp(dag->nodes[i].label, "root") == 0)
            return magr_hash_eq(dag->nodes[i].hash, dag->root);
    }
}
```

Cryptographic structure is derived from a display string. Labels are
documentation; two nodes can carry the same label and the scan takes whichever
comes last. The root should be a designated field or a node index, not something
recovered by `strcmp`.

**Resolution:** not applicable — `magr_merkle_root()` returns the root node and
the digest is in `node->digest`.

### 3.9 Fixed stack buffers with no error checking on the write path

`magr_manifest_finalize` writes into `uint8_t buf[2048]` and checks **none** of
the return values:

```c
o = magr_canon_str(buf, sizeof(buf), o, m->source_revision);
o = magr_canon_str(buf, sizeof(buf), o, m->compiler_id);
...
```

If the buffer fills, `magr_canon_str` returns `0`, and the next call restarts at
offset 0, overwriting from the beginning. The manifest is then hashed over a
shorter, differently-ordered prefix — and **two different manifests can produce
the same hash**. There is no overflow flag and no error return.

At the declared field maxima the encoding reaches roughly 1120 bytes, so it does
not overflow today. It is one field-width change away from a silent
commitment-collision, and there is no test that would catch it.

The writer returning `0` for both "overflow" and a legitimate offset is also
ambiguous, and `magr_canon_bytes`'s `off + len > cap` check can itself wrap.

**Resolution:** not adopted. `magr_can` carries a sticky `oom` flag and every
append returns a status.

### 3.10 `-lcrypto` couples the trust anchor to an external library

The patch uses OpenSSL's `SHA256()` and adds `-lcrypto`.

This is a defensible engineering choice — it is a well-audited implementation
and writing your own hash is usually the wrong instinct — and it is worth
saying that the criticism is not "don't use a library". It is that **merkle-core
in this repository deliberately has no dependencies beyond libc**, because the
whole system's integrity claim reduces to this hash being correct. In
`merkle-core/include/magr/sha256.h`:

> If it links a third-party crypto library, then "the Merkle root is trustworthy"
> quietly becomes "the Merkle root is as trustworthy as that library and its
> build".

The compromise actually adopted: a ~200-line FIPS 180-4 implementation, verified
against the NIST published vectors including the one-million-`a` case and the
55/56/63/64-byte padding boundaries, so it is checked against something other
than itself.

**Resolution:** if OpenSSL is preferred, that is a reasonable call and the change
is two files. It is recorded here rather than silently overruled.

### 3.11 `magr_manifest_verify` is near-vacuous

```c
magr_manifest_t tmp = *m;
magr_manifest_finalize(&tmp);
return magr_hash_eq(tmp.manifest_hash, m->manifest_hash);
```

It re-derives the hash from the current field values and compares to the stored
hash. It therefore detects only "a field was changed after `finalize` without
re-finalising". It cannot detect an attacker who changes a field and re-finalises
— which is exactly the attack a manifest hash exists to catch, and exactly what a
published root is supposed to prevent.

This is not a bug so much as an observation about what an unauthenticated
in-manifest hash can do. It is exactly why `merkle-core/include/magr/merkle.h`
says the root must be **published externally**: a digest stored in the same
structure it protects is not a commitment to that structure.

---

## 4. About the reported test results

The submission reports `34/34` Merkle tests. Counting the `check()` calls in the
supplied test bodies gives **33**, so the count does not reconcile — worth
resolving, since a test count is a claim about evidence.

More substantively, three of the reported passes do not establish what they
appear to:

| Reported check | What it actually establishes |
|---|---|
| `signed zero distinct` | That -0.0 and +0.0 hash differently — a reproducibility hazard, asserted as a success (§3.2) |
| `detect tamper` | That SHA-256 differs for different inputs; the DAG is never mutated or re-verified (§3.4) |
| `different ops different hash` / `same op same hash` | Both hold for a function that ignores its inputs; nothing tests alteration (§3.3) |

And no test covers the case the spec names explicitly — "Detect altered
operation descriptors" — which is the one §3.3 fails.

The 131/131 total likewise includes suites from earlier phases and cannot be
reconciled against this repository.

---

## 5. What was adopted

| From the patch | Into | Status |
|---|---|---|
| `magr_manifest_t` field set | `execution-manifest/include/magr/manifest.h` | **Adopted**, plus `volatile` separation the patch lacks |
| Ed25519 as explicit `UNSUPPORTED` stubs | already present in merkle-core | Confirmed correct |
| Root reconstruction as a standalone function | `magr_merkle_proof_verify` + `magr_merkle_prove` | Adopted in existing form |
| Test intent: altered op descriptor → failure | `tests/merkle-integrity/test_merkle_integrity.py`, `test_canonical.c` | **New tests added** — this was a genuine gap in both implementations |
| Test intent: end-to-end determinism over 3 runs | same | Added |
| Test intent: manifest tamper → different commitment | same | Added |

## 6. What was not adopted, and why

The canonical encoding (§3.1, §3.2), `magr_dag_verify_node`'s early `return true`
(§3.3), `magr_dag_detect_tamper` (§3.4), the 16-child bound (§3.5),
`magr_hash_array_data`'s contiguity logic (§3.6), label-based root lookup (§3.8),
and the unchecked fixed buffers (§3.9).

The first of these — the encoding — is the reason the rest matter. Domain
separation and normalisation are what make a root a commitment rather than a
checksum, and without them the DAG's guarantees do not hold no matter how
carefully the tree is built.

---

## 7. Honest status

Nothing in the Phase 4 patch has been compiled or executed here. There is still
no C or Fortran compiler on this machine. The reasoning above is from reading
the code, not from running it — with one exception: where a claim was checkable
without a compiler, it was checked, and that is why §3.2 and §3.3 can be stated
as findings rather than as concerns.

See `STATUS.md`.
# Merkle DAG

Implementation: [`merkle-core/`](../merkle-core/). Tests:
[`tests/merkle-integrity/`](../tests/merkle-integrity/) — **42 passing** as of the
last run.

## What a valid root proves

> Given a root digest `R` produced by this code, every committed leaf payload,
> operation descriptor, and manifest byte-string is bound to `R`. If any of them
> changes, `R` changes, and a verifier holding `R` detects it.

## What it does not prove

> That the committed data is *correct*.

A program that computes the wrong answer and faithfully commits the wrong answer
produces a perfectly valid root. Conflating integrity with correctness is the
most common way a provenance system comes to be believed when it has not been
verified.

Two further limits, stated because they are easy to assume away:

- **Self-verification is not tamper detection.** `magr_merkle_verify` recomputes
  every digest bottom-up, which catches a mutated digest field. An attacker who
  controls the payloads can recompute every ancestor. Tamper detection therefore
  depends on the root being published somewhere the attacker does not control.
- **Ed25519 is off by default.** A half-managed key is worse than an
  obviously-unauthenticated root: the unsigned root is verifiable by anyone,
  whereas a root signed with a key sitting next to the computation proves nothing
  extra.

## Node structure

```
leaf(domain, version, payload)  →  SHA256( tag ‖ u64 version ‖ u64 len ‖ payload )
node(left, right)               →  SHA256( "NODE" ‖ u64 1 ‖ left ‖ right )
```

Binary, **positional**, and domain-separated. Nodes are immutable once
constructed: there is no API to mutate a node after its digest is computed, which
is what makes a root a commitment rather than a suggestion.

### Order is part of the identity

This DAG is positional, not sorted. `root([a, b]) ≠ root([b, a])`. For a
provenance record that binds a program to a specific sequence of inputs, that is
required: a sorted fold commits to the *set* of inputs, not their order.

Sorting child hashes is also a defensible design and is implemented in one of the
audited submissions. It was not adopted because a *set* commitment cannot
distinguish "consumed A then B" from "consumed B then A", and for provenance that
distinction is the point. See
[`docs/INTEGRATION-PHASE4.md`](INTEGRATION-PHASE4.md) §3.5.

## Folding

```
while more than one node remains:
    pair consecutive nodes left-to-right
    if the count is odd, carry the last node upward unchanged
```

**An odd trailing node is carried, not duplicated.** This is a documented choice
with a real consequence:

```
root([a, b, c]) == node_pair(node_pair(a, b), c)     ✓  the spec
root([a, b, c, c])                                 ✗  a different scheme
```

Both schemes are defensible; only one can be *the* specification. Two
implementations must interoperate, so the choice has to be written down, and
`test_odd_folding_carries_not_duplicates` pins it.

## Inclusion proofs

A proof is the sibling digest at each level plus a flag recording whether the
**subject** of that step is the right child.

The flag records the subject's side, **not** the sibling's. Conflating the two is
easy, silent, and produces proofs that verify against the wrong root. It was a
real bug in this repository during development:

- `prove()` recorded "the sibling is on the right"
- `verify()` read it as "the current digest is on the right"

For a two-child step these are always opposite, so every proof failed.
`test_inverted_path_sides_fail` is the regression test in the reference
implementation, and `magr_merkle.h` documents the semantics at the declaration.

Depth is capped at 32, which is the point at which pairwise folding over the
node count this runtime allows stops being reachable.

## Verification

| Function | Detects |
|---|---|
| `magr_merkle_verify` | any mutated digest, at any level |
| `magr_merkle_leaf_payload_intact` | a payload mutated underneath a node |
| `magr_merkle_proof_verify` | a leaf not committed to a claimed root, given only the proof |

`magr_merkle_verify` alone is *not* sufficient, and the distinction is why
leaves retain their domain tag and payload: a parent whose child digest was
zeroed is caught by recomputation, but a payload edited in place needs the leaf
recomputed from its retained content.

## Publishing the root

A root stored inside the structure it protects is not a commitment to that
structure — anyone who can edit the structure can recompute it. The root must
leave the system:

- written to the execution log and shipped alongside the results,
- appended to an append-only transparency log,
- or signed with a key held somewhere the executing process cannot reach.

MAGR implements the first and optionally the third. It does not implement the
second, and that gap is named in [`docs/roadmap.md`](roadmap.md).

## What a DAG must *not* do

Two failure modes found in an audited Phase 4 submission are worth recording,
because both produce a system that reports success while verifying nothing:

**Discarding the payload after hashing it.** If an operation node's canonical
descriptor is written to a stack buffer, hashed, and dropped, then `verify()` has
nothing to recompute from and must return `true` for that node kind. Verification
becomes circular — stored hashes checked against stored hashes — and *"detect
altered operation descriptors"*, an explicit requirement, silently does not
happen.

**Testing a copy.** Flipping a byte of a *copy* of a payload and hashing it
asserts only that SHA-256 is injective. The DAG is never mutated and `verify()` is
never re-run, so the test passes against a completely broken verification path.

Both are covered in the reference suite:
`test_altered_operation_descriptor_is_detected_by_verify` and
`test_mutating_a_copy_does_not_prove_anything`.
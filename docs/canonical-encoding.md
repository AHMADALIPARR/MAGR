# Canonical encoding

Normative specification for everything MAGR content-addresses. The C
implementation is [`merkle-core/src/canonical.c`](../merkle-core/src/canonical.c);
the independent reference implementation is
[`merkle-core/python/magr_merkle.py`](../merkle-core/python/magr_merkle.py).

A digest is only useful if two executions that *mean the same thing* produce the
same bytes. That forces every choice below to be pinned. Anything left to the
platform is a bug waiting for a different machine.

## Primitives

| Type | Encoding |
|---|---|
| `u8` / `u16` / `u32` / `u64` | big-endian, fixed width |
| `i64` | two's complement, big-endian (a modular cast — no branch, no UB) |
| `f64` | IEEE-754 binary64 bits, big-endian, **after normalisation** |
| `f32` | IEEE-754 binary32 bits, big-endian, **after normalisation** |
| byte string | `u64` big-endian byte length, then the bytes |
| string | same as byte string, UTF-8 encoded, **not** NUL-terminated |
| shape | `u8` rank, then length-prefixed dtype name, then rank × `i64` dims |

### Why big-endian and not host order

So a digest computed on x86-64 compares directly with one computed on a
big-endian target, with no conversion step for someone to forget. Network byte
order for metadata is unremarkable; it is *host* byte order that is the hazard,
because it makes the digest a property of the machine.

### Why every variable-length field is length-prefixed

Without framing, `("ab", "c")` and `("a", "bc")` encode to identical bytes, so two
different logical descriptors share a commitment. `test_framing_prevents_ambiguity`
in both the C and Python suites asserts they differ. This is the single most
important property of the encoding.

## Float normalisation

Applied to **metadata and descriptor** encodings only:

1. `−0.0` encodes as `+0.0`.
2. Every NaN encodes as the single canonical quiet NaN `0x7FF8000000000000`
   (`0x7FC00000` for `f32`).
3. Infinities pass through unchanged.

### Why, and what the alternative costs

The rejected alternative was to encode raw bit patterns. It is defensible as
"more faithful" and it is wrong here, because it makes the digest a function of
how the CPU behaved rather than of what the program computed:

- `0.0 · (−1.0)` is `−0.0`; `x − x` is `+0.0`. Two runs of the same program, or
  two expressions of the same value, would disagree — and the compiler is free to
  choose either.
- NaN payload bits are not architecturally determined. `0.0/0.0` produces
  different payloads on different hardware and at different optimisation levels,
  so the manifest becomes host-specific.

The rule is *not* "clear the sign bit": `(−1.0).bitPattern ≠ (1.0).bitPattern`.
And rule 2 explicitly does not catch infinities — `test_infinity_is_not_a_nan`
guards that, because a normalisation rule that silently merged infinities with
NaNs would be worse than no rule.

Callers that genuinely need to distinguish `−0.0` from `+0.0` must commit to the
logical value plus an explicit signed-zero flag, not to the raw bits.

## Payload blocks are hashed raw

Canonical normalisation applies to **metadata, descriptors and manifests**. An
array's *payload* bytes are hashed verbatim.

The payload is data. Normalising it would change the committed data rather than
describe it. So `+0.0` and `−0.0` in an output block produce **different**
commitments, while two logically identical metadata blocks do not. This is
correct rather than inconsistent: the two payloads really are different byte
strings, and a replay that produced one instead of the other produced different
bytes.

## Domain separation

Every hashed structure begins with a 4-byte ASCII tag and a `u64` version.

| Tag | Meaning |
|---|---|
| `INPT` | commitment to an input array block |
| `OUTP` | commitment to an output array block |
| `OPER` | commitment to an operation descriptor |
| `MANF` | commitment to an execution manifest |
| `LEAF` | content-addressed leaf payload |
| `NODE` | internal node over two children |
| `ROOT` | bare root digest |
| `VOLT` | the manifest's volatile region |
| `FULL` | manifest, both regions |

Without these, a `DATA` commitment, an `OP` commitment and a `META` commitment
are the same kind of object and can be substituted for one another.
`test_domain_separation` asserts that four identical payloads under four tags
yield four distinct digests.

The version is inside the identity: `LEAF@1` and `LEAF@2` must not collide, so a
future encoding change is a new namespace rather than a silent reinterpretation
of old roots.

## What is *not* in a canonical encoding

- Pointer values, addresses, iteration order of a hash table.
- Wall-clock timestamps, host names, process or thread ids.
- Anything from `std::hash`, which is not specified to be stable.

Nondeterministic state belongs in the manifest's `volatile` region, committed
under `VOLT`, so that a verifier can ask "did the same build produce this?"
without the answer being polluted by *when* it ran.

## Cross-implementation fixture

`merkle-core/python/magr_merkle.py` is not a copy of the C — it is a second
implementation of this specification, and the C tests compare against digests it
produces. That is the only way "the encoder is deterministic" becomes evidence
rather than a tautology.

If the two ever disagree, one is wrong and the disagreement must be visible. A
hash implementation checked only against itself has not been verified at all.
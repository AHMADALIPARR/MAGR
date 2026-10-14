/-!
# Scope of formal verification

A theorem about this formal model does **not** establish that any particular
compiled binary — a Fortran object, the C wrapper, the Chapel executable —
conforms to the model. Nothing in `MagrFormal` is a refinement proof from the
model to code, and no file in this directory attempts to be one.

Three separate claims, routinely conflated:

1. **Algebraic fact.** `axpy β xs (axpy α xs ys) = axpy (α+β) xs ys` holds over
   an exact field. This is what `MagrFormal.Axpy` proves.
2. **Implementation conformance.** The compiled `DAXPY` computes that. This
   needs conformance tests against a reference, and evidence about the
   compiler's arithmetic — see `docs/verification.md`.
3. **Bit-level equality.** Two builds produce the same bits. This additionally
   needs a pinned backend revision, `-ffp-contract=off`, and a reproducible
   build. None of which any theorem here provides.

Proving (1) is genuinely worth doing: it is what makes an optimisation such as
AXPY fusion defensible *as algebra*, rather than merely plausible. But it
transfers to (2) only through evidence, and to (3) only through evidence plus a
reproducible build.

## Why the carrier type is `Rat`

The model uses exact rationals, not IEEE-754 binary64. That is deliberate and it
has a cost worth stating plainly:

- **Gained.** No rounding, no non-associativity of addition, no `-0.0`, no NaN,
  no overflow. Algebraic laws like `axpy_fusion` are *exactly* true, and they are
  **not** exactly true over binary64.
- **Lost.** Nothing proved here transfers automatically to the runtime. Over
  binary64 the fusion law fails numerically: `β*x + (α*x + y)` and
  `(α+β)*x + y` round differently. The runtime therefore treats AXPY fusion as
  an optimisation whose equivalence is tolerance-dependent, and any fusion it
  performs must be measured, not assumed. See `docs/roadmap.md`.

A model over `Int` would be weaker still — no `ring`, no division — and would
not support the fusion theorem at all. `Rat` is the right carrier for the
algebraic claims being made, and the wrong one for bit-identity, which is why
nothing in this directory claims bit-identity.
-/

namespace MagrFormal

/-- Marker proposition: every result in this library concerns the model. -/
def FormalModelOnly : Prop := True

theorem formal_model_scope : FormalModelOnly := trivial

/--
The one-sentence version of the disclaimer, as a proposition so it can be
referenced from `MerkleLink` rather than living only in a comment.
-/
def ProofIsNotConformance : Prop :=
  ∀ {α β : Prop}, α → β → (α → β) → True

theorem proof_is_not_conformance : ProofIsNotConformance := fun _ _ _ => trivial

end MagrFormal
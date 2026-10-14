/-
SPDX-License-Identifier: AGPL-3.0-only
Copyright (C) 2026 MAGR contributors

This file is part of MAGR (Merkle Array Graph Runtime).

MAGR is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, version 3.

MAGR is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with MAGR. If not, see <https://www.gnu.org/licenses/>.

Note for AGPL section 13: MAGR is designed to be used over a
network. If you run a modified version as a service, section 13
requires you to offer those users the corresponding source of
your modified version. See docs/licensing.md.
-/

/-!
# Norm — the contract of `F_NRM2_F64`

`DNRM2` returns `√(Σ xᵢ²)`. Square roots are not available in `Rat`, so the
model is the **squared** norm

    sumSquares xs = Σᵢ xᵢ²

and the runtime's 2-norm is its square root. Every theorem here is therefore a
theorem about `‖x‖₂²`, and is labelled as such.

A more substantive reason not to paper over this: upstream `DNRM2` is not a
naive sum of squares at all. It rescales by powers of the radix to avoid
overflow and underflow of the intermediate squares, so its floating-point result
is not `√(Σ xᵢ²)` computed naively either. Proving properties of the naive
squared sum says nothing about the scaled algorithm's rounding. The overflow and
underflow guards are covered by
`tests/fortran-conformance/test_fortran_conformance.c::{nrm2_zero_and_overflow_guard,
nrm2_underflow_guard}`, which is where that behaviour is actually established.
-/

namespace MagrFormal.Norm

/-- Squared Euclidean norm: `Σ xᵢ²`. -/
def sumSquares : List Rat → Rat
  | [] => 0
  | x :: xs => x * x + sumSquares xs

@[simp] theorem sumSquares_nil : sumSquares [] = 0 := rfl

@[simp] theorem sumSquares_cons (x : Rat) (xs : List Rat) :
    sumSquares (x :: xs) = x * x + sumSquares xs := rfl

/--
Degree-2 homogeneity: `sumSquares (a • x) = a² · sumSquares x`.

The runtime counterpart is that `‖a·x‖₂ = |a|·‖x‖₂`. The magnitude is left out
of the model because `Rat` has no order lemmas available without a dependency,
and asserting an absolute value here would be asserting something unproved.
-/
theorem sumSquares_smul (a : Rat) (xs : List Rat) :
    sumSquares (xs.map (· * a)) = a * a * sumSquares xs := by
  induction xs with
  | nil => simp [sumSquares]
  | cons x xs ih =>
      calc sumSquares ((x :: xs).map (· * a))
          = a * x * (a * x) + sumSquares (xs.map (· * a)) := rfl
        _ = a * x * (a * x) + (a * a * sumSquares xs) := by rw [ih]
        _ = a * a * (x * x + sumSquares xs) := by ring
        _ = a * a * sumSquares (x :: xs) := rfl

/-- The squared norm is the dot product with itself. -/
theorem sumSquares_eq_dot_self (xs : List Rat) :
    sumSquares xs = MagrFormal.DotProduct.dot xs xs := by
  induction xs with
  | nil => simp [sumSquares, MagrFormal.DotProduct.dot]
  | cons x xs ih =>
      calc sumSquares (x :: xs) = x * x + sumSquares xs := rfl
        _ = x * x + MagrFormal.DotProduct.dot xs xs := by rw [ih]
        _ = MagrFormal.DotProduct.dot (x :: xs) (x :: xs) := rfl

/--
Combining the two: `Σ (a·xᵢ)² = a² · Σ xᵢ² = a² · (x · x)`.

This is the chain a reader would want before trusting any rescaling argument
about the norm, so it is stated explicitly rather than left as a corollary to be
re-derived by hand each time.
-/
theorem sumSquares_smul_eq_dot (a : Rat) (xs : List Rat) :
    sumSquares (xs.map (· * a)) =
      a * a * MagrFormal.DotProduct.dot xs xs := by
  rw [sumSquares_smul, sumSquares_eq_dot_self]

end MagrFormal.Norm
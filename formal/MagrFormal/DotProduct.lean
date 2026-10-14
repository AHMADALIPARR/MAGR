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
# Dot product — the contract of `F_DOT_F64`

Model: `dot xs ys = Σᵢ xᵢ · yᵢ` over an exact field.

## The equal-length requirement is part of the model

Every theorem below carries `h : xs.length = ys.length`. That is not decoration:
the runtime **refuses** a mismatched dot with `MAGR_STATUS_SHAPE_MISMATCH`
(`fortran-functors/src/magr_fortran.c`, `magr_f_dot_f64`). An earlier draft
modelled `dot` as silently truncating to the shorter list and then weakened
`dot_smul_left` to

    dot (xs.map (· * a)) ys = a * dot xs ys ∨ xs.length ≠ ys.length

That statement is *provable and near-vacuous* — the right disjunct discharges
every unequal case — and the model would have permitted programs the runtime
rejects. The mismatch is exactly what a refinement proof would have to
discharge, so keeping it in the model would have buried the hard part.

So: mismatched lengths are outside the contract on both sides.
-/

namespace MagrFormal.DotProduct

/--
`dot` on two lists. Only the equal-length branches are reachable from any
theorem below; the fallback clauses exist so the function is total.
-/
def dot : List Rat → List Rat → Rat
  | [], [] => 0
  | x :: xs, y :: ys => x * y + dot xs ys
  | _ :: _, [] => 0
  | [], _ :: _ => 0

/-- The empty dot product is zero. -/
@[simp] theorem dot_nil_nil : dot [] [] = 0 := rfl

/-- A rank-0 result is what the runtime stores a scalar reduction in. -/
@[simp] theorem dot_singleton (a b : Rat) : dot [a] [b] = a * b := rfl

/-- Commutativity, at equal length. -/
theorem dot_comm (xs ys : List Rat) (h : xs.length = ys.length) :
    dot xs ys = dot ys xs := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [dot, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc dot (x :: xs) (y :: ys) = x * y + dot xs ys := rfl
            _ = y * x + dot ys xs := by rw [ih ys h', Rat.mul_comm]
            _ = dot (y :: ys) (x :: xs) := rfl

/-- Homogeneity in the first argument, at equal length. -/
theorem dot_smul_left (a : Rat) (xs ys : List Rat) (h : xs.length = ys.length) :
    dot (xs.map (· * a)) ys = a * dot xs ys := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [dot, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc dot ((x :: xs).map (· * a)) (y :: ys)
              = a * x * y + dot (xs.map (· * a)) ys := rfl
            _ = a * x * y + a * dot xs ys := by rw [ih ys h']
            _ = a * (x * y + dot xs ys) := by ring
            _ = a * dot (x :: xs) (y :: ys) := rfl

/-- Zero on the left annihilates. -/
theorem dot_zero_left (xs ys : List Rat) (h : xs.length = ys.length) :
    dot [] ys = 0 := by
  have hd : ys = [] := List.length_eq_zero.mp h
  simp [dot, hd]

/-- Length preservation under the `MAP_LEFT` shape-preserving view, which is
what makes a strided dot's `INCX = INCY = 1` case the same computation. -/
@[simp] theorem map_length (a : Rat) (xs : List Rat) :
    (xs.map (· * a)).length = xs.length := List.length_map _ _

/-- The definition reduces on equal-length input, which is what lets every
theorem below be a plain induction. -/
@[simp] theorem dot_cons (x : Rat) (xs : List Rat) (y : Rat) (ys : List Rat) :
    dot (x :: xs) (y :: ys) = x * y + dot xs ys := rfl

end MagrFormal.DotProduct
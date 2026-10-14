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
# Array model — shape, rank, element count, and strides

This models the **Array ABI**, so it uses **byte** strides, matching
`array-core/include/magr/abi/array_abi.h`:

    int64_t dims[MAGR_MAX_RANK];      /* logical extents        */
    int64_t strides[MAGR_MAX_RANK];   /* BYTES, not elements    */

An earlier draft of this model used element strides. That is a substantive
difference, not a notation choice: an `f64` matrix has byte strides
`{8, 8·ld}`, and any refinement proof to the runtime has to carry the
multiplication by element size. Modelling it in bytes removes a mismatch that
would otherwise have to be discharged later by hand.
-/

namespace MagrFormal.ArrayModel

/-- `MAGR_MAX_RANK` from the Array ABI. -/
def maxRank : Nat := 8

/-- A logical shape together with its well-formedness evidence. -/
structure Shape where
  rank : Nat
  dims : List Nat
  valid : rank ≤ maxRank ∧ dims.length = rank
  deriving Repr

/--
Element count is the product of extents, with the empty product at rank 0.

`foldl (· * ·) 1` rather than `List.prod` because that is literally how the
runtime accumulates it, so a divergence between model and runtime would show up
as a definitional difference rather than hiding behind a library theorem.
-/
def Shape.nelems (s : Shape) : Nat := s.dims.foldl (· * ·) 1

/-- A rank-0 (scalar-shaped) array has exactly one logical element. -/
theorem nelems_rank0 (s : Shape) (h : s.rank = 0) : s.nelems = 1 := by
  have hl : s.dims.length = 0 := by
    have hv := s.valid.2
    rw [h] at hv
    exact hv
  have hd : s.dims = [] := List.length_eq_zero.mp hl
  simp [Shape.nelems, hd]

/-- Any zero extent makes the element count zero. -/
theorem nelems_zero_of_mem_zero (s : Shape) (h : 0 ∈ s.dims) : s.nelems = 0 := by
  have hp : s.dims.prod = 0 := List.prod_eq_zero h
  have heq : s.dims.foldl (· * ·) 1 = s.dims.prod := List.prod_eq_foldl
  rw [Shape.nelems, heq, hp]

/--
Element count is bounded by the number of dimensions' product, hence a rank-0
array and a shape containing a zero are the two degenerate cases the runtime
must handle explicitly.
-/
def maxRankOk (s : Shape) : Prop := s.rank ≤ maxRank := s.valid.1

/-! ### Strides -/

/--
A stride vector, in **bytes**.

`valid` constrains the length only. Contiguity and leading-dimension validity
are properties of a *value*, not of the list, so they are stated where they are
used rather than baked into the type.
-/
structure Strides where
  rank : Nat
  vals : List Int
  valid : vals.length = rank
  deriving Repr

/-- Auxiliary walker. A separate top-level definition rather than a `where`
clause, so it can be referred to in proofs about `colMajorByteStrides`. -/
private def colMajorAux (esz : Nat) : List Nat → List Int
  | [] => []
  | d :: rest => (esz : Int) :: colMajorAux (esz * d) rest

/--
Contiguous column-major byte strides for `dims` with element size `esz`.

Byte 0 holds element `(0,…,0)`, the first axis varies fastest, and the stride
on axis `i` is `esz · ∏_{j<i} dims[j]` — which is exactly the convention under
which element `(i,j)` of an `m × n` matrix with leading dimension `ld` lives at
byte `i·esz + j·ld·esz`, i.e. at element index `i + j·ld`.
-/
def colMajorByteStrides (esz : Nat) (dims : List Nat) : List Int :=
  colMajorAux esz dims

theorem colMajor_length (esz : Nat) (dims : List Nat) :
    (colMajorByteStrides esz dims).length = dims.length := by
  unfold colMajorByteStrides
  induction dims with
  | nil => rfl
  | cons d rest ih => simp [colMajorAux, ih]

/-- The two canonical shapes the numerical functors actually see. -/
@[simp] theorem colMajor_bytes_vector (n : Nat) :
    colMajorByteStrides 8 [n] = [8] := by simp [colMajorByteStrides, colMajorAux]

@[simp] theorem colMajor_bytes_matrix (m n ld : Nat) :
    colMajorByteStrides 8 [m, n] = [8, (8 : Int) * (m : Int)] := by
  simp [colMajorByteStrides, colMajorAux]

/-- The leading dimension is the second stride divided by the element size. -/
theorem leadingDim_matrix (m n : Nat) (ld : Nat) (hld : m ≤ ld) :
    (((colMajorByteStrides 8 [m, n]).get! 1).toNat) / 8 = m := by
  rw [colMajor_bytes_matrix]
  simp only [List.get!_cons_zero, List.get!_cons_succ]
  simp [hld]

end MagrFormal.ArrayModel
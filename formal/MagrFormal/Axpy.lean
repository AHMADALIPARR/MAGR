/-!
# AXPY — the contract of `F_AXPY_F64`

Model: `axpy α xs ys = α·xs + ys`, elementwise.

## Why the model has no behaviour for mismatched lengths

An earlier draft of this model contained

    | xs, [] => xs.map (· * α)   -- "model extension"

which invents a result for `N = 0, DY` of length 0 — a case where DAXPY simply
does nothing, and where MAGR returns `MAGR_STATUS_SHAPE_MISMATCH` before ever
reaching Fortran. An invented clause is worse than a missing one: it makes the
model accept a program that has no defined meaning at runtime.

The model is now total but undefined-off-contract, and every theorem carries the
equal-length hypothesis, exactly as the runtime requires it.
-/

namespace MagrFormal.Axpy

/-- `axpy α xs ys = α·xs + ys`. -/
def axpy (α : Rat) : List Rat → List Rat → List Rat
  | [], [] => []
  | x :: xs, y :: ys => (α * x + y) :: axpy α xs ys
  | _ :: _, [] => []
  | [], _ :: _ => []

/-- Reduction, which is what makes the fusion proof a plain induction. -/
@[simp] theorem axpy_cons (α x y : Rat) (xs ys : List Rat) :
    axpy α (x :: xs) (y :: ys) = (α * x + y) :: axpy α xs ys := rfl

/-- The result has `y`'s length. Combined with `magr_f_axpy_f64`'s
equal-length precondition, the output descriptor shape is determined. -/
theorem axpy_length (α : Rat) (xs ys : List Rat) (h : xs.length = ys.length) :
    (axpy α xs ys).length = ys.length := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [axpy, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc (axpy α (x :: xs) (y :: ys)).length
              = (axpy α (x :: xs) (y :: ys)).length := rfl
            _ = 1 + (axpy α xs ys).length := by rw [axpy_cons]
            _ = 1 + ys.length := by rw [ih ys h']
            _ = (y :: ys).length := rfl

/--
Fusion: `axpy β xs (axpy α xs ys) = axpy (α+β) xs ys`.

This is the algebraic basis for `MAGR_FUSION_AXPY_AXPY`. It is **exact over an
exact field and false over binary64**: `(α+β)*x` rounds once while
`β*x + α*x` rounds twice, so the fused and unfused programs produce different
bits even though they are algebraically equal. The runtime must therefore treat
this as a tolerance-dependent equivalence and measure it, which is why
`docs/roadmap.md` gates fusion on an equivalence contract rather than on this
theorem.
-/
theorem axpy_fusion (α β : Rat) (xs ys : List Rat) (h : xs.length = ys.length) :
    axpy β xs (axpy α xs ys) = axpy (α + β) xs ys := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [axpy, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc axpy β (x :: xs) (axpy α (x :: xs) (y :: ys))
              = β * x + (α * x + y) :: axpy β xs (axpy α xs ys) := rfl
            _ = (β + α) * x + y :: (axpy β xs (axpy α xs ys)) := by
                  congr 1
                  ring
                  rw [ih ys h']
            _ = (α + β) * x + y :: (axpy (α + β) (x :: xs) (y :: ys)) := by
                  congr 1
                  ring

/-- Zero scale leaves `y` unchanged. This is DAXPY's `DA = 0` quick path. -/
theorem axpy_zero (xs ys : List Rat) (h : xs.length = ys.length) :
    axpy 0 xs ys = ys := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [axpy, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc axpy 0 (x :: xs) (y :: ys)
              = 0 * x + y :: axpy 0 xs ys := rfl
            _ = y :: (axpy 0 xs ys) := by congr 1; ring
            _ = y :: ys := by rw [ih ys h']

/-- Unit scale adds, which is the degenerate case of any `a*x + b` fusion rule. -/
theorem axpy_one (xs ys : List Rat) (h : xs.length = ys.length) :
    axpy 1 xs ys = List.zipWith (· + ·) xs ys := by
  induction xs generalizing ys with
  | nil =>
      have hd : ys = [] := List.length_eq_zero.mp h
      simp [axpy, hd]
  | cons x xs ih =>
      cases ys with
      | nil => simp at h
      | cons y ys =>
          have h' : xs.length = ys.length := by simpa using h
          calc axpy 1 (x :: xs) (y :: ys)
              = 1 * x + y :: axpy 1 xs ys := rfl
            _ = (x + y) :: (axpy 1 xs ys) := by congr 1; ring
            _ = (x + y) :: List.zipWith (· + ·) xs ys := by rw [ih ys h']

end MagrFormal.Axpy
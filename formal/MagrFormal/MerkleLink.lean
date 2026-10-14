/-!
# Linking formal artefacts to content-addressed commitments

This module models the *recording* of formal provenance: an execution manifest
may state that a named theorem accompanied a run, and the statement of that
theorem may be content-addressed so a third party can check it is the one
intended.

## What this does NOT do

It does not connect a proof to a binary. Nothing here says that
`libmagr_blas.a` implements `axpy_fusion`, or that the C wrapper's
`magr_f_axpy_f64` matches the `dot`/`axpy` definitions, or that any two builds
agree bit-for-bit. Those need, respectively:

- conformance tests against a reference implementation,
- a refinement argument from the model to the implementation, which is
  separate research and is not attempted here,
- a pinned backend revision plus `-ffp-contract=off` plus a reproducible build.

`recordedInManifest` is a membership test. It is useful — it lets a verifier ask
"did this run claim formal support?" — and it is also trivially satisfiable by
anyone who can edit the manifest. Its only security comes from the manifest
itself being committed: then the claim is *checkable*, which is not the same as
being *true*.
-/

namespace MagrFormal.MerkleLink

/-- Version strings, matching the runtime. -/
def abiVersion : String := "1.0.0"
def irVersion : String := "1.0.0"
def merkleVersion : String := "1.0.0"
def formalVersion : String := "0.1.0"

/--
A content-addressed formal artefact: the `.lean` source text, which is the
stable identity of a *statement plus its proof*. The compiled `.olean` is NOT
modelled, because it varies with the Lean version, the elan toolchain path, and
the build directory, so hashing it produces a value that changes without the
proof changing. See `formal/artifacts/proof_registry.json`.
-/
structure Artifact where
  name : String
  sourcePath : String
  sourceSha256 : String
  deriving Repr

/-- The theorems this model actually proves. -/
def phase6Theorems : List String :=
  [ "MagrFormal.formal_model_scope"
  , "MagrFormal.proof_is_not_conformance"
  , "MagrFormal.ArrayModel.nelems_rank0"
  , "MagrFormal.ArrayModel.nelems_zero_of_mem_zero"
  , "MagrFormal.ArrayModel.colMajor_length"
  , "MagrFormal.ArrayModel.colMajor_bytes_vector"
  , "MagrFormal.ArrayModel.colMajor_bytes_matrix"
  , "MagrFormal.ArrayModel.leadingDim_matrix"
  , "MagrFormal.DotProduct.dot_comm"
  , "MagrFormal.DotProduct.dot_smul_left"
  , "MagrFormal.DotProduct.dot_zero_left"
  , "MagrFormal.DotProduct.map_length"
  , "MagrFormal.Axpy.axpy_length"
  , "MagrFormal.Axpy.axpy_fusion"
  , "MagrFormal.Axpy.axpy_zero"
  , "MagrFormal.Axpy.axpy_one"
  , "MagrFormal.Norm.sumSquares_smul"
  , "MagrFormal.Norm.sumSquares_eq_dot_self"
  , "MagrFormal.Norm.sumSquares_smul_eq_dot"
  ]

/--
Plumbing-only checks. These prove that the registry machinery works, not
anything about the array runtime, and they are listed separately so they are
never mistaken for substantive results.
-/
def plumbingChecks : List String :=
  [ "MagrFormal.MerkleLink.phase6_theorems_nonempty"
  , "MagrFormal.MerkleLink.axpy_fusion_recorded"
  , "MagrFormal.MerkleLink.phase6_theorems_dedup"
  ]

structure FormalManifest where
  abi : String := abiVersion
  ir : String := irVersion
  merkle : String := merkleVersion
  formal : String := formalVersion
  theorems : List String
  deriving Repr

def defaultManifest : FormalManifest where
  theorems := phase6Theorems

/--
Recording a theorem name in a manifest is a *recording act*. Its type is
`Prop` so that nothing downstream can treat it as a proof of anything.
-/
def recordedInManifest (thm : String) (m : FormalManifest) : Prop :=
  thm ∈ m.theorems

@[simp] theorem phase6_theorems_nonempty : phase6Theorems ≠ [] :=
  by simp [phase6Theorems]

theorem axpy_fusion_recorded :
    recordedInManifest "MagrFormal.Axpy.axpy_fusion" defaultManifest := by
  simp [recordedInManifest, defaultManifest, phase6Theorems]

theorem phase6_theorems_dedup : phase6Theorems.Nodup := by
  simp [phase6Theorems]

end MagrFormal.MerkleLink
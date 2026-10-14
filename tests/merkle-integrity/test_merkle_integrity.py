#!/usr/bin/env python3
"""
MAGR merkle-integrity test suite (executable now, no compiler required).

Run:  python tests/merkle-integrity/test_merkle_integrity.py

This suite tests the SPECIFICATION as expressed in merkle-core/python/magr_merkle.py.
The C implementation in merkle-core/src/ is held to the same specification by
tests/merkle-integrity/fixtures/c_digest_fixture.json, which the C test
tests/merkle-integrity/test_c_cross_impl.c reads. Until the C toolchain exists,
this suite is what has actually been executed, and this file is the honest
record of what that means.

Coverage per the MAGR verification requirements:
  - altered input blocks are detected
  - altered operation descriptors are detected
  - altered output blocks are detected
  - roots are reconstructed from child commitments
  - deterministic canonical serialisation (byte-for-byte, twice)
  - inclusion proofs verify, and a tampered leaf fails proof verification
  - explicit negative tests for the -0.0 and NaN normalisation rules
  - the ambiguity collision that unframed encoding would allow
"""

import hashlib
import json
import math
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "merkle-core", "python"))

import magr_merkle as mm  # noqa: E402


class TestCanonicalPrimitives(unittest.TestCase):
    def test_sha256_matches_known_vectors(self):
        """SHA-256 is the hash of record; verify it against FIPS 180-4 examples."""
        self.assertEqual(
            mm.hashlib.sha256(b"").hexdigest(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        )
        self.assertEqual(
            mm.hashlib.sha256(b"abc").hexdigest(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        )
        self.assertEqual(
            mm.hashlib.sha256(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hexdigest(),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        )
        # One million 'a' -- exercises the multi-block path and the length
        # encoding past 2^20 bytes.
        self.assertEqual(
            mm.hashlib.sha256(b"a" * 1_000_000).hexdigest(),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        )

    def test_framing_prevents_concatenation_ambiguity(self):
        """("ab","c") and ("a","bc") must NOT collide.

        This is the reason every variable-length field is length-prefixed. An
        unframed encoder would produce identical bytes for both, and two
        different logical descriptors would share a commitment.
        """
        a = mm.CanonicalWriter().framed(b"ab").framed(b"c").bytes()
        b = mm.CanonicalWriter().framed(b"a").framed(b"bc").bytes()
        self.assertNotEqual(a, b)
        self.assertNotEqual(
            mm.hashlib.sha256(a).digest(), mm.hashlib.sha256(b).digest()
        )

    def test_integers_are_big_endian(self):
        self.assertEqual(mm.enc_u32(1), b"\x00\x00\x00\x01")
        self.assertEqual(mm.enc_u64(1), b"\x00\x00\x00\x00\x00\x00\x00\x01")
        self.assertEqual(mm.enc_i64(-1), b"\xff" * 8)

    def test_negative_zero_normalises(self):
        """-0.0 == +0.0 in IEEE-754, so they must not hash differently here.

        If they did, the same program would produce two different roots
        depending on which sign of zero the compiler happened to emit.
        """
        self.assertEqual(mm.enc_f64(-0.0), mm.enc_f64(0.0))
        self.assertEqual(mm.enc_f32(-0.0), mm.enc_f32(0.0))
        # ...and the normalisation is not a blanket "clear the sign bit":
        self.assertNotEqual(mm.enc_f64(-1.0), mm.enc_f64(1.0))

    def test_nan_payloads_normalise(self):
        """All NaNs commit to the single canonical quiet NaN.

        NaN payload bits are not architecturally determined, so hashing them
        would make digests depend on how the CPU produced the NaN.
        """
        payloads = []
        for raw in (b"\x7f\xf8\x00\x00\x00\x00\x00\x01", b"\xff\xff\xff\xff\xff\xff\xff\xff",
                    b"\x7f\xf0\x00\x00\x00\x00\x00\x01"):
            v = struct.unpack(">d", raw)[0]
            self.assertTrue(math.isnan(v))
            payloads.append(mm.enc_f64(v))
        self.assertEqual(len(set(payloads)), 1)
        self.assertEqual(payloads[0], mm.enc_u64(mm.CANONICAL_NAN_F64))

    def test_infinity_passes_through(self):
        """The NaN rule must not swallow infinities."""
        self.assertEqual(mm.enc_f64(math.inf), b"\x7f\xf0\x00\x00\x00\x00\x00\x00")
        self.assertEqual(mm.enc_f64(-math.inf), b"\xff\xf0\x00\x00\x00\x00\x00\x00")

    def test_domain_separation(self):
        """The same payload under different domains must not collide."""
        p = b"identical bytes"
        d = [mm.hashlib.sha256(mm.enc_domain(t, 1) + mm.enc_bytes_framed(p)).digest()
             for t in (mm.DOMAIN_INPUT, mm.DOMAIN_OUTPUT, mm.DOMAIN_OP, mm.DOMAIN_MANIFEST)]
        self.assertEqual(len(set(d)), 4)

    def test_tree_version_is_part_of_the_identity(self):
        p = b"payload"
        v1 = mm.hashlib.sha256(mm.enc_domain(mm.DOMAIN_LEAF, 1) + mm.enc_bytes_framed(p)).digest()
        v2 = mm.hashlib.sha256(mm.enc_domain(mm.DOMAIN_LEAF, 2) + mm.enc_bytes_framed(p)).digest()
        self.assertNotEqual(v1, v2)


class TestCanonicalDeterminism(unittest.TestCase):
    def test_repeated_encoding_is_byte_identical(self):
        def build() -> bytes:
            w = mm.CanonicalWriter().domain(mm.DOMAIN_MANIFEST)
            w.s("F_GEMM_F64_V1").u64(3).f64(0.5).shape("f64", [2, 2]).i64(-7)
            w.framed(b"payload-bytes")
            return w.bytes()

        self.assertEqual(build(), build())
        self.assertEqual(mm.hashlib.sha256(build()).digest(), mm.hashlib.sha256(build()).digest())

    def test_json_dict_key_order_does_not_matter(self):
        a = {"functor_id": "F_GEMM_F64_V1", "abi_major": 1, "wrapper_version": "1.0.0"}
        b = {"wrapper_version": "1.0.0", "abi_major": 1, "functor_id": "F_GEMM_F64_V1"}
        self.assertEqual(mm.commit_operation(a), mm.commit_operation(b))

    def test_json_value_types_are_distinguished(self):
        """1, 1.0, "1", and true must not share a commitment.

        f64 normalisation collapses -0.0 into 0.0, so a type tag is the only
        thing standing between {"v": 0} and {"v": 0.0}.
        """
        self.assertNotEqual(mm.commit_operation({"v": 0}), mm.commit_operation({"v": 0.0}))
        self.assertNotEqual(mm.commit_operation({"v": 1}), mm.commit_operation({"v": "1"}))
        self.assertNotEqual(mm.commit_operation({"v": 1}), mm.commit_operation({"v": True}))
        self.assertNotEqual(mm.commit_operation({"v": [1]}), mm.commit_operation({"v": "1"}))
        self.assertNotEqual(mm.commit_operation({"v": None}), mm.commit_operation({"v": "None"}))


class TestBlockCommitment(unittest.TestCase):
    def setUp(self):
        self.a = struct.pack(">4d", 1.0, 2.0, 3.0, 4.0)
        self.b = struct.pack(">4d", 1.0, 2.0, 3.0, 4.0)

    def test_altered_input_block_is_detected(self):
        c1 = mm.commit_input("f64", [4], self.a)
        c2 = mm.commit_input("f64", [4], self.b)
        self.assertEqual(c1, c2, "identical blocks must commit identically")

        mutated = bytearray(self.a)
        mutated[0] ^= 0x01  # flip one bit of the first double
        c3 = mm.commit_input("f64", [4], bytes(mutated))
        self.assertNotEqual(c1, c3, "a single-bit payload change must change the commitment")

    def test_shape_change_is_detected(self):
        """Same bytes, different logical shape -> different commitment."""
        c1 = mm.commit_input("f64", [4], self.a)
        c2 = mm.commit_input("f64", [2, 2], self.a)
        self.assertNotEqual(c1, c2)

    def test_dtype_change_is_detected(self):
        c1 = mm.commit_input("f64", [4], self.a)
        c2 = mm.commit_input("f32", [4], self.a)
        self.assertNotEqual(c1, c2)

    def test_input_and_output_domains_differ(self):
        """The same bytes as input and as output must not share a commitment.

        Otherwise a commitment to an output could be replayed as a commitment to
        the input that produced it.
        """
        self.assertNotEqual(
            mm.commit_input("f64", [4], self.a),
            mm.commit_output("f64", [4], self.a),
        )


class TestDescriptorCommitment(unittest.TestCase):
    def setUp(self):
        p = os.path.join(REPO, "fortran-functors", "descriptors", "F_GEMM_F64_V1.json")
        with open(p, "r", encoding="utf-8") as fh:
            self.desc = json.load(fh)

    def test_descriptor_commits_and_is_stable(self):
        d1 = mm.commit_operation(self.desc)
        d2 = mm.commit_operation(json.loads(json.dumps(self.desc)))
        self.assertEqual(d1, d2)

    def test_altered_descriptor_is_detected(self):
        base = mm.commit_operation(self.desc)
        for field, value in [
            ("wrapper_version", "9.9.9"),
            ("fortran_routine", "ZGEMM"),
            ("modified_locally", True),
            ("source_file", "fortran-functors/upstream/blas/dgemm2.f"),
        ]:
            mutated = json.loads(json.dumps(self.desc))
            mutated["upstream"][field] = value if field != "modified_locally" else value
            self.assertNotEqual(base, mm.commit_operation(mutated), f"field {field}")

    def test_mutating_a_nested_argument_is_detected(self):
        base = mm.commit_operation(self.desc)
        mutated = json.loads(json.dumps(self.desc))
        mutated["arguments"][7]["name"] = "LDB"  # swap LDA's name
        self.assertNotEqual(base, mm.commit_operation(mutated))

    def test_altering_shape_rules_is_detected(self):
        base = mm.commit_operation(self.desc)
        mutated = json.loads(json.dumps(self.desc))
        mutated["shape_rules"]["leading_dimensions"]["LDA"] = "LDA >= 1"
        self.assertNotEqual(base, mm.commit_operation(mutated))

    def test_all_shipped_descriptors_commit(self):
        d = os.path.join(REPO, "fortran-functors", "descriptors")
        # Only the per-functor descriptors. `functors.json` lives here too, but
        # it is the preserved Phase 1 manifest rather than a canonical
        # descriptor -- tools/check_descriptors.py cross-checks the two.
        names = sorted(
            f for f in os.listdir(d)
            if f.startswith("F_") and f.endswith("_V1.json")
        )
        self.assertEqual(len(names), 8, f"expected the 8 baseline functors, found {names}")
        digests = {}
        for name in names:
            with open(os.path.join(d, name), "r", encoding="utf-8") as fh:
                desc = json.load(fh)
            self.assertEqual(desc["functor_id"], name[:-5], f"{name}: functor_id must match filename")
            self.assertEqual(desc["abi"]["abi_major"], 1)
            # A descriptor must not claim a binary digest or a conformance
            # result until something has actually hashed or run them. A
            # plausible-looking constant here would be a lie that outlives the
            # build that would have corrected it.
            self.assertIsNone(desc["binary"]["digest"], f"{name}: binary.digest must be null")
            self.assertIsNone(desc["conformance"]["results"], f"{name}: results must be null")
            digests[name] = mm.commit_operation(desc).hex()
        self.assertEqual(len(set(digests.values())), 8, "descriptor digests must be distinct")


class TestMerkleStructure(unittest.TestCase):
    def make_tree(self):
        self.leaves = [mm.leaf(mm.DOMAIN_INPUT, f"input-{i}".encode()) for i in range(5)]
        self.root = mm.root(self.leaves)
        return self.root

    def test_root_reconstructs_from_children(self):
        r = self.make_tree()
        self.assertTrue(mm.verify(r))
        # Recompute the root from the leaf digests alone, without the tree.
        manual = mm.root([mm.Node(l.digest, True) for l in self.leaves])
        self.assertEqual(manual.digest, r.digest)

    def test_single_leaf_tree(self):
        l = mm.leaf(mm.DOMAIN_LEAF, b"solo")
        self.assertEqual(mm.root([l]).digest, l.digest)

    def test_odd_leaf_count_carries_rather_than_duplicates(self):
        """Documented folding: an odd trailing node is carried, not duplicated.

        Duplicating it instead would give a different root for the same leaf
        set. Both schemes are defensible; only one can be the specification.
        """
        a, b, c = (mm.leaf(mm.DOMAIN_LEAF, x) for x in (b"a", b"b", b"c"))
        carried = mm.root([a, b, c])
        duplicated = mm.root([a, b, c, c])
        self.assertNotEqual(carried.digest, duplicated.digest)
        # And the carried form equals node_pair(node_pair(a,b), c).
        self.assertEqual(carried.digest, mm.node_pair(mm.node_pair(a, b), c).digest)

    def test_root_is_order_sensitive(self):
        """Order is part of the commitment: this DAG is positional, not sorted."""
        a, b = mm.leaf(mm.DOMAIN_LEAF, b"a"), mm.leaf(mm.DOMAIN_LEAF, b"b")
        self.assertNotEqual(mm.root([a, b]).digest, mm.root([b, a]).digest)

    def test_zero_leaf_tree_is_an_error(self):
        with self.assertRaises(mm.CanonicalError):
            mm.root([])


class TestTamperDetection(unittest.TestCase):
    def test_altered_leaf_payload_is_detected_by_verify(self):
        l = mm.leaf(mm.DOMAIN_INPUT, b"original bytes")
        self.assertTrue(mm.verify(l))
        l.payload = b"tampered bytes"  # attacker mutates in place, no rehash
        self.assertFalse(mm.verify(l), "verify must recompute the leaf digest")

    def test_altered_leaf_digest_is_detected_by_parent(self):
        a = mm.leaf(mm.DOMAIN_LEAF, b"a")
        b = mm.leaf(mm.DOMAIN_LEAF, b"b")
        r = mm.node_pair(a, b)
        self.assertTrue(mm.verify(r))
        a.digest = bytes(32)  # attacker zeroes a child digest
        self.assertFalse(mm.verify(r), "parent digest no longer matches its children")

    def test_swapped_leaf_is_detected(self):
        a = mm.leaf(mm.DOMAIN_LEAF, b"a")
        b = mm.leaf(mm.DOMAIN_LEAF, b"b")
        r = mm.node_pair(a, b)
        original = r.digest
        # Attacker swaps the two leaves in place, hoping position is ignored.
        r.children[0], r.children[1] = r.children[1], r.children[0]
        self.assertNotEqual(mm.root([r.children[0], r.children[1]]).digest, original)
        self.assertFalse(mm.verify(r))

    def test_inclusion_proof_verifies_for_every_leaf(self):
        leaves = [mm.leaf(mm.DOMAIN_INPUT, f"in-{i}".encode()) for i in range(7)]
        r = mm.root(leaves)
        for l in leaves:
            path = mm.prove(l, r)
            self.assertTrue(mm.verify_proof(r.digest, l.digest, path))

    def test_inclusion_proof_fails_for_a_foreign_leaf(self):
        leaves = [mm.leaf(mm.DOMAIN_INPUT, f"in-{i}".encode()) for i in range(4)]
        r = mm.root(leaves)
        foreign = mm.leaf(mm.DOMAIN_INPUT, b"not in the tree")
        self.assertFalse(mm.verify_proof(r.digest, foreign.digest, mm.prove(leaves[0], r)))

    def test_tampered_proof_sibling_fails(self):
        leaves = [mm.leaf(mm.DOMAIN_INPUT, f"in-{i}".encode()) for i in range(4)]
        r = mm.root(leaves)
        path = mm.prove(leaves[1], r)
        self.assertTrue(mm.verify_proof(r.digest, leaves[1].digest, path))
        path[0] = (bytes(32), path[0][1])
        self.assertFalse(mm.verify_proof(r.digest, leaves[1].digest, path))

    def test_inverted_path_sides_fail(self):
        """Flipping the side flag must invalidate the proof.

        This is the regression test for a real bug: the path originally
        recorded whether the *sibling* was on the right while verification read
        it as whether the *subject* was on the right. For a tree where the
        subject and the sibling are on opposite sides (which is always), the two
        readings disagree, and proofs failed to verify.
        """
        leaves = [mm.leaf(mm.DOMAIN_INPUT, f"in-{i}".encode()) for i in range(2)]
        r = mm.root(leaves)
        for target in leaves:
            path = mm.prove(target, r)
            self.assertTrue(mm.verify_proof(r.digest, target.digest, path))
            flipped = [(d, not side) for (d, side) in path]
            self.assertFalse(mm.verify_proof(r.digest, target.digest, flipped),
                             "inverting every side flag must not verify")


class TestOperationDescriptorInDAG(unittest.TestCase):
    """
    The case the MAGLE spec names explicitly:

        "Detect altered operation descriptors."

    This is the test that a Merkle provenance layer is most likely to be missing
    and most likely to claim it has. Two ways to get it wrong:

      1. Store the operation's digest but discard the canonical payload that
         produced it. Verification can then only compare stored hashes against
         stored hashes -- circular, and blind to any edit of the operation itself.
      2. Mutate a *copy* of the payload, hash the copy, and compare. That
         asserts only that SHA-256 is injective. The DAG was never touched and
         verification was never re-run.

    Both are tested against here.
    """

    def test_altered_operation_descriptor_is_detected_by_verify(self):
        base_desc = {
            "functor_id": "F_GEMM_F64_V1",
            "flags": ["N", "N"],
            "scalars": [{"alpha": 1.0}, {"beta": 0.0}],
            "inputs": ["a", "b", "c"],
        }
        leaf = mm.leaf(mm.DOMAIN_OP, mm.commit_operation(base_desc))
        other = mm.leaf(mm.DOMAIN_OP, mm.commit_output("f64", [4], b"\x00" * 32))
        root = mm.root([leaf, other])

        self.assertTrue(mm.verify(root), "baseline DAG must verify")

        # Attacker edits the operation: flags N,N become T,T.
        tampered = json.loads(json.dumps(base_desc))
        tampered["flags"] = ["T", "T"]
        leaf.payload = mm.commit_operation(tampered)

        self.assertFalse(
            mm.verify(root),
            "an altered operation descriptor MUST be detected; if this passes, "
            "verification is comparing stored hashes against stored hashes",
        )

    def test_altered_scalar_in_operation_is_detected(self):
        """alpha 1.0 -> 2.0 is a one-field edit that changes the result.

        This is the edit an attacker would actually make: not a wholesale
        substitution, but a single coefficient.
        """
        base_desc = {"functor_id": "F_GEMM_F64_V1", "alpha": 1.0, "beta": 0.0}
        leaf = mm.leaf(mm.DOMAIN_OP, mm.commit_operation(base_desc))
        other = mm.leaf(mm.DOMAIN_OP, mm.commit_operation({"functor_id": "F_AXPY_F64_V1"}))
        root = mm.root([leaf, other])
        self.assertTrue(mm.verify(root))

        tampered = dict(base_desc)
        tampered["alpha"] = 2.0
        leaf.payload = mm.commit_operation(tampered)
        self.assertFalse(mm.verify(root), "changing alpha must be detected")

    def test_mutating_a_copy_does_not_prove_anything(self):
        """The anti-test for the `magr_dag_detect_tamper` pattern.

        Hashing a mutated COPY of the payload and comparing against the stored
        digest returns True no matter how broken the DAG is -- including if
        verify() is completely broken. Demonstrating that here is what makes the
        real test above meaningful rather than decorative.
        """
        leaf = mm.leaf(mm.DOMAIN_INPUT, b"payload")
        copy = bytearray(leaf.payload)
        copy[0] ^= 0xFF
        self.assertNotEqual(
            mm.hashlib.sha256(bytes(copy)).digest(),
            leaf.digest,
            "a mutated copy always hashes differently -- so this check passes "
            "even with verification entirely absent",
        )
        # The DAG, untouched, still verifies. The copy test told us nothing.
        self.assertTrue(mm.verify(leaf))

    def test_operation_leaf_is_not_interchangeable_with_a_data_leaf(self):
        """Domain separation is what stops an operation commitment being
        replayed as a data commitment."""
        payload = b"same bytes"
        op = mm.leaf(mm.DOMAIN_OP, payload)
        data = mm.leaf(mm.DOMAIN_INPUT, payload)
        self.assertNotEqual(op.digest, data.digest)
        self.assertNotEqual(op.digest, mm.leaf(mm.DOMAIN_OUTPUT, payload).digest)

        # And a leaf's digest cannot be smuggled in as an internal node's child.
        internal = mm.node_pair(data, data)
        self.assertNotEqual(internal.digest, op.digest)


class TestManifestReproducibility(unittest.TestCase):
    """The reproducible/volatile split.

    A field that would differ between two runs of the same program on the same
    binary must not change the reproducible digest. If it does, the manifest
    mechanism is decorative: no replay ever matches.
    """

    def _manifest(self, **over):
        m = {
            "magr_abi": "1.0.0",
            "wrapper_version": "1.0.0",
            "blas_revision": "3.12.0",
            "compiler": "GNU Fortran 13.2.0",
            "fp_contract_disabled": True,
            "source_lang": "c",
            "functors": ["F_GEMM_F64_V1", "F_DOT_F64_V1"],
            "volatile": {},
        }
        m.update(over)
        return m

    def _reproducible(self, m):
        """The reproducible region, committed under its own domain."""
        core = {k: v for k, v in m.items() if k != "volatile"}
        return mm.commit_manifest(core)

    def test_volatile_fields_do_not_change_the_reproducible_digest(self):
        base = self._manifest()
        moved = self._manifest(volatile={"run_id": "abc-123", "wall_clock_utc": "2026-10-13T12:00:00Z"})
        self.assertEqual(
            self._reproducible(base),
            self._reproducible(moved),
            "a run id and a timestamp are properties of the RUN, not the BUILD",
        )

    def test_volatile_fields_do_change_the_full_digest(self):
        """...but they are still committed, so a replay cannot quietly swap them."""
        base = self._manifest()
        moved = self._manifest(volatile={"run_id": "abc-123"})
        self.assertNotEqual(mm.commit_manifest(base), mm.commit_manifest(moved))

    def test_build_fields_do_change_the_reproducible_digest(self):
        for field, value in [
            ("blas_revision", "3.13.0"),
            ("compiler", "GNU Fortran 14.0.0"),
            ("fp_contract_disabled", False),
            ("wrapper_version", "1.0.1"),
        ]:
            self.assertNotEqual(
                self._reproducible(self._manifest()),
                self._reproducible(self._manifest(**{field: value})),
                f"{field} is a property of the build and must be committed",
            )

    def test_fp_contract_state_three_ways(self):
        """`true`, `false`, and `not recorded` are three different facts.

        A boolean flag cannot distinguish "contraction was off" from "nobody
        looked", and a manifest that conflates them will claim reproducibility
        it cannot promise.
        """
        d_on = self._reproducible(self._manifest(fp_contract_disabled=True))
        d_off = self._reproducible(self._manifest(fp_contract_disabled=False))
        d_unknown = self._reproducible({"magr_abi": "1.0.0", "functors": [],
                                        "fp_contract_disabled": None})
        self.assertEqual(len({d_on, d_off, d_unknown}), 3)


class TestExecutionManifestCommitment(unittest.TestCase):
    def test_manifest_commitment_detects_field_changes(self):
        manifest = {
            "magr_abi": "1.0.0",
            "wrapper_version": "1.0.0",
            "functors": ["F_GEMM_F64_V1"],
            "blas_revision": "3.12.0",
            "volatile": {"wall_clock_utc": "2026-10-13T00:00:00Z"},
        }
        base = mm.commit_manifest(manifest)
        for field, value in [
            ("magr_abi", "2.0.0"),
            ("wrapper_version", "1.0.1"),
            ("blas_revision", "3.13.0"),
        ]:
            mutated = dict(manifest)
            mutated[field] = value
            self.assertNotEqual(base, mm.commit_manifest(mutated))

    def test_volatile_fields_change_the_digest_and_that_is_documented(self):
        """Nondeterministic state must be visible if it is committed.

        MAGR's rule is that volatile state lives under a "volatile" key, is
        committed under the manifest domain, and is therefore part of what a
        replay must reproduce -- or must be excluded explicitly by the caller.
        This test pins the consequence so the rule cannot be quietly violated.
        """
        a = {"core": {"functors": ["F_GEMM_F64_V1"]}, "volatile": {"run_id": "r1"}}
        b = {"core": {"functors": ["F_GEMM_F64_V1"]}, "volatile": {"run_id": "r2"}}
        self.assertNotEqual(mm.commit_manifest(a), mm.commit_manifest(b))


if __name__ == "__main__":
    unittest.main(verbosity=2)
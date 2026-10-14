# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 MAGR contributors
#
# This file is part of MAGR (Merkle Array Graph Runtime).
#
# MAGR is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, version 3.
#
# MAGR is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
#
# Note for AGPL section 13: MAGR is designed to be used over a
# network. If you run a modified version as a service, section 13
# requires you to offer those users the corresponding source of
# your modified version. See docs/licensing.md.

"""
magr_merkle.py -- reference implementation of MAGR's canonical encoding and
Merkle DAG, in pure Python with no third-party dependencies.

This file exists for two reasons, and it is important to be clear about which
is which:

  1. It is the CROSS-IMPLEMENTATION ORACLE. merkle-core/src/{canonical,merkle}.c
     and this module implement the same specification. The C tests include a
     fixture of digests produced by this module; if the two ever disagree,
     one of them is wrong and the test says which. This is the whole reason the
     encoding is written down normatively in docs/canonical-encoding.md instead
     of being implied by the C code.

  2. It is runnable today. The C toolchain is not always present, and a
     cryptographic layer that can only be checked when a compiler exists is a
     cryptographic layer nobody checks.

It is NOT a dependency of the runtime. Nothing in array-core, the Fortran
functors, or the Chapel runtime imports it.

See docs/canonical-encoding.md for the normative specification this implements.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from typing import Iterable, Sequence

# --------------------------------------------------------------------------
# Domain tags. These must match MAGR_DOMAIN_* in include/magr/canonical.h.
# --------------------------------------------------------------------------

DOMAIN_INPUT = b"INPT"
DOMAIN_OP = b"OPER"
DOMAIN_OUTPUT = b"OUTP"
DOMAIN_MANIFEST = b"MANF"
DOMAIN_LEAF = b"LEAF"
DOMAIN_NODE = b"NODE"
DOMAIN_ROOT = b"ROOT"

TREE_VERSION = 1

CANONICAL_NAN_F64 = 0x7FF8000000000000
CANONICAL_NAN_F32 = 0x7FC00000


class CanonicalError(Exception):
    """Raised when a value cannot be canonically encoded."""


# --------------------------------------------------------------------------
# Primitive encoders. All integers are big-endian; all variable-length fields
# are u64-length-prefixed.
# --------------------------------------------------------------------------


def enc_bytes_framed(data: bytes) -> bytes:
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise CanonicalError("enc_bytes_framed expects bytes")
    return len(data).to_bytes(8, "big", signed=False) + bytes(data)


def enc_u8(v: int) -> bytes:
    if not 0 <= v <= 0xFF:
        raise CanonicalError(f"u8 out of range: {v}")
    return bytes([v])


def enc_u16(v: int) -> bytes:
    if not 0 <= v <= 0xFFFF:
        raise CanonicalError(f"u16 out of range: {v}")
    return v.to_bytes(2, "big")


def enc_u32(v: int) -> bytes:
    if not 0 <= v <= 0xFFFFFFFF:
        raise CanonicalError(f"u32 out of range: {v}")
    return v.to_bytes(4, "big")


def enc_u64(v: int) -> bytes:
    if not 0 <= v <= 0xFFFFFFFFFFFFFFFF:
        raise CanonicalError(f"u64 out of range: {v}")
    return v.to_bytes(8, "big")


def enc_i64(v: int) -> bytes:
    if not -(2**63) <= v < 2**63:
        raise CanonicalError(f"i64 out of range: {v}")
    return (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "big")


def normalise_f64_bits(bits: int) -> int:
    """Apply the -0.0 -> +0.0 and NaN -> canonical quiet NaN rules."""
    if bits == 0x8000000000000000:
        return 0
    if (bits & 0x7FF0000000000000) == 0x7FF0000000000000 and (bits & 0x000FFFFFFFFFFFFF) != 0:
        return CANONICAL_NAN_F64
    return bits


def enc_f64(v: float) -> bytes:
    import struct

    bits = struct.unpack(">Q", struct.pack(">d", float(v)))[0]
    return enc_u64(normalise_f64_bits(bits))


def enc_f32(v: float) -> bytes:
    import struct

    bits = struct.unpack(">I", struct.pack(">f", float(v)))[0]
    if bits == 0x80000000:
        bits = 0
    elif (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0:
        bits = CANONICAL_NAN_F32
    return enc_u32(bits)


def enc_str(s: str) -> bytes:
    raw = s.encode("utf-8")
    return len(raw).to_bytes(8, "big") + raw


def enc_domain(tag: bytes, version: int) -> bytes:
    if len(tag) != 4:
        raise CanonicalError(f"domain tag must be 4 bytes, got {tag!r}")
    return tag + enc_u64(version)


def enc_shape(dtype: str, dims: Sequence[int]) -> bytes:
    if len(dims) > 255:
        raise CanonicalError("rank out of range")
    out = enc_u8(len(dims)) + enc_str(dtype)
    for d in dims:
        out += enc_i64(int(d))
    return out


# --------------------------------------------------------------------------
# Writer
# --------------------------------------------------------------------------


class CanonicalWriter:
    """Accumulates canonical bytes. Mirrors magr_can in canonical.c."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def raw(self, data: bytes) -> "CanonicalWriter":
        self._buf += bytes(data)
        return self

    def framed(self, data: bytes) -> "CanonicalWriter":
        self._buf += enc_bytes_framed(data)
        return self

    def u8(self, v: int) -> "CanonicalWriter":
        self._buf += enc_u8(v)
        return self

    def u16(self, v: int) -> "CanonicalWriter":
        self._buf += enc_u16(v)
        return self

    def u32(self, v: int) -> "CanonicalWriter":
        self._buf += enc_u32(v)
        return self

    def u64(self, v: int) -> "CanonicalWriter":
        self._buf += enc_u64(v)
        return self

    def i64(self, v: int) -> "CanonicalWriter":
        self._buf += enc_i64(v)
        return self

    def f64(self, v: float) -> "CanonicalWriter":
        self._buf += enc_f64(v)
        return self

    def f32(self, v: float) -> "CanonicalWriter":
        self._buf += enc_f32(v)
        return self

    def s(self, v: str) -> "CanonicalWriter":
        self._buf += enc_str(v)
        return self

    def domain(self, tag: bytes, version: int = TREE_VERSION) -> "CanonicalWriter":
        self._buf += enc_domain(tag, version)
        return self

    def shape(self, dtype: str, dims: Sequence[int]) -> "CanonicalWriter":
        self._buf += enc_shape(dtype, dims)
        return self

    def bytes(self) -> bytes:
        return bytes(self._buf)

    def digest(self) -> bytes:
        return hashlib.sha256(self._buf).digest()


# --------------------------------------------------------------------------
# Merkle DAG
# --------------------------------------------------------------------------


@dataclass
class Node:
    digest: bytes
    is_leaf: bool
    payload: bytes | None = None
    domain: bytes | None = None
    children: list["Node"] = field(default_factory=list)

    def __post_init__(self) -> None:
        if len(self.digest) != 32:
            raise CanonicalError("digest must be 32 bytes")


def leaf(domain: bytes, payload: bytes) -> Node:
    d = hashlib.sha256(enc_domain(domain, TREE_VERSION) + enc_bytes_framed(payload)).digest()
    return Node(digest=d, is_leaf=True, payload=bytes(payload), domain=domain)


def node_pair(left: Node, right: Node) -> Node:
    d = hashlib.sha256(
        enc_domain(DOMAIN_NODE, TREE_VERSION) + left.digest + right.digest
    ).digest()
    return Node(digest=d, is_leaf=False, children=[left, right])


def root(nodes: Sequence[Node]) -> Node:
    """Pairwise fold, carrying an odd trailing node up unchanged.

    This is the scheme documented in docs/merkle.md section "Folding". An
    implementation that instead duplicates the odd leaf produces a DIFFERENT
    root; that difference is intentional and is what the cross-implementation
    fixture in tests/merkle-integrity pins down.
    """
    if not nodes:
        raise CanonicalError("cannot build a root from zero nodes")
    level = list(nodes)
    while len(level) > 1:
        nxt: list[Node] = []
        i = 0
        while i + 1 < len(level):
            nxt.append(node_pair(level[i], level[i + 1]))
            i += 2
        if len(level) % 2 == 1:
            nxt.append(level[-1])
        level = nxt
    return level[0]


def verify(n: Node) -> bool:
    """Recompute internal node digests bottom-up and compare."""
    if n.is_leaf:
        if n.domain is None or n.payload is None:
            return False
        recomputed = hashlib.sha256(
            enc_domain(n.domain, TREE_VERSION) + enc_bytes_framed(n.payload)
        ).digest()
        return recomputed == n.digest
    if len(n.children) != 2:
        return False
    for c in n.children:
        if not verify(c):
            return False
    recomputed = hashlib.sha256(
        enc_domain(DOMAIN_NODE, TREE_VERSION) + n.children[0].digest + n.children[1].digest
    ).digest()
    return recomputed == n.digest


def prove(leaf_node: Node, root_node: Node) -> list[tuple[bytes, bool]]:
    """Return [(sibling_digest, subject_is_right), ...] from leaf to root.

    The boolean records whether the *subject* of the current step (the node
    whose digest is on the path) is the right child of its parent. It is NOT a
    statement about the sibling's side; conflating the two is an easy and silent
    way to produce proofs that verify against the wrong root.
    """
    if leaf_node is root_node:
        return []

    def walk(target: Node, node: Node) -> list[tuple[bytes, bool]] | None:
        if node is target:
            return []
        if node.is_leaf:
            return None
        for i, child in enumerate(node.children):
            sub = walk(target, child)
            if sub is not None:
                sib = node.children[i ^ 1]
                sub.append((sib.digest, i == 1))
                return sub
        return None

    path = walk(leaf_node, root_node)
    if path is None:
        raise CanonicalError("leaf is not in the tree")
    return path


def verify_proof(claimed_root: bytes, leaf_digest: bytes, path: Iterable[tuple[bytes, bool]]) -> bool:
    cur = leaf_digest
    for sibling, subject_is_right in path:
        if subject_is_right:
            cur = hashlib.sha256(enc_domain(DOMAIN_NODE, TREE_VERSION) + sibling + cur).digest()
        else:
            cur = hashlib.sha256(enc_domain(DOMAIN_NODE, TREE_VERSION) + cur + sibling).digest()
    return cur == claimed_root


# --------------------------------------------------------------------------
# Layer-specific commitments
# --------------------------------------------------------------------------


def commit_operation(descriptor: dict) -> bytes:
    """Canonicalise a functor descriptor and commit to it.

    JSON is NOT canonicalised by re-serialising the parsed object with
    json.dumps(sort_keys=True) alone: that is stable for dicts but silently
    loses duplicate keys and normalises number formatting in ways that depend
    on the parser. We commit to the parsed value with an explicit, ordered
    encoder instead.
    """
    w = CanonicalWriter()
    _encode_json_value(w, descriptor)
    # Matches magr_can_digest() exactly: the canonical value is itself the
    # length-framed payload of a domain-separated hash.
    return hashlib.sha256(
        enc_domain(DOMAIN_OP, TREE_VERSION) + enc_bytes_framed(w.bytes())
    ).digest()


def commit_block(domain: bytes, dtype: str, dims: Sequence[int], payload: bytes) -> bytes:
    w = CanonicalWriter().domain(domain)
    w.s(dtype)
    w.framed(enc_shape(dtype, dims))
    w.framed(payload)
    return hashlib.sha256(w.bytes()).digest()


def commit_input(dtype: str, dims: Sequence[int], payload: bytes) -> bytes:
    return commit_block(DOMAIN_INPUT, dtype, dims, payload)


def commit_output(dtype: str, dims: Sequence[int], payload: bytes) -> bytes:
    return commit_block(DOMAIN_OUTPUT, dtype, dims, payload)


def commit_manifest(manifest: dict) -> bytes:
    w = CanonicalWriter()
    _encode_json_value(w, manifest)
    return hashlib.sha256(
        enc_domain(DOMAIN_MANIFEST, TREE_VERSION) + enc_bytes_framed(w.bytes())
    ).digest()


def _encode_json_value(w: CanonicalWriter, v) -> None:
    """Ordered, type-tagged encoding of a JSON-ish value.

    Type tags matter: without them, the integer 1, the float 1.0, the string
    "1", and the boolean true would all have to be distinguished by the value
    alone, and {"a": 1} vs {"a": 1.0} would collide after float normalisation.
    """
    if v is None:
        w.u8(0)
    elif isinstance(v, bool):
        w.u8(1)
        w.u8(1 if v else 0)
    elif isinstance(v, int):
        w.u8(2)
        w.s(str(v))  # decimal string avoids int64 range questions across languages
    elif isinstance(v, float):
        w.u8(3)
        w.f64(v)
    elif isinstance(v, str):
        w.u8(4)
        w.s(v)
    elif isinstance(v, (list, tuple)):
        w.u8(5)
        w.u64(len(v))
        for item in v:
            _encode_json_value(w, item)
    elif isinstance(v, dict):
        w.u8(6)
        w.u64(len(v))
        # Sorted by UTF-8 byte order of the key. Sorting is mandatory: JSON
        # object order is not semantically meaningful, so the canonical form
        # must impose one.
        for k in sorted(v.keys(), key=lambda s: s.encode("utf-8")):
            w.s(k)
            _encode_json_value(w, v[k])
    else:
        raise CanonicalError(f"cannot canonically encode {type(v).__name__}")


def commit_descriptors(descriptors: Iterable[dict]) -> bytes:
    """Single commitment covering a set of functor descriptors.

    Sorted by functor_id first: the set of committed functors is a property of
    the build, not of the order the build system happened to discover them.
    """
    items = sorted(descriptors, key=lambda d: d.get("functor_id", ""))
    w = CanonicalWriter()
    w.u64(len(items))
    for d in items:
        _encode_json_value(w, d)
    return hashlib.sha256(
        enc_domain(DOMAIN_MANIFEST, TREE_VERSION) + enc_bytes_framed(w.bytes())
    ).digest()


def hexd(b: bytes) -> str:
    return b.hex()
# SPDX-License-Identifier: Apache-2.0
"""ECDSA-P256 verification + canonicalization for SoM-release provenance signatures.

Public + verify-only: the private signing key and the signing operation live in
alp-sdk-internal. ``canonical_bytes`` here is the single source of truth for the bytes
that are signed/verified. A signature covers the document with its ``signature`` field
set to null, serialized with sorted keys + compact separators.
"""
from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ALGORITHM = "ecdsa-p256-sha256"
FORMAT_VERSION = 1


def canonical_bytes(doc: dict) -> bytes:
    """Deterministic bytes that get signed/verified: the doc with signature=null."""
    d = dict(doc)
    d["signature"] = None
    return json.dumps(
        d, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")


def load_public_key(pem: "str | bytes") -> ec.EllipticCurvePublicKey:
    if isinstance(pem, str):
        pem = pem.encode()
    return serialization.load_pem_public_key(pem)


def load_public_key_file(path: "str | Path") -> ec.EllipticCurvePublicKey:
    return load_public_key(Path(path).read_bytes())


def compute_key_id(public_key) -> str:
    der = public_key.public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    # 64-bit prefix: enough to disambiguate signing keys for rotation; it is a
    # lookup hint, not a secret, so truncation is intentional.
    return hashlib.sha256(der).hexdigest()[:16]


def verify_signature(doc: dict, public_key) -> bool:
    sig = doc.get("signature")
    if not isinstance(sig, dict):
        return False
    if sig.get("format_version") != FORMAT_VERSION:
        return False
    if sig.get("algorithm") != ALGORITHM:
        return False
    if sig.get("key_id") != compute_key_id(public_key):
        return False
    try:
        raw = base64.b64decode(sig["value"], validate=True)
    except Exception:
        return False
    try:
        public_key.verify(raw, canonical_bytes(doc), ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError, TypeError):
        # wrong key type / malformed-but-decodable signature must return False,
        # never propagate (the contract is -> bool).
        return False


def verify_with_keys(doc: dict, public_keys: list) -> bool:
    return any(verify_signature(doc, k) for k in public_keys)

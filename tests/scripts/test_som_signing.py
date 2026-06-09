import base64, json, sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import som_signing as s
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def _keypair():
    k = ec.generate_private_key(ec.SECP256R1())
    return k, k.public_key()


def _sign(doc, key):
    sig = key.sign(s.canonical_bytes(doc), ec.ECDSA(hashes.SHA256()))
    return {
        "format_version": s.FORMAT_VERSION, "algorithm": s.ALGORITHM,
        "key_id": s.compute_key_id(key.public_key()),
        "signed_at": "2026-06-09", "value": base64.b64encode(sig).decode(),
    }


def test_canonical_bytes_sets_signature_null_and_is_sorted():
    a = s.canonical_bytes({"b": 1, "a": 2, "signature": {"x": 1}})
    b = s.canonical_bytes({"signature": None, "a": 2, "b": 1})
    assert a == b == b'{"a":2,"b":1,"signature":null}'


def test_sign_then_verify_roundtrip():
    key, pub = _keypair()
    doc = {"sku": "E1M-V2N101", "release_version": "som-0.2.0", "signature": None}
    doc["signature"] = _sign(doc, key)
    assert s.verify_signature(doc, pub) is True


def test_tamper_fails():
    key, pub = _keypair()
    doc = {"sku": "E1M-V2N101", "signature": None}
    doc["signature"] = _sign(doc, key)
    doc["sku"] = "E1M-V2N102"
    assert s.verify_signature(doc, pub) is False


def test_wrong_key_fails_on_key_id():
    key, _ = _keypair()
    _, other_pub = _keypair()
    doc = {"a": 1, "signature": None}
    doc["signature"] = _sign(doc, key)
    assert s.verify_signature(doc, other_pub) is False


def test_absent_signature_returns_false():
    _, pub = _keypair()
    assert s.verify_signature({"a": 1, "signature": None}, pub) is False


def test_verify_with_keys_rotation():
    old, old_pub = _keypair()
    new, new_pub = _keypair()
    doc = {"a": 1, "signature": None}
    doc["signature"] = _sign(doc, new)
    assert s.verify_with_keys(doc, [old_pub, new_pub]) is True
    assert s.verify_with_keys(doc, [old_pub]) is False

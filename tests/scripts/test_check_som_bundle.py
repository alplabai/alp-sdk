"""Unit tests for scripts/check_som_bundle.py."""

import base64
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_som_bundle.py"
EXAMPLE = REPO / "metadata" / "templates" / "som-release-bundle.example.json"
sys.path.insert(0, str(REPO / "scripts"))
import som_signing  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True,
    )


def _valid_bundle():
    h = "0" * 64
    return {
        "schema_version": 1,
        "sku": "E1M-V2N101",
        "family": "v2n",
        "hw_rev": "r1",
        "release_version": "som-1.0.0",
        "created": "2026-06-08",
        "status": "complete",
        "components": [
            {"role": "bl2", "file": "artifacts/bl2.bin", "sha256": h,
             "size_bytes": 1, "flash_target": "xspi:mtd0"},
            {"role": "fip", "file": "artifacts/fip.bin", "sha256": h,
             "size_bytes": 1, "flash_target": "xspi:mtd1"},
            {"role": "system_image", "file": "artifacts/img.wic.gz", "sha256": h,
             "size_bytes": 1, "flash_target": "emmc"},
        ],
        "provenance": {
            "toolchain": "gcc-13.4.0", "u_boot_srcrev": "bcf29d98",
            "tf_a_srcrev": "4092464", "uboot_defconfig": "rzv2n-dev_defconfig",
            "patches": ["pmic-removal", "add-ether", "deepx"],
            "equivalence": "functional-equiv modulo gcc",
        },
        "signature": None,
    }


def test_shipped_example_validates():
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_valid_bundle_passes(tmp_path):
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(_valid_bundle()))
    proc = _run("--bundle", str(p))
    assert proc.returncode == 0, proc.stdout


def test_complete_status_requires_system_image(tmp_path):
    b = _valid_bundle()
    b["components"] = [c for c in b["components"] if c["role"] != "system_image"]
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(b))
    proc = _run("--bundle", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_bootloader_only_without_image_passes(tmp_path):
    b = _valid_bundle()
    b["status"] = "bootloader-only:image-pending-hw"
    b["components"] = [c for c in b["components"] if c["role"] != "system_image"]
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(b))
    proc = _run("--bundle", str(p))
    assert proc.returncode == 0, proc.stdout


def test_missing_bl2_fails(tmp_path):
    b = _valid_bundle()
    b["components"] = [c for c in b["components"] if c["role"] != "bl2"]
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(b))
    proc = _run("--bundle", str(p))
    assert proc.returncode != 0


def test_bad_sha256_fails(tmp_path):
    b = _valid_bundle()
    b["components"][0]["sha256"] = "NOTHEX"
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(b))
    proc = _run("--bundle", str(p))
    assert proc.returncode != 0


def test_bad_created_date_fails(tmp_path):
    # format:date must be enforced (format_checker), not just annotated:
    # an impossible date and a timestamp both have to be rejected.
    for bad in ("2026-13-45", "2026-06-08T12:00:00Z"):
        b = _valid_bundle()
        b["created"] = bad
        p = tmp_path / "bundle.json"
        p.write_text(json.dumps(b))
        proc = _run("--bundle", str(p))
        assert proc.returncode != 0, f"{bad!r} should be rejected\n{proc.stdout}"


def _sign_bundle(bundle, key):
    sig = key.sign(som_signing.canonical_bytes(bundle), ec.ECDSA(hashes.SHA256()))
    bundle["signature"] = {
        "format_version": som_signing.FORMAT_VERSION,
        "algorithm": som_signing.ALGORITHM,
        "key_id": som_signing.compute_key_id(key.public_key()),
        "signed_at": "2026-06-09",
        "value": base64.b64encode(sig).decode(),
    }
    return bundle


def _write_pub(key, path):
    path.write_bytes(key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))


def test_signed_bundle_verifies(tmp_path):
    key = ec.generate_private_key(ec.SECP256R1())
    b = _sign_bundle(_valid_bundle(), key)
    p = tmp_path / "bundle.json"; p.write_text(json.dumps(b))
    pub = tmp_path / "pub.pem"; _write_pub(key, pub)
    proc = _run("--bundle", str(p), "--pubkey", str(pub), "--require-signature")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "verified" in proc.stdout


def test_tampered_signed_bundle_fails(tmp_path):
    key = ec.generate_private_key(ec.SECP256R1())
    b = _sign_bundle(_valid_bundle(), key)
    b["hw_rev"] = "r2"  # mutate AFTER signing
    p = tmp_path / "bundle.json"; p.write_text(json.dumps(b))
    pub = tmp_path / "pub.pem"; _write_pub(key, pub)
    proc = _run("--bundle", str(p), "--pubkey", str(pub))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_unsigned_with_require_fails(tmp_path):
    p = tmp_path / "bundle.json"; p.write_text(json.dumps(_valid_bundle()))
    proc = _run("--bundle", str(p), "--require-signature")
    assert proc.returncode != 0


def test_unsigned_without_require_passes(tmp_path):
    p = tmp_path / "bundle.json"; p.write_text(json.dumps(_valid_bundle()))
    proc = _run("--bundle", str(p))
    assert proc.returncode == 0, proc.stdout


def test_malformed_signature_object_rejected_by_schema(tmp_path):
    b = _valid_bundle()
    b["signature"] = {"algorithm": "ecdsa-p256-sha256"}  # missing required fields
    p = tmp_path / "bundle.json"; p.write_text(json.dumps(b))
    proc = _run("--bundle", str(p))
    assert proc.returncode != 0

"""Unit tests for scripts/check_som_bundle.py."""

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_som_bundle.py"
EXAMPLE = REPO / "metadata" / "templates" / "som-release-bundle.example.json"


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

# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_helper_firmware.py (issue #852, metadata half).

Covers the three-way contract: present+matching sha256 verifies, present+
wrong sha256 is a hard failure, and an absent artefact (the real GD32
gitignored-build-output case) is reported but does NOT fail the gate.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_helper_firmware as gate  # noqa: E402


def _write_fw(tmp_path: Path, name: str, content: bytes) -> Path:
    fw = tmp_path / name
    fw.write_bytes(content)
    return fw


def test_present_artifact_matching_checksum_verifies(tmp_path: Path) -> None:
    content = b"fake-firmware-bytes"
    _write_fw(tmp_path, "fw.bin", content)
    entry = {
        "name": "some_bridge",
        "firmware_path": "fw.bin",
        "sha256": hashlib.sha256(content).hexdigest(),
    }
    status, errors = gate.check_entry("E1M-TEST", entry, tmp_path)
    assert status == "verified"
    assert errors == []


def test_present_artifact_wrong_checksum_is_caught(tmp_path: Path) -> None:
    """Sensitivity proof: a tampered/wrong checksum must fail, not pass."""
    content = b"fake-firmware-bytes"
    _write_fw(tmp_path, "fw.bin", content)
    entry = {
        "name": "some_bridge",
        "firmware_path": "fw.bin",
        "sha256": "0" * 64,  # deliberately wrong
    }
    status, errors = gate.check_entry("E1M-TEST", entry, tmp_path)
    assert status == "mismatch"
    assert len(errors) == 1
    assert "sha256 mismatch" in errors[0]


def test_absent_artifact_is_reported_non_fatally(tmp_path: Path) -> None:
    """The GD32 gitignored-build-output case: file doesn't exist on a
    fresh clone.  Must be reported (status='absent') but NOT raise an
    error -- CI on a fresh clone must not redden over this."""
    entry = {
        "name": "gd32_bridge",
        "firmware_path": "firmware/gd32-bridge/build/gd32/gd32-bridge.bin",
        "sha256": "TBD",
    }
    status, errors = gate.check_entry("E1M-V2N101", entry, tmp_path)
    assert status == "absent"
    assert errors == []


def test_tbd_firmware_path_is_not_applicable(tmp_path: Path) -> None:
    entry = {"name": "cc3501e_otp", "firmware_path": "TBD"}
    status, errors = gate.check_entry("E1M-TEST", entry, tmp_path)
    assert status == "not-applicable"
    assert errors == []


def test_real_metadata_has_no_checksum_mismatch() -> None:
    """End-to-end: run the gate's main() against the real repo metadata.

    Must exit 0 -- the real CC3501E binary is present and matches, and the
    real GD32 binary is absent from this checkout (gitignored build
    output), which the gate must not treat as fatal.
    """
    rc = gate.main([])
    assert rc == 0

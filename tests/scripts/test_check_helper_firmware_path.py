"""Unit tests for scripts/check_helper_firmware_path.py (issue #1372)."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_helper_firmware_path.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_preset(tmp_path: Path, sku: str, body: str) -> None:
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{sku}.yaml").write_text(f"sku: {sku}\n{body}")


def test_empty_tree_passes(tmp_path):
    """No metadata/e1m_modules at all -> exit 0."""
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_resolving_firmware_path_passes(tmp_path):
    """firmware_path names a real repo-relative file -> exit 0."""
    fw_dir = tmp_path / "firmware" / "cc3501e" / "prebuilt"
    fw_dir.mkdir(parents=True)
    (fw_dir / "cc3501e-v0.2.0.bin").write_bytes(b"\x00" * 16)
    _write_preset(
        tmp_path, "E1M-TEST",
        "helper_firmware:\n"
        "  - name: cc3501e_otp\n"
        "    chip: cc3501e\n"
        "    firmware_path: firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin\n"
        "    flash_policy: recovery_only\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_missing_firmware_path_fails(tmp_path):
    """firmware_path names a file that does not exist -> exit 1, naming the
    preset, the entry, and the path -- the exact defect issue #1372 filed
    (a stale/wrong path resolving to nothing, silently)."""
    _write_preset(
        tmp_path, "E1M-TEST",
        "helper_firmware:\n"
        "  - name: cc3501e_otp\n"
        "    chip: cc3501e\n"
        "    firmware_path: firmware/cc3501e/prebuilt/does-not-exist.bin\n"
        "    flash_policy: recovery_only\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "E1M-TEST.yaml" in out
    assert "cc3501e_otp" in out
    assert "does-not-exist.bin" in out


def test_omitted_firmware_path_is_legal_and_passes(tmp_path):
    """Omitting firmware_path entirely is schema-legal (means "no artefact
    yet") -- must NOT be flagged as broken, the exact case the issue calls
    out explicitly."""
    _write_preset(
        tmp_path, "E1M-TEST",
        "helper_firmware:\n"
        "  - name: gd32_bridge\n"
        "    chip: gd32g553\n"
        "    flash_policy: recovery_only\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_empty_helper_firmware_list_passes(tmp_path):
    """helper_firmware: [] (e.g. E1M-NX9101, no helper MCU at all) -> exit 0."""
    _write_preset(tmp_path, "E1M-TEST", "helper_firmware: []\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_firmware_path_pointing_at_a_directory_fails(tmp_path):
    """A firmware_path that resolves to a directory (not a regular file --
    e.g. a bad rename left a stale dir behind) must fail the same way a
    missing path does; is_file() is deliberate over exists()."""
    (tmp_path / "firmware" / "cc3501e" / "prebuilt" / "cc3501e-v0.2.0.bin").mkdir(
        parents=True
    )
    _write_preset(
        tmp_path, "E1M-TEST",
        "helper_firmware:\n"
        "  - name: cc3501e_otp\n"
        "    chip: cc3501e\n"
        "    firmware_path: firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin\n"
        "    flash_policy: recovery_only\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "E1M-TEST.yaml" in out


def test_real_repo_passes():
    """Baseline: the real repo's six AEN presets (all currently pointing at
    firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin) resolve today -- exit 0."""
    proc = _run("--root", str(REPO))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout

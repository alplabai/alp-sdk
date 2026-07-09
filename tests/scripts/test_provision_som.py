"""Unit tests for scripts/provision_som.py -- dry-run orchestration only
(no hardware). Uses a fixture bundle, the real check_som_bundle + program_eeprom,
and a stub som_ledger so the private ledger isn't a dependency."""

import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "provision_som.py"


def _make_bundle(tmp_path, status="bootloader-only:image-pending-hw", with_image=False):
    d = tmp_path / "bundle"
    (d / "artifacts").mkdir(parents=True)
    h = "0" * 64
    (d / "artifacts" / "bl2.bin").write_bytes(b"BL2")
    (d / "artifacts" / "fip.bin").write_bytes(b"FIP")
    comps = [
        {"role": "bl2", "file": "artifacts/bl2.bin", "sha256": h, "size_bytes": 3,
         "flash_target": "xspi:mtd0"},
        {"role": "fip", "file": "artifacts/fip.bin", "sha256": h, "size_bytes": 3,
         "flash_target": "xspi:mtd1"},
    ]
    if with_image:
        (d / "artifacts" / "img.wic.gz").write_bytes(b"IMG")
        comps.append({"role": "system_image", "file": "artifacts/img.wic.gz",
                      "sha256": h, "size_bytes": 3, "flash_target": "emmc"})
    bundle = {
        "schema_version": 1, "sku": "E1M-V2N101", "family": "v2n", "hw_rev": "r1",
        "release_version": "som-1.0.0", "created": "2026-06-08", "status": status,
        "components": comps,
        "provenance": {"toolchain": "gcc-13.4.0", "u_boot_srcrev": "bcf29d98",
                       "tf_a_srcrev": "4092464", "uboot_defconfig": "rzv2n-dev_defconfig",
                       "patches": ["deepx"], "equivalence": "functional-equiv modulo gcc"},
        "signature": None,
    }
    (d / "bundle.json").write_text(json.dumps(bundle))
    return d


def _stub_ledger(tmp_path):
    """A fake som_ledger: `alloc` prints a serial; `record` appends its argv to a
    log file and exits 0. Lets us assert the ledger hook without the private tool."""
    log = tmp_path / "ledger_calls.txt"
    s = tmp_path / "stub_ledger.py"
    s.write_text(
        "import sys\n"
        f"open(r'{log}', 'a').write(' '.join(sys.argv[1:]) + '\\n')\n"
        "if 'alloc' in sys.argv: print('2026W24-0001')\n"
        "sys.exit(0)\n")
    return s, log


def _run(*args, env=None):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True, env=env)


def test_dry_run_bootloader_only_plans_bl2_fip_skips_image(tmp_path):
    d = _make_bundle(tmp_path)
    proc = _run("--bundle", str(d), "--serial", "2026W24-0001")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    out = proc.stdout
    assert "xSPI mtd0" in out and "xSPI mtd1" in out
    assert "skipped (bundle is bootloader-only)" in out
    assert "built 128-byte manifest" in out  # EEPROM step ran (real, host-side)


def test_dry_run_complete_bundle_plans_emmc(tmp_path):
    d = _make_bundle(tmp_path, status="complete", with_image=True)
    env = os.environ.copy()
    env["PATH"] = ""
    proc = _run("--bundle", str(d), "--serial", "2026W24-0001", env=env)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "yocto_wic" in proc.stdout or "emmc" in proc.stdout.lower()


def test_invalid_bundle_fails_fast(tmp_path):
    d = _make_bundle(tmp_path)
    b = json.loads((d / "bundle.json").read_text())
    b["sku"] = "NOT-A-SKU"           # fails the schema pattern
    (d / "bundle.json").write_text(json.dumps(b))
    proc = _run("--bundle", str(d), "--serial", "2026W24-0001")
    assert proc.returncode != 0
    assert "validation" in proc.stdout.lower() or "valid" in proc.stdout.lower()


def test_ledger_hook_allocs_and_records(tmp_path):
    d = _make_bundle(tmp_path)
    stub, log = _stub_ledger(tmp_path)
    proc = _run("--bundle", str(d), "--ledger-root", str(tmp_path / "ledger"),
                "--som-ledger", str(stub), "--by", "lab")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    calls = log.read_text()
    assert "alloc --sku E1M-V2N101" in calls
    assert "record" in calls and "2026W24-0001" in calls
    assert "--test-result pending-hw" in calls   # dry-run => pending-hw


def test_missing_serial_without_ledger_fails(tmp_path):
    d = _make_bundle(tmp_path)
    proc = _run("--bundle", str(d))   # no --serial, no --ledger-root
    assert proc.returncode != 0

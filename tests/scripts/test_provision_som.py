"""Unit tests for scripts/provision_som.py -- dry-run orchestration only
(no hardware). Uses a fixture bundle, the real check_som_bundle + program_eeprom,
and a stub som_ledger so the private ledger isn't a dependency. Of the two
--carrier tests below, only the one that resolves to a real HiL spec dir
(test_carrier_auto_derive_resolves_real_hil_spec) shells the real
tests/hil/run_smoke.py --validate, which needs PyYAML -- a core
`alp-sdk-cli` dependency (pyproject.toml), not an extra install, but not
"no hardware" isolation either. The other (..._miss_fails_loud) derives to a
dir that doesn't exist, so provision() fails at the HiL-spec check -- before
the flash loop, before _alloc_serial, and before run_smoke.py is ever
invoked."""

import json
import os
import subprocess
import sys
from pathlib import Path

import provision_som as ps

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


def test_carrier_auto_derive_resolves_real_hil_spec(tmp_path):
    """--carrier auto-derives tests/hil/<sku-without-'e1m-'>-<carrier>. The
    real dir is tests/hil/v2n101-x-evk (no e1m- prefix); regression for
    issue #1276 where the derive kept the prefix and pointed nowhere.

    Asserts on the derived path strings only, not proc.returncode -- the
    real run_smoke.py --validate verdict (and its PyYAML dependency) is not
    what this test pins; folding it in makes this test go red for reasons
    that have nothing to do with the derive."""
    d = _make_bundle(tmp_path)   # sku "E1M-V2N101" -> tests/hil/v2n101-x-evk
    proc = _run("--bundle", str(d), "--serial", "2026W24-0001", "--carrier", "x-evk")
    out = proc.stdout
    assert "e1m-v2n101-x-evk" not in out
    assert "v2n101-x-evk" in out
    assert "is not a directory" not in out
    assert "power-on test skipped" not in out   # it actually ran, not a silent no-op


def test_carrier_auto_derive_miss_fails_loud(tmp_path):
    """A --carrier that derives to a dir which doesn't exist must FAIL the
    step, not silently report ok=True like an unconfigured HiL spec does."""
    d = _make_bundle(tmp_path)
    proc = _run("--bundle", str(d), "--serial", "2026W24-0001",
                "--carrier", "not-a-real-carrier")
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "[FAIL] test:" in proc.stdout
    assert "is not a directory" in proc.stdout


def test_carrier_derive_miss_does_not_write_a_ledger_row(tmp_path):
    """A --carrier that derives to a missing dir must fail before the HiL
    spec dir is even checked against anything that costs a serial: no
    --serial is given here, so if the tool reached _alloc_serial it would
    allocate one from the stub ledger. It must not reach that far -- the
    derive is validated before flashing and before allocation, so a
    mis-derived --carrier can never burn a manufacturing serial with
    nothing recorded against it."""
    d = _make_bundle(tmp_path)
    stub, log = _stub_ledger(tmp_path)
    proc = _run("--bundle", str(d),
                "--carrier", "not-a-real-carrier",
                "--ledger-root", str(tmp_path / "ledger"), "--som-ledger", str(stub))
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert not log.exists(), log.read_text()   # no alloc call ever made, no orphaned serial


def test_execute_skipped_power_on_test_records_pending_hw_not_pass(tmp_path, monkeypatch):
    """With --execute and neither --hil-spec nor --carrier, the power-on
    test step is a no-op ("skipped"), not a passed test -- recording it as a
    ledger 'pass' is the same "reports success it did not earn" defect #1276
    names, one branch over from the --carrier derive.

    This drives provision() itself, because the wiring that turns a test
    step's outcome into a ledger verdict lives in provision()'s call to
    _record, not in either callee -- a test that called _power_on_test +
    _record directly would bypass that wiring entirely.

    flash:bl2/flash:fip need real hardware under --execute
    (xspi_flashwriter's real-write branch requires a real Flash Writer
    .mot and pyserial), which #1276 has nothing to do with, so _flash is
    stubbed to an immediate pass; validate, eeprom, test, and record all
    run for real."""
    d = _make_bundle(tmp_path)   # bootloader-only, no --carrier/--hil-spec
    stub, log = _stub_ledger(tmp_path)
    monkeypatch.setattr(
        ps, "_flash",
        lambda cfg, comp: ps.Step(f"flash:{comp['role']}", True, "stubbed -- not what #1276 tests"))
    cfg = ps.Cfg(bundle_dir=d, execute=True, serial="2026W24-0001",
                mfg_date="2026-06-08", ledger_root=tmp_path / "ledger",
                som_ledger=stub, by="lab")
    rc = ps.provision(cfg)
    assert rc == 0, log.read_text() if log.exists() else "<no ledger calls -- record step never ran>"
    calls = log.read_text()
    assert "--test-result pass" not in calls
    assert "--test-result pending-hw" in calls


def test_execute_failed_power_on_test_records_fail_not_pending_hw(tmp_path, monkeypatch):
    """A power-on test that was configured, ran, and FAILED records ledger
    'fail' -- not 'pass', and (since #1305) not 'pending-hw' either.

    Run against the pre-#1305 generator this asserts RED: `_record` emitted
    `"pass" if (cfg.execute and test_ran and test_ok) else "pending-hw"`, so
    a unit that failed its power-on test got the same ledger row as a unit
    nobody ever tested -- "not yet verified" and "verified bad" were the
    same string downstream.

    `som_ledger.py` lives in alp-sdk-internal and declares
    `choices=["pass", "fail", "pending-hw"]`, so the value this asserts is
    one the real tool accepts; emitting it does not make the record step
    exit non-zero on the bench path (the risk #1305 was held on).

    Stubs tests/hil/run_smoke.py itself (via --alp-sdk-root), not
    _power_on_test, so the real subprocess + returncode wiring that turns
    a failing HiL run into a ledger verdict is exercised end to end."""
    d = _make_bundle(tmp_path)
    stub, log = _stub_ledger(tmp_path)
    monkeypatch.setattr(
        ps, "_flash",
        lambda cfg, comp: ps.Step(f"flash:{comp['role']}", True, "stubbed -- not what #1276 tests"))
    fake_root = tmp_path / "fake_sdk"
    fake_hil = fake_root / "tests" / "hil"
    fake_hil.mkdir(parents=True)
    (fake_hil / "run_smoke.py").write_text("import sys\nsys.exit(1)\n")
    spec_dir = fake_hil / "stub-carrier"
    spec_dir.mkdir()
    cfg = ps.Cfg(bundle_dir=d, execute=True, serial="2026W24-0001",
                mfg_date="2026-06-08", ledger_root=tmp_path / "ledger",
                som_ledger=stub, by="lab", hil_spec=spec_dir, alp_sdk_root=fake_root)
    rc = ps.provision(cfg)
    assert rc == 1   # the test step itself failed
    calls = log.read_text() if log.exists() else ""
    assert "--test-result fail" in calls, calls
    assert "--test-result pass" not in calls
    assert "--test-result pending-hw" not in calls


def test_relative_hil_spec_resolves_against_alp_sdk_root_not_cwd(tmp_path, monkeypatch):
    """A relative --hil-spec must resolve against --alp-sdk-root (or the
    repo root), like the --carrier derive and the run_smoke.py runner do --
    not the process's current working directory, which docs/provisioning.md's
    bench example (`--hil-spec tests/hil/v2n101-x-evk`, run from the repo
    root) would otherwise hard-fail whenever CWD != that root."""
    d = _make_bundle(tmp_path)
    stub, log = _stub_ledger(tmp_path)
    monkeypatch.setattr(
        ps, "_flash",
        lambda cfg, comp: ps.Step(f"flash:{comp['role']}", True, "stubbed -- not what #1276 tests"))
    fake_root = tmp_path / "fake_sdk"
    fake_hil = fake_root / "tests" / "hil"
    fake_hil.mkdir(parents=True)
    (fake_hil / "run_smoke.py").write_text("import sys\nsys.exit(0)\n")
    (fake_hil / "board-x").mkdir()
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)
    cfg = ps.Cfg(bundle_dir=d, execute=True, serial="2026W24-0001",
                mfg_date="2026-06-08", ledger_root=tmp_path / "ledger",
                som_ledger=stub, by="lab",
                hil_spec=Path("tests/hil/board-x"), alp_sdk_root=fake_root)
    rc = ps.provision(cfg)
    calls = log.read_text() if log.exists() else "<no ledger calls>"
    assert rc == 0, calls
    assert "--test-result pass" in calls, calls


def test_eeprom_message_does_not_claim_execute_performed_the_write(tmp_path, monkeypatch, capsys):
    """The eeprom step's message must not vary with --execute in a way that
    implies the RIIC0 write happened -- it's HW-gated and this script never
    performs it, in dry-run or --execute alike.

    Drives provision() directly (like the other --execute tests) with
    _flash stubbed, since a real --execute flash needs hardware unrelated
    to what this test pins."""
    d = _make_bundle(tmp_path)
    monkeypatch.setattr(
        ps, "_flash",
        lambda cfg, comp: ps.Step(f"flash:{comp['role']}", True, "stubbed -- not what #1276 tests"))
    cfg = ps.Cfg(bundle_dir=d, execute=True, serial="2026W24-0001", mfg_date="2026-06-08")
    ps.provision(cfg)
    out = capsys.readouterr().out
    assert "(execute)" not in out
    assert "HW-gated" in out

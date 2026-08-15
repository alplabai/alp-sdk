#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""provision_som.py -- provision one E1M SoM from a Piece-5 release bundle.

Sequence (stop on first failure): validate bundle -> resolve and validate the
HiL spec dir -> flash bl2/fip (xSPI) + system image (eMMC) -> alloc serial +
build the EEPROM manifest -> power-on test -> record to the ledger.

The HiL spec dir is validated before anything is flashed and before a serial is
allocated, so a wrong --carrier cannot consume a manufacturing serial or leave a
written part with no ledger row against it.

Dry-run by DEFAULT; pass --execute to perform real flashing. The ledger steps
are optional (enabled by --ledger-root). See docs/provisioning.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
REPO = SCRIPTS.parent
# scripts/ is NOT a package (no __init__.py); the repo convention is to put
# scripts/ on sys.path and import flash_backends as a top-level package.
sys.path.insert(0, str(SCRIPTS))
import flash_backends as fb

# bundle.json flash_target -> (backend method, xspi partition or None)
_TARGET_BACKEND = {
    "xspi:mtd0": ("xspi_flashwriter", "mtd0"),
    "xspi:mtd1": ("xspi_flashwriter", "mtd1"),
    "emmc": ("yocto_wic_to_sd_or_emmc", None),
}
_FLASH_ORDER = {"bl2": 0, "fip": 1, "system_image": 2}


@dataclass
class Cfg:
    bundle_dir: Path
    execute: bool = False
    serial: str | None = None
    mfg_date: str = ""
    carrier: str = ""
    hil_spec: Path | None = None
    ledger_root: Path | None = None
    som_ledger: Path | None = None
    by: str = "provision_som"
    station: str | None = None
    port: str | None = None
    flash_writer: str | None = None
    emmc_device: str | None = None
    alp_sdk_root: Path | None = None


@dataclass
class Step:
    name: str
    ok: bool
    message: str
    command: list = field(default_factory=list)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _validate_bundle(cfg: Cfg):
    bj = cfg.bundle_dir / "bundle.json"
    if not bj.is_file():
        return None, Step("validate", False, f"no bundle.json in {cfg.bundle_dir}")
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / "check_som_bundle.py"), "--bundle", str(bj)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        return None, Step("validate", False,
                          f"bundle failed schema validation:\n{proc.stdout.strip()}")
    doc = json.loads(bj.read_text(encoding="utf-8"))
    return doc, Step("validate", True,
                     f"bundle {doc['release_version']} ({doc['status']}) valid")


def _flash(cfg: Cfg, comp: dict) -> Step:
    target = comp["flash_target"]
    spec = _TARGET_BACKEND.get(target)
    if spec is None:
        return Step(f"flash:{comp['role']}", False, f"no backend for {target}")
    method, partition = spec
    backend = fb.lookup(method)
    if backend is None:
        return Step(f"flash:{comp['role']}", False, f"backend {method} not registered")
    flash_args: dict = {}
    if partition is not None:
        flash_args["flash_partition"] = partition
        if cfg.port:
            flash_args["port"] = cfg.port
        if cfg.flash_writer:
            flash_args["flash_writer"] = cfg.flash_writer
        if cfg.execute:
            flash_args["confirm"] = True
    else:  # emmc / yocto_wic
        flash_args["target"] = cfg.emmc_device or "/dev/<emmc>"
        if cfg.execute:
            flash_args["confirm"] = True
    ctx = fb.FlashContext(
        artefact_path=cfg.bundle_dir / comp["file"], flash_args=flash_args,
        core_id=comp["role"], sku="", sdk_root=cfg.alp_sdk_root, dry_run=not cfg.execute)
    r = backend.flash(ctx)
    return Step(f"flash:{comp['role']}", r.ok, r.message, r.command)


def _alloc_serial(cfg: Cfg, bundle: dict):
    if cfg.serial:
        return cfg.serial, Step("serial", True, f"using provided serial {cfg.serial}")
    if not (cfg.ledger_root and cfg.som_ledger):
        return None, Step("serial", False,
                          "no --serial and no --ledger-root/--som-ledger to alloc from")
    cmd = [sys.executable, str(cfg.som_ledger), "--ledger-root", str(cfg.ledger_root),
           "alloc", "--sku", bundle["sku"]]
    if cfg.mfg_date:
        cmd += ["--date", cfg.mfg_date]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        return None, Step("serial", False, f"alloc failed: {proc.stderr.strip()}")
    serial = proc.stdout.strip()
    if not serial:
        return None, Step("serial", False, "alloc returned an empty serial")
    return serial, Step("serial", True, f"allocated serial {serial}", cmd)


def _eeprom(cfg: Cfg, bundle: dict):
    """Build the 128-byte manifest (real, host-side). The i2c write+verify is
    HW-gated -- this step only plans it; --execute does not perform it (no
    backend exists to), so the step's message must never vary with
    cfg.execute in a way that implies otherwise."""
    tmp = Path(tempfile.mkdtemp(prefix="provision_"))
    # write-text-newline-exempt: tempdir board.yaml, never in the repo tree
    (tmp / "board.yaml").write_text(
        f"som:\n  sku: {bundle['sku']}\n  hw_rev: {bundle['hw_rev']}\n", encoding="utf-8")
    manifest = tmp / "eeprom-manifest.bin"
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / "program_eeprom.py"),
         "--board-yaml", str(tmp / "board.yaml"), "--serial", cfg.serial,
         "--mfg-date", cfg.mfg_date, "--output", str(manifest)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        return None, Step("eeprom", False,
                          f"program_eeprom failed: {(proc.stderr or proc.stdout).strip()}")
    plan = ["i2c-write", "bus=RIIC0", "addr=0x50", "offset=0x0",
            f"bytes={manifest.stat().st_size}", "read-back-verify"]
    return manifest, Step(
        "eeprom", True,
        f"built 128-byte manifest for serial {cfg.serial}; RIIC0 @0x50 write + "
        f"read-back-verify is HW-gated -- this script only plans it, in "
        f"dry-run and --execute alike", plan)


def _power_on_test(cfg: Cfg) -> tuple[bool, Step]:
    """Returns (test_ran, step). A skipped test (no --hil-spec/--carrier) is
    not a failure of this *step* -- it's an intentional no-op when nobody
    asked for one -- but the caller must never fold that into "passed": see
    _record's `test_ran`/`test_ok` gate. provision() already validated and
    resolved cfg.hil_spec to an existing absolute directory (before
    flashing or allocating a serial), so by the time this runs it's either
    None (no test wanted) or known-good."""
    if not cfg.hil_spec:
        ledger_note = (" -- ledger records pending-hw, not pass"
                       if cfg.ledger_root and cfg.som_ledger else "")
        return False, Step("test", True,
                          f"no HiL spec configured; power-on test skipped{ledger_note}")
    root = cfg.alp_sdk_root or REPO
    runner = root / "tests" / "hil" / "run_smoke.py"
    mode = [] if cfg.execute else ["--validate"]
    cmd = [sys.executable, str(runner), *mode, str(cfg.hil_spec)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    ok = proc.returncode == 0
    verb = "ran HiL smoke" if cfg.execute else "validated HiL spec (would run smoke)"
    return True, Step("test", ok, f"{verb} {cfg.hil_spec} rc={proc.returncode}", cmd)


def _record(cfg: Cfg, bundle: dict, manifest: Path, test_ran: bool, test_ok: bool) -> Step:
    """Three-valued, one value per outcome (#1305):

      * `pass`        -- --execute, a test was configured, it ran, it passed.
      * `fail`        -- --execute, a test was configured, it ran, it FAILED.
      * `pending-hw`  -- everything else: dry-run, no test configured, or a
                         test skipped under --execute.

    `test_ran` must mean the power-on test was actually configured and
    invoked -- a skipped test is not a passed test (#1276) -- so `test_ok`
    only earns a `pass` when `test_ran` is also True.

    `cfg.execute` gates BOTH `pass` and `fail`, and that is load-bearing, not
    symmetry for its own sake: `_power_on_test` returns `test_ran=True` in a
    dry run too, because it still invokes the runner with `--validate` to
    check the spec. A spec that fails validation on a workstation with no
    board attached is not a unit that failed its power-on test, and must not
    be recorded as one.

    Until #1305 this collapsed `fail` into `pending-hw`, so a unit whose
    power-on test executed and failed was indistinguishable in the ledger
    from a unit never tested -- "not yet verified" and "verified bad" read
    the same. `som_ledger.py` lives in alp-sdk-internal and accepts
    `choices=["pass", "fail", "pending-hw"]`, so the private side takes the
    third value; that was the ordering constraint #1305 named, and it is
    met."""
    if not (cfg.ledger_root and cfg.som_ledger):
        return Step("record", True, "no --ledger-root/--som-ledger; ledger record skipped")
    if cfg.execute and test_ran:
        result = "pass" if test_ok else "fail"
    else:
        result = "pending-hw"
    cmd = [sys.executable, str(cfg.som_ledger), "--ledger-root", str(cfg.ledger_root),
           "record", "--sku", bundle["sku"], "--serial", cfg.serial,
           "--family", bundle["family"], "--hw-rev", bundle["hw_rev"],
           "--bundle-version", bundle["release_version"],
           "--bundle-sha256", _sha256(cfg.bundle_dir / "bundle.json"),
           "--mfg-date", cfg.mfg_date, "--eeprom-bin", str(manifest),
           "--test-result", result, "--by", cfg.by]
    if cfg.station:
        cmd += ["--station", cfg.station]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    ok = proc.returncode == 0
    detail = "ok" if ok else (proc.stderr or proc.stdout).strip()
    return Step("record", ok, f"ledger record {cfg.serial} ({result}): {detail}", cmd)


def provision(cfg: Cfg) -> int:
    steps: list[Step] = []

    def done():
        _print(steps, cfg.execute)
        return 0 if all(s.ok for s in steps) else 1

    bundle, sv = _validate_bundle(cfg)
    steps.append(sv)
    if not sv.ok:
        return done()

    # --carrier selects the per-SoM HiL smoke set when --hil-spec isn't given.
    # tests/hil/<dir> is NOT "e1m-"-prefixed (e.g. tests/hil/v2n101-x-evk), even
    # though bundle['sku'] is (e.g. "E1M-V2N101") -- strip it before deriving.
    root = cfg.alp_sdk_root or REPO
    if cfg.hil_spec is None and cfg.carrier:
        sku_dir = bundle["sku"].lower().removeprefix("e1m-")
        cfg.hil_spec = root / "tests" / "hil" / f"{sku_dir}-{cfg.carrier}"

    # Validate the HiL spec dir (derived or given) here, before anything
    # irreversible happens below -- xSPI flashing, allocating a manufacturing
    # serial. Checking this later (inside the test step, after flash+alloc)
    # let a mis-derived --carrier burn an allocated serial with no ledger row
    # ever recorded against it.
    if cfg.hil_spec is not None:
        cfg.hil_spec = cfg.hil_spec if cfg.hil_spec.is_absolute() else root / cfg.hil_spec
        if not cfg.hil_spec.is_dir():
            steps.append(Step("test", False,
                              f"HiL spec dir is not a directory: {cfg.hil_spec}"))
            return done()

    bootloader_only = bundle["status"].startswith("bootloader-only")
    image_skipped = False
    for comp in sorted(bundle["components"], key=lambda c: _FLASH_ORDER.get(c["role"], 9)):
        if comp["role"] == "system_image" and bootloader_only:
            steps.append(Step("flash:system_image", True, "skipped (bundle is bootloader-only)"))
            image_skipped = True
            continue
        steps.append(_flash(cfg, comp))
        if not steps[-1].ok:
            return done()
    if bootloader_only and not image_skipped:
        steps.append(Step("flash:system_image", True, "skipped (bundle is bootloader-only)"))

    serial, ss = _alloc_serial(cfg, bundle)
    steps.append(ss)
    if not ss.ok:
        return done()
    cfg.serial = serial

    manifest, es = _eeprom(cfg, bundle)
    steps.append(es)
    if not es.ok:
        return done()

    test_ran, ts = _power_on_test(cfg)
    steps.append(ts)
    steps.append(_record(cfg, bundle, manifest, test_ran, ts.ok))
    # _eeprom created a temp dir that held the manifest through _record; clean it.
    if manifest is not None:
        shutil.rmtree(manifest.parent, ignore_errors=True)
    return done()


def _print(steps: list[Step], execute: bool) -> None:
    mode = "EXECUTE" if execute else "DRY-RUN"
    print(f"=== provision_som [{mode}] ===")
    for s in steps:
        mark = "OK  " if s.ok else "FAIL"
        print(f"[{mark}] {s.name}: {s.message}")
        if s.command:
            print(f"        cmd: {' '.join(str(x) for x in s.command)}")
    bad = [s.name for s in steps if not s.ok]
    print(f"--- {'all steps ok' if not bad else 'FAILED at: ' + ', '.join(bad)} ---")


def main() -> int:
    ap = argparse.ArgumentParser(description="Provision one E1M SoM from a Piece-5 bundle.")
    ap.add_argument("--bundle", type=Path, required=True, help="bundle dir (bundle.json + artifacts/)")
    ap.add_argument("--execute", action="store_true", help="actually flash/program (default: dry-run)")
    ap.add_argument("--serial", help="explicit serial; else alloc via --ledger-root")
    ap.add_argument("--mfg-date", default=date.today().isoformat())
    ap.add_argument("--carrier", default="",
                    help="derives --hil-spec as tests/hil/<sku-without-'e1m-'>-<carrier> "
                         "(e.g. sku E1M-V2N101 + --carrier x-evk -> tests/hil/v2n101-x-evk); "
                         "ignored if --hil-spec is given")
    ap.add_argument("--hil-spec", type=Path,
                    help="tests/hil/<board> dir for the power-on test; a relative path "
                         "resolves against --alp-sdk-root (or the repo root), not the "
                         "working directory; a dir that doesn't exist FAILS the step "
                         "(it's not treated as 'no test wanted' -- only omitting both "
                         "--hil-spec and --carrier does that)")
    ap.add_argument("--ledger-root", type=Path, help="enable ledger alloc+record (private)")
    ap.add_argument("--som-ledger", type=Path, help="path to som_ledger.py (private)")
    ap.add_argument("--by", default="provision_som")
    ap.add_argument("--station")
    ap.add_argument("--port", help="serial port for the xSPI Flash Writer")
    ap.add_argument("--flash-writer", help="path to Flash_Writer_SCIF_*.mot")
    ap.add_argument("--emmc-device", help="host block device for the eMMC image (/dev/...)")
    ap.add_argument("--alp-sdk-root", type=Path)
    a = ap.parse_args()
    cfg = Cfg(bundle_dir=a.bundle, execute=a.execute, serial=a.serial, mfg_date=a.mfg_date,
              carrier=a.carrier, hil_spec=a.hil_spec, ledger_root=a.ledger_root,
              som_ledger=a.som_ledger, by=a.by, station=a.station, port=a.port,
              flash_writer=a.flash_writer, emmc_device=a.emmc_device, alp_sdk_root=a.alp_sdk_root)
    return provision(cfg)


if __name__ == "__main__":
    sys.exit(main())

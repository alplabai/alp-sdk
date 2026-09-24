"""provision_som.py plan | run | status (the V2N flow's CLI), end to end in a
subprocess, offline. The real-bundle test needs the PRIVATE repo: point
ALP_SDK_INTERNAL at a checkout (or have it as a sibling) or it is skipped."""

from __future__ import annotations

import gzip
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

from .test_provision_gates import _ext4, _wic
from .test_provision_steps import BL2, CATALOGUE, DTB, FIP, MARKERS, SERIAL, SKU

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "provision_som.py"
D16_BL2 = b"\x00" * 64 + bytes.fromhex(MARKERS["markers"][1]["hex"])


def _run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, check=False,
                          text=True, encoding="utf-8", env={**os.environ, "PYTHONIOENCODING": "utf-8"})


def _bundle(tmp_path, bl2=BL2, status="complete"):
    d = tmp_path / "bundle"
    (d / "artifacts").mkdir(parents=True)
    comps = []
    for role, name, data, target in (
            ("bl2", "bl2_bp_spi.bin", bl2, "xspi:mtd0"), ("bl2_mmc", "bl2_bp_mmc.bin", bl2, "emmc:boot1"),
            ("fip", "fip.bin", FIP, "xspi:mtd1"),
            ("system_image", "img.wic.gz", gzip.compress(_wic(_ext4(["Image", DTB])), mtime=0), "emmc")):
        (d / "artifacts" / name).write_bytes(data)
        comps.append({"role": role, "file": f"artifacts/{name}", "sha256": hashlib.sha256(data).hexdigest(),
                      "size_bytes": len(data), "flash_target": target})
    bundle = {"schema_version": 1, "sku": SKU, "family": "v2n-m1", "hw_rev": "r1",
              "release_version": "som-9.9.9", "created": "2026-09-24", "status": status,
              "memory_tier": {"dram_mbit": 32768}, "components": comps,
              "provenance": {"toolchain": "gcc-13.4.0", "u_boot_srcrev": "bcf29d98",
                             "tf_a_srcrev": "4092464", "uboot_defconfig": "rzv2n-dev_defconfig",
                             "patches": ["deepx"], "equivalence": "functional-equiv modulo gcc"},
              "signature": None}
    (d / "bundle.json").write_text(json.dumps(bundle), encoding="utf-8")
    return d


def _inputs(tmp_path):
    ledger = tmp_path / "ledger"
    (ledger / "schema").mkdir(parents=True)
    (ledger / "schema" / "v2n.keys.yaml").write_text(yaml.safe_dump(CATALOGUE), encoding="utf-8")
    markers = tmp_path / "markers.json"
    markers.write_text(json.dumps(MARKERS), encoding="utf-8")
    return ledger, markers


def test_offline_plan_passes_schema_and_gates(tmp_path):
    ledger, markers = _inputs(tmp_path)
    p = _run("plan", "--sku", SKU, "--serial", SERIAL, "--bundle", _bundle(tmp_path),
             "--ledger-root", ledger, "--tier-markers", markers)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "[DONE   ] preflight" in p.stdout and "[PLANNED] bootstrap" in p.stdout
    assert not (ledger / SKU).exists()                     # a plan writes nothing


def test_plan_refuses_tier(tmp_path):
    ledger, markers = _inputs(tmp_path)
    p = _run("plan", "--sku", SKU, "--serial", SERIAL, "--bundle", _bundle(tmp_path, bl2=D16_BL2),
             "--ledger-root", ledger, "--tier-markers", markers)
    assert p.returncode == 1
    assert "DDR tier mismatch" in p.stdout and "FAILED at: preflight" in p.stdout


def _internal() -> Path | None:
    cands = [os.environ.get("ALP_SDK_INTERNAL", "")]
    cands += [str(parent / "alp-sdk-internal") for parent in REPO.parents]
    for c in filter(None, cands):
        p = Path(c)
        if (p / "releases" / "E1M-V2N101" / "som-0.2.0" / "bundle.json").is_file() and \
                (p / "metadata" / "ddr-tier-markers.json").is_file():
            return p
    return None


@pytest.mark.skipif(_internal() is None, reason="private alp-sdk-internal checkout not found")
def test_plan_real_som_0_2_0_refuses_tier_on_4gb_preset(tmp_path):
    priv = _internal()
    ledger, _ = _inputs(tmp_path)
    p = _run("plan", "--sku", SKU, "--serial", SERIAL,
             "--bundle", priv / "releases" / "E1M-V2N101" / "som-0.2.0",
             "--ledger-root", ledger, "--tier-markers", priv / "metadata" / "ddr-tier-markers.json")
    assert p.returncode == 1, p.stdout + p.stderr
    assert "DDR tier mismatch" in p.stdout and "D16S32" in p.stdout


def test_usage_errors_exit_2(tmp_path):
    ledger, _ = _inputs(tmp_path)
    b = _bundle(tmp_path)
    assert _run("plan", "--sku", "E1M-NOPE", "--bundle", b, "--ledger-root", ledger).returncode == 2
    assert _run("plan", "--sku", SKU, "--bundle", b, "--ledger-root", ledger,
                "--only", "bogus").returncode == 2
    assert _run("run", "--sku", SKU, "--serial", SERIAL, "--bundle", b,
                "--ledger-root", ledger).returncode == 2              # run needs --bench
    assert _run("plan", "--sku", SKU).returncode == 2


def test_status_reports_steps_and_ship_blockers(tmp_path):
    ledger, _ = _inputs(tmp_path)
    d = ledger / SKU
    d.mkdir()
    (d / f"{SERIAL}.state.json").write_text(json.dumps(
        {"schema": 1, "steps": {"preflight": {"status": "done", "at": "2026-09-24T00:00:00Z"}},
         "overrides": [{"gate": "tier_triangle", "reason": "bench", "at": "2026-09-24T00:00:00Z"}]}),
        encoding="utf-8")
    (d / f"{SERIAL}.unit.yaml").write_text("act88760_gpio4_defect: yes\ndisposition: bench-only\n",
                                            encoding="utf-8")
    p = _run("status", "--sku", SKU, "--serial", SERIAL, "--ledger-root", ledger)
    assert p.returncode == 1
    assert "preflight" in p.stdout and "done" in p.stdout and "override tier_triangle" in p.stdout
    assert "gpio4_defect" in p.stdout and "missing eeprom_unique_id" in p.stdout


def test_legacy_flat_flow_skips_bl2_mmc(tmp_path):
    p = _run("--bundle", _bundle(tmp_path), "--serial", SERIAL)
    assert "flash:bl2_mmc: skipped (emmc:boot1" in p.stdout, p.stdout

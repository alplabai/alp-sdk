# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_system_manifest.py + the system-manifest v1 contract.

The manifest is the single derived projection of board.yaml that tools (the
alp-sdk-vscode extension, CI, flashers) consume. These lock the schema
against the committed corpus + a caller-supplied manifest, and the gate
behaviour -- see check_system_manifest.py's WHAT WAS LOST docstring for why
this no longer regenerates from board.yaml via the (deleted) orchestrator.
"""
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_system_manifest.py"
SCHEMA = REPO / "metadata" / "schemas" / "system-manifest-v1.schema.json"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def test_schema_is_valid_draft202012():
    import jsonschema
    jsonschema.Draft202012Validator.check_schema(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def test_default_corpus_conforms():
    # the committed corpus at tests/fixtures/emit-snapshots/*.system-manifest.snap
    # matches the documented contract.
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 failure(s)" in proc.stdout


def test_multicore_manifest_is_the_per_image_map():
    # reads the committed snapshot rather than emitting one -- see
    # check_system_manifest.py's WHAT WAS LOST docstring.
    import jsonschema
    import yaml
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    v = jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())
    snap = REPO / "tests/fixtures/emit-snapshots/rpmsg-v2n.system-manifest.snap"
    doc = yaml.safe_load(snap.read_text(encoding="utf-8"))
    assert list(v.iter_errors(doc)) == []
    # one slice per core -- the multi-image map the IDE consumes
    by_id = {s["core_id"]: s for s in doc["slices"]}
    assert set(by_id) == {"a55_cluster", "m33_sm"}
    assert by_id["a55_cluster"]["os"] == "yocto"
    assert by_id["m33_sm"]["os"] == "zephyr"
    # every slice carries the wiring the IDE needs, no re-derivation
    for s in by_id.values():
        assert {"core_id", "os", "status", "flash_method", "flash_args"} <= set(s)


def test_broken_slice_rejected(tmp_path):
    bad = {
        "schema_version": 1,
        "generated_by": "test",
        "hw_info": {"sku": "E1M-V2N101", "som_hw_rev": "r1", "board_name": None,
                    "board_hw_rev": None, "silicon": "renesas:rzv2n:n44"},
        # slice missing the required flash_method + flash_args
        "slices": [{"core_id": "m33_sm", "os": "zephyr", "status": "pending"}],
        "ipc": [], "helper_mcus": [], "boot_order": [],
    }
    p = tmp_path / "system-manifest.yaml"
    p.write_text(json.dumps(bad))
    proc = _run("--manifest", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_unknown_top_level_key_rejected(tmp_path):
    doc = {
        "schema_version": 1, "generated_by": "test",
        "hw_info": {"sku": "X", "som_hw_rev": None, "board_name": None,
                    "board_hw_rev": None, "silicon": None},
        "slices": [], "ipc": [], "helper_mcus": [], "boot_order": [],
        "bogus_key": 1,   # additionalProperties:false must catch drift/typos
    }
    p = tmp_path / "m.yaml"
    p.write_text(json.dumps(doc))
    proc = _run("--manifest", str(p))
    assert proc.returncode != 0


def test_hw_info_eeprom_projection_allowed(tmp_path):
    doc = {
        "schema_version": 1, "generated_by": "test",
        "hw_info": {
            "sku": "E1M-V2N101", "som_hw_rev": "r1",
            "board_name": "E1M-X-EVK", "board_hw_rev": None,
            "silicon": "renesas:rzv2n:n44",
            "eeprom": {
                "bus": "e1m_i2c0", "bus_id": 0,
                "addr_7bit": 0x54, "offset": 32,
            },
        },
        "slices": [], "ipc": [], "helper_mcus": [], "boot_order": [],
    }
    p = tmp_path / "m.yaml"
    p.write_text(json.dumps(doc))
    proc = _run("--manifest", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr

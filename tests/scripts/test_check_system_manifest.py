# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_system_manifest.py + the system-manifest v1 contract.

The manifest is the single derived projection of board.yaml that tools (the
alp-sdk-vscode extension, CI, flashers) consume. These lock the emitter <->
contract lockstep and the gate behaviour.
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
    # the orchestrator's emitter output for representative projects matches
    # the documented contract (emitter <-> schema lockstep / drift detection).
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 failure(s)" in proc.stdout


def test_multicore_manifest_is_the_per_image_map():
    sys.path.insert(0, str(REPO / "scripts"))
    import jsonschema
    import yaml
    from alp_orchestrate import emit_system_manifest, load_board_yaml
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    v = jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())
    doc = yaml.safe_load(emit_system_manifest(
        load_board_yaml(REPO / "examples/multicore/rpmsg-v2n/board.yaml")))
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


# ---------------------------------------------------------------------
# The memory[] name join (#1365 item 3) -- JSON Schema cannot express it,
# so the gate carries it in Python.  Both directions are pinned: dropping
# the check must turn one of these red.
# ---------------------------------------------------------------------


def _manifest(**panes):
    doc = {
        "schema_version": 1, "generated_by": "test",
        "hw_info": {
            "sku": "E1M-AEN801", "som_hw_rev": "r1",
            "board_name": "E1M-EVK", "board_hw_rev": None,
            "silicon": "alif:ensemble:e8",
        },
        "slices": [], "ipc": [], "helper_mcus": [], "boot_order": [],
    }
    doc.update(panes)
    return doc


_MRAM_ROW = {
    "name": "mram_main", "source": "som_preset",
    "kind": "flash", "status": "ok",
}


def test_a_carve_out_region_naming_a_declared_region_passes(tmp_path):
    doc = _manifest(
        memory=[_MRAM_ROW],
        ipc=[{"name": "link", "kind": "rpmsg", "endpoints": ["a", "b"],
              "carve_out_region": "mram_main"}],
    )
    p = tmp_path / "m.yaml"
    p.write_text(json.dumps(doc))
    proc = _run("--manifest", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_a_carve_out_region_naming_no_region_is_refused(tmp_path):
    """The refusing direction, pinned: a typo'd or stale
    `carve_out_region` must fail the gate rather than ship a link that
    points at nothing."""
    doc = _manifest(
        memory=[_MRAM_ROW],
        ipc=[{"name": "link", "kind": "rpmsg", "endpoints": ["a", "b"],
              "carve_out_region": "mram_mian"}],
    )
    p = tmp_path / "m.yaml"
    p.write_text(json.dumps(doc))
    proc = _run("--manifest", str(p))
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "carve_out_region 'mram_mian' names no memory[] region" in proc.stdout


def test_the_join_is_skipped_when_the_memory_pane_is_absent(tmp_path):
    """An absent `memory:` means a producer older than #1365 item 3, or a
    SoM whose layout is still pending -- neither is a join failure."""
    doc = _manifest(
        ipc=[{"name": "link", "kind": "rpmsg", "endpoints": ["a", "b"],
              "carve_out_region": "anything_at_all"}],
    )
    p = tmp_path / "m.yaml"
    p.write_text(json.dumps(doc))
    proc = _run("--manifest", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr

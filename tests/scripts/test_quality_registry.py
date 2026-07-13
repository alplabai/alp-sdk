# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/quality-tasks-v1.schema.json"
REGISTRY = REPO / "metadata/quality-tasks-v1.json"


def test_schema_is_closed_draft2020():
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["schemaVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)


def test_registry_validates_and_covers_all_check_scripts():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(reg)
    on_disk = {p.name for p in (REPO / "scripts").glob("check_*.py")
               if p.name != "check_quality_registry.py"}
    listed = {Path(t["script"]).name for t in reg["tasks"]
              if t["runner"] == "check-script"}
    assert listed == on_disk, f"orphan={on_disk-listed} phantom={listed-on_disk}"


def test_registry_gate_set_superset_of_legacy_17():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    gate = {Path(t["script"]).name for t in reg["tasks"]
            if t["runner"] == "check-script" and t["gate"]}
    legacy17 = {
        "check_pin_conflicts.py", "check_e1m_pinout.py",
        "check_inference_backend_parity.py", "check_e1m_route_capability.py",
        "check_emit_snapshots.py", "check_stub_symbol_matrix.py",
        "check_stub_issues.py", "check_vendor_ext_tags.py",
        "check_public_header_purity.py", "check_local_paths.py",
        "check_sw_fallback_tags.py", "check_som_bundle.py",
        "check_chip_manifest_parity.py", "check_chip_header_status.py",
        "check_example_portability.py", "check_doc_drift.py",
        "check_version_doc_sync.py"}
    assert legacy17 <= gate, f"regression: dropped {legacy17-gate}"


def test_informational_scripts_not_gated():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    by_script = {Path(t["script"]).name: t for t in reg["tasks"]
                 if t["runner"] == "check-script"}
    assert by_script["check_test_coverage.py"]["gate"] is False
    assert by_script["check_cross_platform.py"]["gate"] is False


import sys
sys.path.insert(0, str(REPO / "scripts"))
import quality_tasks  # noqa: E402


def test_gate_scripts_are_gated_check_scripts():
    gs = quality_tasks.gate_scripts()
    assert "scripts/check_doc_drift.py" in gs
    assert "scripts/check_test_coverage.py" not in gs  # informational
    assert gs == sorted(gs)


def test_cli_gate_scripts_prints_one_per_line(capsys):
    quality_tasks.main(["--gate-scripts"])
    out = capsys.readouterr().out.strip().splitlines()
    assert "scripts/check_doc_drift.py" in out


def test_scripts_for_profile_subset_of_check_scripts():
    pr = set(quality_tasks.scripts_for_profile("pr"))
    assert pr <= set(quality_tasks.check_scripts())
    assert "scripts/check_doc_drift.py" in pr

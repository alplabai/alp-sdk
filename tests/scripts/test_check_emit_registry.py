# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_emit_registry.py + the emit-registry v1 contract.

The registry is the single source of truth for every `--emit` mode the SDK
exposes (scripts/alp_project.py + scripts/alp_orchestrate/cli.py, re-exposed
by scripts/alp_cli/emit.py). These lock the schema/registry validity and the
code<->registry drift-detection gate.
"""
import copy
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_emit_registry.py"
SCHEMA = REPO / "metadata" / "schemas" / "emit-registry-v1.schema.json"
REGISTRY = REPO / "metadata" / "emit-registry-v1.json"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def _registry() -> dict:
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def test_schema_is_valid_draft202012():
    import jsonschema
    jsonschema.Draft202012Validator.check_schema(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def test_committed_registry_conforms():
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "in sync with the code" in proc.stdout


def test_committed_registry_is_valid_json():
    json.loads(REGISTRY.read_text(encoding="utf-8"))


def test_real_emit_modes_matches_registry_exactly():
    sys.path.insert(0, str(REPO / "scripts"))
    import check_emit_registry as cer
    code_modes = cer.real_emit_modes()
    registry_modes = {m["mode"] for m in _registry()["modes"]}
    assert code_modes == registry_modes


def test_every_mode_field_is_grounded_in_code_paths():
    for m in _registry()["modes"]:
        assert (REPO / m["owner"]["module"]).is_file(), m["mode"]
        assert m["owner"]["cli"] in ("alp_project", "alp_orchestrate")
        assert m["scope"] in ("project", "core", "system")


def test_missing_mode_rejected(tmp_path):
    doc = copy.deepcopy(_registry())
    doc["modes"] = [m for m in doc["modes"] if m["mode"] != "build-plan"]
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0
    assert "missing from the registry" in proc.stdout
    assert "build-plan" in proc.stdout


def test_phantom_mode_rejected(tmp_path):
    doc = copy.deepcopy(_registry())
    phantom = copy.deepcopy(doc["modes"][0])
    phantom["mode"] = "totally-fake-mode"
    doc["modes"].append(phantom)
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0
    assert "phantom entries" in proc.stdout
    assert "totally-fake-mode" in proc.stdout


def test_unknown_top_level_key_rejected(tmp_path):
    doc = copy.deepcopy(_registry())
    doc["bogus_key"] = 1
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0


def test_mode_missing_required_field_rejected(tmp_path):
    doc = copy.deepcopy(_registry())
    del doc["modes"][0]["scope"]
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0


def test_bad_scope_enum_rejected(tmp_path):
    doc = copy.deepcopy(_registry())
    doc["modes"][0]["scope"] = "galaxy"
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0


def test_malformed_json_rejected(tmp_path):
    p = tmp_path / "registry.json"
    p.write_text("not json", encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0
    assert "parse error" in proc.stdout

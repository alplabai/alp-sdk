# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_emit_registry.py + the emit-registry v1 catalogue.

The registry is the single source of truth for every `--emit` mode the SDK
publishes. It used to be checked for EQUALITY against the `--emit ...
choices=[...]` lists in `scripts/alp_project.py` and
`scripts/alp_orchestrate/cli.py`; both are being deleted with the planner, and
after that no file here enumerates the full surface. The gate now checks the
catalogue against the two PARTIAL enumerations alp-sdk still holds -- the
committed emit snapshots and `west alp-emit`'s `_EMIT_MODES` -- as subsets, plus
`owner.module` paths that name this repo.

`test_phantom_mode_rejected` is deliberately GONE rather than rewritten: a
registry entry nothing implements can no longer be detected from this
repository, because the implementation is in another one. Re-establishing it is
tan-side work. A test asserting the registry against itself would have kept the
name and lost the meaning.
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
    assert "emit modes catalogued" in proc.stdout


def test_committed_registry_is_valid_json():
    json.loads(REGISTRY.read_text(encoding="utf-8"))


def test_west_alp_emit_modes_are_all_catalogued():
    """The surviving half of the old equality check.

    `west alp-emit` is the one in-repo CLI that still enumerates emit modes
    after the planner leaves, and it offers 8 of the 20. Read through the
    gate's own `ast` reader, so this test and the gate cannot disagree about
    what the west command declares.
    """
    sys.path.insert(0, str(REPO / "scripts"))
    import check_emit_registry as cer
    registry_modes = {m["mode"] for m in _registry()["modes"]}
    assert cer.west_emit_modes() <= registry_modes


def test_every_mode_field_is_grounded_in_code_paths():
    for m in _registry()["modes"]:
        assert (REPO / m["owner"]["module"]).is_file(), m["mode"]
        assert m["owner"]["cli"] in ("alp_project", "alp_orchestrate")
        assert m["scope"] in ("project", "core", "system")


def test_a_mode_west_offers_but_the_registry_omits_is_rejected(tmp_path):
    """Replaces `test_missing_mode_rejected`, which planted its break in the
    old AST-of-alp_project.py path. `kconfig` is in `_EMIT_MODES`, so dropping
    it from the catalogue is the same defect in the surviving direction."""
    doc = copy.deepcopy(_registry())
    doc["modes"] = [m for m in doc["modes"] if m["mode"] != "kconfig"]
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0
    assert "west alp-emit" in proc.stdout
    assert "kconfig" in proc.stdout


def test_an_owner_module_naming_a_deleted_path_is_rejected(tmp_path):
    """The check that keeps the catalogue from describing code that is gone --
    which is exactly the state the planner deletion creates if the fields are
    not repointed in the same commit."""
    doc = copy.deepcopy(_registry())
    doc["modes"][0]["owner"]["module"] = "scripts/gone/nowhere.py"
    p = tmp_path / "registry.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--registry", str(p))
    assert proc.returncode != 0
    assert "does not exist" in proc.stdout


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

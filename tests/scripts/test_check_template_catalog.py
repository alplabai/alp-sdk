# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_template_catalog.py + the template-catalog v1
contract (metadata/templates/catalog-v1.json).

The catalog is the single source of truth for the SDK's project-template
archetypes; these lock the schema/catalog validity and the drift-detection
gate, including the #448/#520 known-content-quality-gap rule.
"""
import copy
import json
import subprocess
import sys
from pathlib import Path

import check_template_catalog as ctc  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_template_catalog.py"
SCHEMA = REPO / "metadata" / "schemas" / "template-catalog-v1.schema.json"
CATALOG = REPO / "metadata" / "templates" / "catalog-v1.json"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def _catalog() -> dict:
    return json.loads(CATALOG.read_text(encoding="utf-8"))


def _write(tmp_path, doc) -> Path:
    p = tmp_path / "catalog.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    return p


def test_schema_is_valid_draft202012():
    import jsonschema
    jsonschema.Draft202012Validator.check_schema(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def test_committed_catalog_conforms():
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "no drift" in proc.stdout


def test_committed_catalog_is_valid_json():
    json.loads(CATALOG.read_text(encoding="utf-8"))


def test_every_archetype_has_a_record():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    archetypes = set(
        schema["$defs"]["template"]["properties"]["archetype"]["enum"])
    present = {t["archetype"] for t in _catalog()["templates"]}
    assert archetypes == present


def test_every_example_path_exists_and_has_board_yaml():
    for t in _catalog()["templates"]:
        example_dir = REPO / t["example"]
        assert example_dir.is_dir(), t["id"]
        assert (example_dir / "board.yaml").is_file(), t["id"]


def test_every_testcase_yaml_exists():
    for t in _catalog()["templates"]:
        for tc in t["test"]["testcase_yaml"]:
            assert (REPO / tc).is_file(), tc


def test_every_portable_api_header_exists():
    for t in _catalog()["templates"]:
        for header in t["requires"]["portable_apis"]:
            assert (REPO / header).is_file(), header


def test_every_library_is_real():
    import alp_project_emit
    real = set(alp_project_emit._LIBRARY_KCONFIG.keys())
    for t in _catalog()["templates"]:
        for lib in t["requires"]["libraries"]:
            assert lib in real, (t["id"], lib)


def test_every_chip_has_a_manifest():
    for t in _catalog()["templates"]:
        for chip in t["requires"]["chips"]:
            assert (REPO / "metadata" / "chips" / f"{chip}.yaml").is_file(), chip


def test_dangling_example_path_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["example"] = "examples/peripheral-io/does-not-exist"
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "does not exist" in proc.stdout


def test_dangling_testcase_yaml_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["test"]["testcase_yaml"] = ["examples/nope/testcase.yaml"]
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "testcase_yaml" in proc.stdout
    assert "does not exist" in proc.stdout


def test_unreal_library_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["requires"]["libraries"] = ["totally_fake_library"]
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "not a real library" in proc.stdout


def test_unreal_chip_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["requires"]["chips"] = ["totally_fake_chip"]
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "has no metadata/chips" in proc.stdout


def test_dangling_portable_api_header_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["requires"]["portable_apis"] = ["include/alp/does_not_exist.h"]
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "does not exist" in proc.stdout


def test_stable_record_on_issue_448_example_rejected(tmp_path):
    """#448: iot-connected-camera is documented as v1.0-ready but its own
    runtime/testcase.yaml still describe a v0.1/v0.2 skeleton -- a `stable`
    template may never point at it."""
    doc = copy.deepcopy(_catalog())
    t = doc["templates"][0]
    t["example"] = "examples/connectivity/iot-connected-camera"
    t["test"]["testcase_yaml"] = [
        "examples/connectivity/iot-connected-camera/testcase.yaml"]
    t["status"] = "stable"
    t.pop("note", None)
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "#448" in proc.stdout
    assert "tracked content-quality gap" in proc.stdout


def test_stable_record_on_issue_520_example_rejected(tmp_path):
    """#520: iot-dashboard bypasses <alp/display.h>/<alp/gui.h> for a direct
    zephyr/drivers/display.h include -- a `stable` template may never point
    at it (nor the five display/LVGL examples #520 also names)."""
    doc = copy.deepcopy(_catalog())
    t = doc["templates"][0]
    t["example"] = "examples/connectivity/iot-dashboard"
    t["test"]["testcase_yaml"] = [
        "examples/connectivity/iot-dashboard/testcase.yaml"]
    t["status"] = "stable"
    t.pop("note", None)
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "#520" in proc.stdout


def test_known_gap_example_allowed_as_preview(tmp_path):
    """The same known-gap example is fine when the record is honestly
    marked `preview` with a note -- the gate only blocks `stable`."""
    doc = copy.deepcopy(_catalog())
    t = doc["templates"][0]
    t["example"] = "examples/connectivity/iot-connected-camera"
    t["test"]["testcase_yaml"] = [
        "examples/connectivity/iot-connected-camera/testcase.yaml"]
    t["status"] = "preview"
    t["note"] = "Tracked gap #448 -- example still behaves as a skeleton."
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_preview_without_note_rejected_by_schema(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["status"] = "preview"
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0


def test_duplicate_id_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    dupe = copy.deepcopy(doc["templates"][0])
    # Perturb a non-id field so this isn't ALSO a fully-duplicate array
    # element (which the schema's own `uniqueItems` would catch first,
    # under a different message) -- this test targets the id-specific
    # semantic check.
    dupe["title"] = dupe["title"] + " (dupe)"
    doc["templates"].append(dupe)
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "duplicate template id" in proc.stdout


def test_missing_archetype_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"] = [t for t in doc["templates"] if t["archetype"] != "gateway"]
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "gateway" in proc.stdout


def test_unknown_top_level_key_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["bogus_key"] = 1
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0


def test_bad_archetype_enum_rejected(tmp_path):
    doc = copy.deepcopy(_catalog())
    doc["templates"][0]["archetype"] = "galaxy"
    p = _write(tmp_path, doc)
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0


def test_malformed_json_rejected(tmp_path):
    p = tmp_path / "catalog.json"
    p.write_text("not json", encoding="utf-8")
    proc = _run("--catalog", str(p))
    assert proc.returncode != 0
    assert "parse error" in proc.stdout

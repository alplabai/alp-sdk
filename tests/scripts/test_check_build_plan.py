# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_build_plan.py + the build-plan v1 contract.

The plan is the machine-readable projection of board.yaml that the `alp`
CLI / alp-sdk-vscode 'Wave C' consumer reads (see #610). These lock the
committed build-plan snapshot corpus <-> contract lockstep and the gate
behaviour (the gate no longer generates a fresh plan -- see
scripts/check_build_plan.py's docstring for what that demotion costs).
"""
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_build_plan.py"
SCHEMA = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def test_schema_is_valid_draft202012():
    import jsonschema
    jsonschema.Draft202012Validator.check_schema(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def test_default_corpus_conforms():
    # the committed build-plan snapshots under tests/fixtures/emit-snapshots/
    # still match the documented contract (corpus <-> schema drift detection,
    # now that the gate no longer generates a fresh plan to check).
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 failure(s)" in proc.stdout


def test_empty_corpus_rejected(tmp_path):
    # a corpus dir with no *.build-plan.snap files must fail, not pass --
    # the exact silent-erosion failure this gate exists to catch.
    proc = _run("--snapshot-dir", str(tmp_path))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_valid_plan_file_passes(tmp_path):
    # build the fixture from a committed snapshot instead of invoking the
    # doomed orchestrator emitter directly.
    snapshot = REPO / "tests/fixtures/emit-snapshots/rpmsg-v2n.build-plan.snap"
    p = tmp_path / "build-plan.json"
    p.write_text(snapshot.read_text(encoding="utf-8"), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_plan_missing_required_key_rejected(tmp_path):
    bad = {
        "schemaVersion": 1,
        "generatedBy":   "test",
        "boardYaml":     "board.yaml",
        "sku":           "E1M-V2N101",
        "buildRoot":     "build",
        # slice missing the required "env" key
        "slices": [{
            "coreId": "m33_sm", "backend": "zephyr",
            "buildDir": "build/m33_sm-zephyr", "appDir": None,
            "configArtefacts": [], "command": None,
        }],
        "sharedArtefacts": [], "warnings": [],
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout
    assert "env" in proc.stdout


def test_plan_unknown_top_level_key_rejected(tmp_path):
    bad = {
        "schemaVersion": 1, "generatedBy": "test", "boardYaml": "board.yaml",
        "sku": "E1M-V2N101", "buildRoot": "build",
        "slices": [], "sharedArtefacts": [], "warnings": [],
        "bogusKey": 1,  # additionalProperties:false must catch drift/typos
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_plan_wrong_schema_version_rejected(tmp_path):
    bad = {
        "schemaVersion": 2,  # locked const -- any other value must fail
        "generatedBy": "test", "boardYaml": "board.yaml",
        "sku": "E1M-V2N101", "buildRoot": "build",
        "slices": [], "sharedArtefacts": [], "warnings": [],
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout

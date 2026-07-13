# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
BOARD_SCHEMA = REPO / "metadata/schemas/board.schema.json"


def test_schema_allows_explicit_schema_version():
    schema = json.loads(BOARD_SCHEMA.read_text(encoding="utf-8"))
    assert schema["properties"]["schemaVersion"]["type"] == "integer"
    assert schema["properties"]["schemaVersion"]["minimum"] == 1
    jsonschema.Draft202012Validator.check_schema(schema)


def test_stamped_board_validates_against_schema():
    schema = json.loads(BOARD_SCHEMA.read_text(encoding="utf-8"))
    doc = {"schemaVersion": 1, "som": {"sku": "E1M-AEN801"},
           "cores": {"m55_hp": {"app": "./src"}}}
    jsonschema.Draft202012Validator(schema).validate(doc)


import sys
sys.path.insert(0, str(REPO / "scripts"))
import check_board_schema_version as gate  # noqa: E402


def test_gate_flags_unstamped_below_latest(tmp_path):
    # With the real v1->v2 migration registered, LATEST is 2, so an unstamped
    # (== v1) board.yaml is below LATEST and IS drift (needs migration).
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("som:\n  sku: X\ncores:\n  m55_hp:\n    app: ./src\n")
    assert gate.find_drift(tmp_path) == [b / "board.yaml"]


def test_gate_clean_on_explicit_latest(tmp_path):
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text(
        f"schemaVersion: {gate.alp_migrate.LATEST}\nsom:\n  sku: X\n")
    assert gate.find_drift(tmp_path) == []


def test_gate_delegates_to_plan(tmp_path, monkeypatch):
    # With a real migration registered (synthetic v1->v2), an at-v1 file
    # becomes drift -- proves the gate flags via plan(), not a hardcoded rule.
    monkeypatch.setattr(gate.alp_migrate, "STEPS", [(1, 2, lambda l, r: None)])
    monkeypatch.setattr(gate.alp_migrate, "LATEST", 2)
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("som:\n  sku: X\n")  # v1, below new LATEST
    assert gate.find_drift(tmp_path) == [b / "board.yaml"]

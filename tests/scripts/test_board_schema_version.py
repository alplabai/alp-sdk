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


def test_gate_flags_unstamped(tmp_path):
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("som:\n  sku: X\ncores:\n  m55_hp:\n    app: ./src\n")
    assert gate.find_drift(tmp_path) == [b / "board.yaml"]


def test_gate_passes_stamped(tmp_path):
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("schemaVersion: 1\nsom:\n  sku: X\n")
    assert gate.find_drift(tmp_path) == []

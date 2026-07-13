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

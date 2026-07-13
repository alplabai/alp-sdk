import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/alp-lock-v1.schema.json"

def test_schema_is_draft2020_closed():
    s = json.loads(SCHEMA.read_text())
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["lockVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)

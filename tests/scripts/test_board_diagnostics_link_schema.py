# tests/scripts/test_board_diagnostics_link_schema.py
"""board.yaml `diagnostics.link:` schema validation (alplabai/tan-cli#1350)."""
import json
from pathlib import Path

import jsonschema
import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMA = json.loads((_ROOT / "metadata/schemas/board.schema.json").read_text(encoding="utf-8"))
_DIAG = _SCHEMA["properties"]["diagnostics"]


@pytest.mark.parametrize("value", ["auto", "itcm"])
def test_link_accepts_known_targets(value):
    jsonschema.validate({"link": value, "console": "ram"}, _DIAG)


@pytest.mark.parametrize("value", ["mram", "hp", "ITCM", True, 0])
def test_link_rejects_unknown_targets(value):
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate({"link": value}, _DIAG)


def test_link_defaults_to_auto():
    assert _DIAG["properties"]["link"]["default"] == "auto"

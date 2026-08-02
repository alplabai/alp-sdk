# tests/scripts/test_som_sku_pattern.py
"""The SoM SKU regex is duplicated (by design -- JSON Schema has no $ref-able
cross-file pattern reuse here) across two schema files: `som-preset-v1.schema.json`
(the preset's own `sku`) and `board.schema.json` (a customer's `som.sku`). Pin
them equal so a one-sided edit (widening one, forgetting the other) fails loudly
instead of silently reintroducing the drift fixed in #1089.

`soc-spec-v1.schema.json` has its own `ref` pattern -- a `vendor:family:part`
triple, an unrelated shape -- and must NOT be pinned here.

`metadata/schemas/som-release-bundle-v1.schema.json:sku` is a *third*, looser
copy (`^E1M-(AEN|V2N|V2M|NX9)\\d{3}$`, no AEN tier-3-8 restriction). It is
deliberately not pinned equal to the two above: it constrains a different
artifact -- the SoM release-bundle manifest `scripts/provision_som.py` consumes
(public, documented at `docs/provisioning.md`, gated by
`scripts/check_som_bundle.py`) -- rather than a board/preset SKU field, so it is
not the same constraint and not in scope for the #1089 fix. It also carries its
own `family` enum, which the two pinned schemas do not.
"""
import json
import re
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMAS = _ROOT / "metadata" / "schemas"

_SOM_PRESET_SCHEMA = json.loads((_SCHEMAS / "som-preset-v1.schema.json").read_text(encoding="utf-8"))
_BOARD_SCHEMA = json.loads((_SCHEMAS / "board.schema.json").read_text(encoding="utf-8"))

_PRESET_SKU_PATTERN = _SOM_PRESET_SCHEMA["properties"]["sku"]["pattern"]
_BOARD_SKU_PATTERN = _BOARD_SCHEMA["properties"]["som"]["properties"]["sku"]["pattern"]


def test_som_sku_pattern_matches_across_schemas():
    """som-preset-v1.schema.json:sku and board.schema.json:som.sku must be
    byte-identical -- these are two copies of the same constraint, not two
    independent constraints, and drifting apart is exactly the #1089 bug."""
    assert _PRESET_SKU_PATTERN == _BOARD_SKU_PATTERN


@pytest.mark.parametrize("sku", [
    "E1M-AEN301", "E1M-AEN801",  # already-shipped, must keep matching
    "E1M-AEN302", "E1M-AEN709",  # newly allocated per-configuration tails (issue #1089)
    "E1M-V2N103", "E1M-V2M999",
    "E1M-NX9101",
])
def test_widened_pattern_admits_new_and_existing_skus(sku):
    assert re.match(_PRESET_SKU_PATTERN, sku)


@pytest.mark.parametrize("sku", [
    "E1M-AEN201",   # tier digit 2 is out of the 3-8 silicon-tier range
    "E1M-AEN3012",  # too many digits in the config tail
    "E1M-XYZ301",   # not a recognised family
    "E1M-V2N1",     # tail too short
])
def test_widened_pattern_still_rejects_invalid_skus(sku):
    assert not re.match(_PRESET_SKU_PATTERN, sku)

# SPDX-License-Identifier: Apache-2.0
"""chip-v1.schema.json types rails[]/channels[]/register_table[] items (#2347):
every committed manifest validates, and a seeded violation in each is rejected."""

from __future__ import annotations

import copy
import json
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
VALIDATOR = jsonschema.Draft202012Validator(
    json.loads((REPO / "metadata" / "schemas" / "chip-v1.schema.json").read_text(encoding="utf-8")))


def _load(chip):
    return yaml.safe_load((REPO / "metadata" / "chips" / f"{chip}.yaml").read_text(encoding="utf-8"))


def test_every_manifest_validates():
    for f in sorted((REPO / "metadata" / "chips").glob("*.yaml")):
        assert not list(VALIDATOR.iter_errors(yaml.safe_load(f.read_text(encoding="utf-8")))), f.name


@pytest.mark.parametrize("chip,block,mutate", [
    ("act8760", "register_table", lambda d: d["register_table"][0].__setitem__("write", "maybe")),
    ("act8760", "rails", lambda d: d["rails"][0].pop("vset0_reg")),
    ("da9292", "channels", lambda d: d["channels"][0].__setitem__("bogus_key", 1)),
])
def test_seeded_item_violation_is_rejected(chip, block, mutate):
    doc = copy.deepcopy(_load(chip))
    mutate(doc)
    assert any(list(e.path)[:1] == [block] for e in VALIDATOR.iter_errors(doc))

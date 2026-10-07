# SPDX-License-Identifier: Apache-2.0
"""som-preset v2 (#2024): `write_authority` is schema-required on every
`memory_map:` row, `inference.npu_population` is gone, v1 is rejected, and
every region the loader DERIVES states its authority explicitly."""
from __future__ import annotations

import copy
import importlib.util
import json
import sys
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata" / "schemas" / "som-preset-v2.schema.json"
PRESET = REPO / "metadata" / "e1m_modules" / "E1M-AEN801.yaml"
METADATA = REPO / "metadata"


@pytest.fixture(scope="module")
def validator():
    return jsonschema.Draft202012Validator(json.loads(SCHEMA.read_text(encoding="utf-8")))


@pytest.fixture()
def preset():
    return yaml.safe_load(PRESET.read_text(encoding="utf-8"))


def test_real_preset_valid(validator, preset):
    assert list(validator.iter_errors(preset)) == []


def test_region_without_write_authority_rejected(validator, preset):
    doc = copy.deepcopy(preset)
    del doc["memory_map"][0]["write_authority"]
    msgs = [e.message for e in validator.iter_errors(doc)]
    assert any("write_authority" in m for m in msgs), msgs


def test_npu_population_rejected(validator, preset):
    doc = copy.deepcopy(preset)
    doc["inference"]["npu_population"] = [{"variant": "u85"}]
    msgs = [e.message for e in validator.iter_errors(doc)]
    assert any("npu_population" in m for m in msgs), msgs


def test_v1_schema_version_rejected(validator, preset):
    doc = copy.deepcopy(preset)
    doc["schema_version"] = 1
    assert list(validator.iter_errors(doc)) != []


@pytest.fixture(scope="module")
def alp_project():
    path = REPO / "scripts" / "alp_project.py"
    spec = importlib.util.spec_from_file_location("alp_project", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["alp_project"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.mark.parametrize("preset_name", ["E1M-AEN301", "E1M-V2N101"])
def test_derived_regions_carry_write_authority(alp_project, preset_name):
    doc = yaml.safe_load((METADATA / "e1m_modules" / f"{preset_name}.yaml")
                         .read_text(encoding="utf-8"))
    doc.pop("memory_map", None)  # force the derived path
    regions = alp_project.resolve_memory_map(doc, METADATA)
    assert regions
    assert all("write_authority" in r for r in regions), regions

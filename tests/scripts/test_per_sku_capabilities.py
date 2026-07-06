# SPDX-License-Identifier: Apache-2.0
"""Per-SKU capability granularity (`silicon_capabilities.unpopulated`).

Covers the three layers of the restriction mechanism:

  1. Schema — som-preset-v1 accepts a well-formed restriction block on a
     real preset and rejects malformed shapes (empty list, wrong types,
     unknown sibling keys, duplicate entries, bad name pattern).
  2. Semantic validation — scripts/validate_metadata.py's cross-check:
     every listed name must be truthy in the referenced SoC JSON's
     `capabilities:` block (restriction can only REMOVE what the silicon
     offers) and must not collide with the preset's additive
     `capabilities:` block.
  3. Generator — scripts/gen_soc_caps.py emits an ALP_SOM_<SKU>-gated
     `#undef/#define 0` override block for restricted SKUs, and emits
     byte-identical output for the current (unrestricted) catalogue —
     the zero-behaviour-change pin.

resolve_capabilities() behaviour lives in test_resolve_capabilities.py.
"""
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
SOM_SCHEMA_PATH = REPO / "metadata" / "schemas" / "som-preset-v1.schema.json"
REAL_PRESET = REPO / "metadata" / "e1m_modules" / "E1M-AEN801.yaml"
GEN_SCRIPT = REPO / "scripts" / "gen_soc_caps.py"
VALIDATE_SCRIPT = REPO / "scripts" / "validate_metadata.py"
SOC_CAPS_HEADER = REPO / "include" / "alp" / "soc_caps.h"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def som_validator() -> jsonschema.Draft202012Validator:
    schema = json.loads(SOM_SCHEMA_PATH.read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


@pytest.fixture(scope="module")
def real_preset() -> dict:
    return yaml.safe_load(REAL_PRESET.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# 1. Schema accept / reject
# ---------------------------------------------------------------------------

def test_schema_accepts_restriction_on_real_preset(som_validator, real_preset):
    """A well-formed restriction block on a real, schema-valid preset passes."""
    doc = copy.deepcopy(real_preset)
    doc["silicon_capabilities"] = {"unpopulated": ["gpu2d", "dave2d"]}
    assert list(som_validator.iter_errors(doc)) == []


def test_schema_accepts_preset_without_restriction(som_validator, real_preset):
    """The field is optional — every current preset (no field) stays valid."""
    assert list(som_validator.iter_errors(real_preset)) == []


@pytest.mark.parametrize("bad_block", [
    {},                                        # missing required `unpopulated`
    {"unpopulated": []},                       # empty list (minItems 1)
    {"unpopulated": "gpu2d"},                  # not a list
    {"unpopulated": [True]},                   # non-string entry
    {"unpopulated": ["GPU2D"]},                # violates ^[a-z][a-z0-9_]*$
    {"unpopulated": ["gpu2d", "gpu2d"]},       # duplicates (uniqueItems)
    {"unpopulated": ["gpu2d"], "extra": 1},    # unknown sibling key
    {"populated": ["gpu2d"]},                  # wrong key entirely
])
def test_schema_rejects_malformed_restriction(som_validator, real_preset, bad_block):
    doc = copy.deepcopy(real_preset)
    doc["silicon_capabilities"] = bad_block
    assert list(som_validator.iter_errors(doc)), (
        f"schema accepted malformed silicon_capabilities: {bad_block!r}")


# ---------------------------------------------------------------------------
# 2. Semantic cross-check (validate_metadata.py)
# ---------------------------------------------------------------------------

@pytest.fixture()
def vm(tmp_path, monkeypatch):
    """validate_metadata module repointed at a synthetic metadata root."""
    mod = _load_module("validate_metadata_under_test", VALIDATE_SCRIPT)
    monkeypatch.setattr(mod, "REPO", tmp_path)
    monkeypatch.setattr(mod, "SOCS", tmp_path / "metadata" / "socs")
    return mod


def _write_soc(tmp_path: Path, capabilities: dict) -> None:
    soc_dir = tmp_path / "metadata" / "socs" / "testvendor" / "testfam"
    soc_dir.mkdir(parents=True, exist_ok=True)
    (soc_dir / "testpart.json").write_text(json.dumps({
        "ref": "testvendor:testfam:testpart",
        "capabilities": capabilities,
    }), encoding="utf-8")


def _write_preset(tmp_path: Path, body: dict) -> Path:
    som_dir = tmp_path / "metadata" / "e1m_modules"
    som_dir.mkdir(parents=True, exist_ok=True)
    path = som_dir / "E1M-TEST.yaml"
    path.write_text(yaml.safe_dump(body), encoding="utf-8")
    return path


def test_semantic_accepts_cap_the_silicon_offers(vm, tmp_path):
    _write_soc(tmp_path, {"gpu2d": True, "ethos_u55_count": 2})
    preset = _write_preset(tmp_path, {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        "silicon_capabilities": {"unpopulated": ["gpu2d", "ethos_u55_count"]},
    })
    assert vm._check_silicon_capability_restrictions([preset]) == []


def test_semantic_rejects_cap_the_silicon_lacks(vm, tmp_path):
    """A name absent from (or falsy in) the SoC capabilities block fails —
    a SKU can only remove what the silicon offers."""
    _write_soc(tmp_path, {"gpu2d": True, "cau": False})
    for bad in ("not_a_cap", "cau"):
        preset = _write_preset(tmp_path, {
            "sku": "E1M-TEST",
            "silicon": "testvendor:testfam:testpart",
            "silicon_capabilities": {"unpopulated": [bad]},
        })
        failures = vm._check_silicon_capability_restrictions([preset])
        assert failures, f"accepted `{bad}` the silicon does not offer"
        assert bad in failures[0][1][0]


def test_semantic_rejects_overlap_with_additive_capabilities(vm, tmp_path):
    """A name in both `capabilities:` and `unpopulated:` is ambiguous."""
    _write_soc(tmp_path, {"gpu2d": True})
    preset = _write_preset(tmp_path, {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        "capabilities": {"gpu2d": True},
        "silicon_capabilities": {"unpopulated": ["gpu2d"]},
    })
    failures = vm._check_silicon_capability_restrictions([preset])
    assert failures
    assert "never both" in " ".join(failures[0][1])


def test_semantic_rejects_unresolvable_silicon_ref(vm, tmp_path):
    """A restriction on a preset whose silicon ref has no SoC JSON fails
    loudly instead of silently skipping the cross-check."""
    preset = _write_preset(tmp_path, {
        "sku": "E1M-TEST",
        "silicon": "ghost:vendor:part",
        "silicon_capabilities": {"unpopulated": ["gpu2d"]},
    })
    failures = vm._check_silicon_capability_restrictions([preset])
    assert failures
    assert "does not resolve" in failures[0][1][0]


def test_semantic_skips_presets_without_the_field(vm, tmp_path):
    _write_soc(tmp_path, {"gpu2d": True})
    preset = _write_preset(tmp_path, {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
    })
    assert vm._check_silicon_capability_restrictions([preset]) == []


# ---------------------------------------------------------------------------
# 3. Generator (gen_soc_caps.py)
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def gen():
    return _load_module("gen_soc_caps_under_test", GEN_SCRIPT)


def _write_restricted_som(tmp_path: Path, unpopulated: list[str]) -> Path:
    som_dir = tmp_path / "e1m_modules"
    som_dir.mkdir(parents=True, exist_ok=True)
    (som_dir / "E1M-AEN801.yaml").write_text(yaml.safe_dump({
        "sku": "E1M-AEN801",
        "silicon": "alif:ensemble:e8",
        "silicon_capabilities": {"unpopulated": unpopulated},
    }), encoding="utf-8")
    return som_dir


def test_restricted_sku_drops_alp_soc_flags(gen, tmp_path):
    """A restricted SKU gets an ALP_SOM_<SKU>-gated block forcing the
    matching ALP_SOC_* macros to 0, so ALP_HAS(GPU2D)/ALP_HAS(DAVE2D)
    resolve false for that SKU's build."""
    som_dir = _write_restricted_som(tmp_path, ["gpu2d", "dave2d"])
    text = gen.emit(som_dir=som_dir)

    block = text.split("#if defined(ALP_SOM_E1M_AEN801)", 1)
    assert len(block) == 2, "missing per-SKU restriction gate"
    body = block[1].split("#endif", 1)[0]
    assert "#undef ALP_SOC_GPU2D" in body
    assert "#define ALP_SOC_GPU2D 0" in body
    assert "#undef ALP_SOC_DAVE2D" in body
    assert "#define ALP_SOC_DAVE2D 0" in body
    # The ALP_CAP_* layer must come AFTER the override so ALP_HAS()
    # resolves against the narrowed set.
    assert text.index("ALP_SOM_E1M_AEN801") < text.index("#define ALP_HAS(cap)")


def test_loader_only_caps_do_not_emit_macros(gen, tmp_path):
    """Count-style silicon caps with no ALP_SOC_* macro (ethos_u85_count)
    surface as a comment, never as an #undef."""
    som_dir = _write_restricted_som(tmp_path, ["ethos_u85_count"])
    text = gen.emit(som_dir=som_dir)
    assert "#undef ALP_SOC_ETHOS_U85_COUNT" not in text
    assert "ethos_u85_count" in text  # documented as loader-layer only


def test_unrestricted_catalogue_emits_byte_identical_header(gen, tmp_path):
    """Zero-behaviour-change pin: with no restricted SKU (an empty SoM dir
    AND the real catalogue) the emitted header is byte-identical, and it
    matches the committed include/alp/soc_caps.h content modulo the
    clang-format pass (asserted via the on-disk regen test below)."""
    empty = tmp_path / "empty"
    empty.mkdir()
    assert gen.emit(som_dir=empty) == gen.emit()  # real catalogue == none restricted


def test_regen_leaves_committed_header_unchanged():
    """Running the real generator against the repo must not change the
    committed header — existing SKUs regenerate byte-identical."""
    import subprocess
    before = SOC_CAPS_HEADER.read_text(encoding="utf-8")
    subprocess.run([sys.executable, str(GEN_SCRIPT)], check=True)
    after = SOC_CAPS_HEADER.read_text(encoding="utf-8")
    assert before == after

# SPDX-License-Identifier: Apache-2.0
"""`metadata/npu_ops/<family>/*.json` -- the per-NPU op-support data asset (ADR-0028).

Reshaped from a flat one-file-per-backend layout to one file per SUPPORT-
TABLE IDENTITY: a backend can publish more than one distinct table (Vela
alone publishes two TFLite tables -- Ethos-U85, and Ethos-U55/U65 -- both
invariant to `--accelerator-config`), so the shape is
`metadata/npu_ops/<backend_family>/<variant>@<toolchain>-<toolchain_version>.json`.
"""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_NPU_OPS = _ROOT / "metadata" / "npu_ops"

#: Every table on disk, discovered by GLOB -- a table added later (a new
#: variant, a new backend family) is picked up by every parametrized check
#: below with no parametrize list to hand-update.
_ALL_TABLES = sorted(_NPU_OPS.glob("*/*.json"))


def _load(path: Path) -> dict:
    return json.loads(path.read_text("utf-8"))


def _ids(path: Path) -> str:
    return path.relative_to(_NPU_OPS).as_posix()


# ---------------------------------------------------------------------------
# Directory-wide checks. Parametrized over the glob above, not a hard-coded
# per-backend list -- this is the "works for a table added later" property.
# ---------------------------------------------------------------------------

def test_at_least_one_table_exists():
    assert _ALL_TABLES, "metadata/npu_ops/ has no <family>/<variant>@... tables at all"


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_every_table_carries_the_required_fields(path):
    data = _load(path)
    for key in ("applies_to", "op_namespace", "authority", "stance",
                "provenance", "supported_ops"):
        assert key in data, f"{path}: missing {key!r}"
    assert data["stance"] == "screening"
    assert data["authority"] in ("tool-generated", "vendor-manual")
    ops = data["supported_ops"]
    assert isinstance(ops, list) and ops
    assert len(ops) == len(set(ops)), f"{path}: duplicate entries in supported_ops"


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_filename_matches_applies_to_identity(path):
    """A table's PATH is a claim -- `<variant>@<toolchain>-<toolchain_version>
    .json` -- that its own `applies_to` body must reproduce exactly. A future
    consumer resolving a table by path alone (rather than reading every
    file's body) depends on this holding for every table, in every family
    directory, forever -- hence a glob-parametrized check rather than one
    written against today's three files."""
    data = _load(path)
    at = data["applies_to"]
    expected_stem = f"{at['variant']}@{at['toolchain']}-{at['toolchain_version']}"
    assert path.stem == expected_stem, (
        f"{path.name} declares applies_to implying `{expected_stem}.json`")


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_op_spelling_matches_declared_namespace(path):
    """TFLite builtins are UPPER_SNAKE (`CONV_2D`); ONNX operators are
    CamelCase and are NOT upper-snake (`Conv`). This is the assertion that
    would have caught the pre-reshape drpai/deepx_dxm1 seed files being
    spelled in the wrong vocabulary entirely."""
    data = _load(path)
    ns = data["op_namespace"]
    ops = data["supported_ops"]
    if ns == "tflite":
        assert all(op == op.upper() for op in ops), path
    elif ns == "onnx":
        assert not any(op == op.upper() for op in ops), path
    else:
        pytest.fail(f"{path}: unrecognised op_namespace {ns!r}")


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_generated_banner_matches_authority(path):
    """A `tool-generated` table is machine-reproducible and self-identifies
    with a DO-NOT-EDIT banner; a `vendor-manual` table is intentionally
    hand-transcribed (there is no tool to regenerate it from) and must NOT
    carry a banner that falsely promises one."""
    data = _load(path)
    has_banner = isinstance(data.get("_generated"), str)
    if data["authority"] == "tool-generated":
        assert has_banner, f"{path}: tool-generated but no _generated banner"
    else:
        assert not has_banner, f"{path}: vendor-manual but carries a _generated banner"


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_count_expected_self_check_when_present(path):
    data = _load(path)
    count_expected = data["provenance"].get("count_expected")
    if count_expected is not None:
        assert len(data["supported_ops"]) == count_expected, (
            f"{path}: provenance.count_expected={count_expected} but "
            f"supported_ops has {len(data['supported_ops'])} entries")


# ---------------------------------------------------------------------------
# Ethos-U85 vs Ethos-U55/U65 -- the concrete invariant this whole reshape was
# done to ground. This is a fact about these two SPECIFIC tables' relationship
# (Vela publishes exactly two TFLite tables), not a rule every table obeys,
# so it is written against the two files directly rather than the glob.
# ---------------------------------------------------------------------------

_U85 = _NPU_OPS / "ethos_u" / "u85@vela-5.1.0.json"
_U55_U65 = _NPU_OPS / "ethos_u" / "u55-u65@vela-5.1.0.json"

#: The exact Ethos-U85-only delta over Ethos-U55/U65 (17 ops). Reproduced
#: independently of scripts/gen_npu_ops.py's own pinned constant of the same
#: name, so a bug shared between the generator and this test can't both agree
#: on a wrong answer.
_EXPECTED_U85_ONLY_DELTA = {
    "CAST", "DIV", "EQUAL", "GATHER", "GREATER", "GREATER_EQUAL", "LESS",
    "LESS_EQUAL", "LOGICAL_AND", "LOGICAL_NOT", "LOGICAL_OR", "NOT_EQUAL",
    "REDUCE_ALL", "REDUCE_ANY", "SCATTER_ND", "SELECT", "SELECT_V2",
}


def test_u85_and_u55_u65_tables_exist():
    assert _U85.is_file(), "expected metadata/npu_ops/ethos_u/u85@vela-5.1.0.json"
    assert _U55_U65.is_file(), "expected metadata/npu_ops/ethos_u/u55-u65@vela-5.1.0.json"


def test_u55_u65_is_a_strict_subset_of_u85_with_the_known_17_op_delta():
    u85 = set(_load(_U85)["supported_ops"])
    u55_u65 = set(_load(_U55_U65)["supported_ops"])
    assert u55_u65 <= u85, (
        f"ops present in U55/U65 but not U85 (should be impossible): "
        f"{sorted(u55_u65 - u85)}")
    delta = u85 - u55_u65
    assert delta == _EXPECTED_U85_ONLY_DELTA, (
        f"Ethos-U85-only delta changed -- expected "
        f"{sorted(_EXPECTED_U85_ONLY_DELTA)}, got {sorted(delta)}")
    assert len(delta) == 17


def test_u85_and_u55_u65_share_the_same_vela_report_content_hash():
    """Both tables were parsed out of the SAME `vela --supported-ops-report`
    run -- their content_hash must agree, or they were regenerated from two
    different reports and are no longer a matched pair."""
    u85 = _load(_U85)
    u55_u65 = _load(_U55_U65)
    assert (u85["provenance"]["content_hash"]
            == u55_u65["provenance"]["content_hash"])


# ---------------------------------------------------------------------------
# DEEPX: absence is the deliberate signal, not a gap left to fill in later.
# ---------------------------------------------------------------------------

def test_no_deepx_table_exists():
    """dxcom 2.3.0 publishes no op-support table. The only evidence available
    is 15 ONNX ops observed present in a yolo11n.onnx it demonstrably
    compiled -- a LOWER BOUND on support, not a support table, and not safe to
    ship as one: doing so would mass-produce false "no-fit" verdicts on V2M,
    the SKU where DEEPX is the headline feature.

    A consuming static-fit engine (tan's model-fit analyzer, per ADR-0028)
    MUST treat a backend whose directory doesn't exist here as
    `undetermined`, never as "zero ops known -> cpu-fallback". This test
    exists so that invariant can never be silently repaired by someone
    "helpfully" adding a deepx table back."""
    assert not (_NPU_OPS / "deepx").exists()
    assert not (_NPU_OPS / "deepx_dxm1").exists()
    assert not list(_NPU_OPS.glob("deepx*"))


def test_no_cpu_op_support_file():
    """`cpu` is the universal fallback -- it supports everything and is never
    looked up, so a `cpu.json` / `cpu/` table would be dead data that could
    drift."""
    assert not (_NPU_OPS / "cpu.json").exists()
    assert not (_NPU_OPS / "cpu").exists()


# ---------------------------------------------------------------------------
# The compute-dominant ops any static-fit estimator will score first, per
# vocabulary -- carried over from the pre-reshape flat-file tests so the
# reshape can't silently lose CONV_2D/Conv-class coverage.
# ---------------------------------------------------------------------------

_REQUIRED_OPS = {
    "tflite": {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"},
    "onnx": {"Conv", "Gemm"},
}


@pytest.mark.parametrize("path", _ALL_TABLES, ids=_ids)
def test_compute_dominant_ops_present(path):
    data = _load(path)
    required = _REQUIRED_OPS.get(data["op_namespace"])
    if required is not None:
        assert required <= set(data["supported_ops"]), path

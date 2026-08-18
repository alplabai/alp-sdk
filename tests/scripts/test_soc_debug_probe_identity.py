# SPDX-License-Identifier: Apache-2.0
"""
`validate_metadata._check_soc_debug_probe_identity` -- the semantic gate
that makes `variants[].debug.jlink_device` keys real `cores[].id` values
(#987).  JSON Schema can express that `jlink_device` is an object of
string values but not that its *keys* name a real core on *this* SoC, so
this gate does that cross-reference.

Run locally:

    python -m pytest tests/scripts/test_soc_debug_probe_identity.py -v
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import validate_metadata as V  # noqa: E402


def _run(tmp_path, monkeypatch, doc) -> int:
    """Run the gate on one synthetic SoC doc; return the failure count."""
    monkeypatch.setattr(V, "REPO", tmp_path)  # so relative_to(REPO) resolves
    p = tmp_path / "soc.json"
    p.write_text(json.dumps(doc))
    return len(V._check_soc_debug_probe_identity([p]))


_E8_LIKE = {
    "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
    "variants": [
        {
            "order_code": "AE822FA0E5597LS0",
            "debug": {
                "pyocd_target": "AE822FA0E5597LS0",
                "jlink_device": {
                    "m55_hp": "Cortex-M55",
                    "m55_he": "Cortex-M55",
                },
                "jlink_flash_device": "AE822FA0E5597LS0_M55_HE",
            },
        }
    ],
}


def test_valid_jlink_device_keys_pass(tmp_path, monkeypatch):
    assert _run(tmp_path, monkeypatch, copy.deepcopy(_E8_LIKE)) == 0


def test_jlink_device_key_referencing_unknown_core_fails(tmp_path, monkeypatch):
    doc = copy.deepcopy(_E8_LIKE)
    doc["variants"][0]["debug"]["jlink_device"]["m55_nope"] = "Cortex-M55"
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_variant_with_no_debug_block_passes(tmp_path, monkeypatch):
    # Absence is a valid, publishable state -- the cardinal rule of #987.
    doc = {"cores": [{"id": "m55_hp"}], "variants": [{"order_code": "AE302F80F55D5LE"}]}
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_pyocd_target_only_variant_passes(tmp_path, monkeypatch):
    # E3/E5/E7: pyocd_target with no jlink_device at all -- nothing to cross-ref.
    doc = {
        "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
        "variants": [{"order_code": "AE512F80F55D5LS", "debug": {"pyocd_target": "AE512F80F55D5LS"}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_real_alif_socs_pass_the_gate():
    # The shipped e3-e8 SoC specs must satisfy the gate as-is.
    socs = sorted((V.SOCS / "alif" / "ensemble").glob("e*.json"))
    assert socs, "no Alif ensemble SoC specs found"
    assert V._check_soc_debug_probe_identity(socs) == []


def test_non_object_variants_and_cores_entries_do_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants[]`/`cores[]` entries are schema-typed objects, but the
    schema pass that would reject a non-object entry runs separately and is
    not guaranteed to have run first. A bare string in either list used to
    reach a bare `.get()` and raise `AttributeError` here, hiding the schema
    FAIL line that already explains the real problem. Filtered to dicts,
    this doc must return cleanly -- no real variant survives the filter, so
    there is nothing left to cross-reference.

    `variants` also carries ONE real dict entry (whose `jlink_device` names
    the one real `cores` dict entry) so the function does not short-circuit
    at `if not variants: continue` before ever reaching the `core_ids` /
    `m_core_ids` filters this test exists to exercise -- a prior version of
    this fixture (`variants: ["not-a-dict"]` only) never actually reached
    either line, so reverting either guard alone would not have reddened
    this test."""
    doc = {
        "cores": ["not-a-dict", {"id": "m55_hp", "type": "cortex-m55"}],
        "variants": ["not-a-dict", {"order_code": "AE999X",
                                     "debug": {"jlink_device": {"m55_hp": "Cortex-M55"}}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The SoC doc's top level is schema-typed as an object, but a
    malformed file could parse to a bare JSON array -- `doc.get("variants")`
    used to raise `AttributeError: 'list' object has no attribute 'get'`
    here, aborting the whole gate mid-run instead of leaving the schema
    FAIL line (which already flags the type mismatch) to explain the real
    problem."""
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = tmp_path / "soc.json"
    p.write_text(json.dumps([]))
    assert V._check_soc_debug_probe_identity([p]) == []  # must not raise


def test_non_list_variants_and_cores_do_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants`/`cores` are themselves schema-typed as arrays, but a
    malformed document can carry a non-list scalar there (e.g. the bare int
    `5`, which is truthy) -- iterating the unfiltered value used to raise
    `TypeError: 'int' object is not iterable`, aborting the whole gate
    mid-run instead of leaving the schema FAIL line (which already flags
    the type mismatch) to explain the real problem."""
    doc = {"cores": 5, "variants": 5}
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_object_debug_block_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants[].debug` is schema-typed as an object, but a malformed
    document can still carry a scalar there (e.g. a bare string), which is
    truthy and used to reach `debug.get(...)` and raise `AttributeError`.
    Normalised to `{}`, the variant is treated as carrying no debug facts."""
    doc = {
        "cores": [{"id": "m55_hp"}],
        "variants": [{"order_code": "AE999X", "debug": "not-a-dict"}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_string_core_id_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`cores[].id` is schema-typed as a string, but a malformed doc can
    carry a dict/list there -- both `core_ids` and (for Cortex-M rows)
    `m_core_ids` are built via unfiltered set comprehensions that used to
    raise `TypeError: unhashable type: 'dict'`. A real dict-typed `id`
    alongside a real string `id` (whose `jlink_device` entry the one real
    variant carries) proves the filter runs, not an early short-circuit."""
    doc = {
        "cores": [{"id": {"nested": "dict"}, "type": "cortex-m55"},
                  {"id": "m55_hp", "type": "cortex-m55"}],
        "variants": [{"order_code": "AE999X",
                      "debug": {"jlink_device": {"m55_hp": "Cortex-M55"}}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_object_jlink_device_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`debug.jlink_device` is schema-typed as an object, but
    `debug.get("jlink_device") or {}` does not protect a non-empty NON-STRING
    scalar (e.g. a bare integer, which is truthy) -- that used to reach
    `for core_id in jlink_device` and raise `TypeError: 'int' object is not
    iterable` (a string would merely iterate characters and not crash, which
    is why this needs its own scalar type). Normalised to `{}`, there is
    nothing to cross-reference."""
    doc = {
        "cores": [{"id": "m55_hp"}],
        "variants": [{"order_code": "AE999X", "debug": {"jlink_device": 5}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


# ---------------------------------------------------------------------
# `debug.expect_dpidr` must not be published half-armed (#1355)
#
# The DPIDR preflight is TWO facts -- the expected SW-DP IDR and the
# live-core attach profile the read is performed with -- and a downstream
# flasher (tan `flash_plan.py::validate_flow_d_preflight_args`) REFUSES a
# half-armed pair rather than skipping the check, so a variant that
# publishes one without the other does not leave the guard merely unarmed:
# it hard-fails every flash of that part, dry runs included.
#
# Fixtures below carry `cores[].type` because the rule is scoped to
# Cortex-M cores: `debug.jlink_device` is legitimately sparse across
# `cores[]` (E8 publishes m55_hp/m55_he and nothing for the a32_cluster,
# which boots Linux off storage rather than being J-Link flashed).
# ---------------------------------------------------------------------

_E8_LIKE_WITH_A32 = {
    "cores": [
        {"id": "a32_cluster", "type": "cortex-a32"},
        {"id": "m55_hp", "type": "cortex-m55"},
        {"id": "m55_he", "type": "cortex-m55"},
    ],
    "variants": [
        {
            "order_code": "AE822FA0E5597LS0",
            "debug": {
                "pyocd_target": "AE822FA0E5597LS0",
                "jlink_device": {
                    "m55_hp": "Cortex-M55",
                    "m55_he": "Cortex-M55",
                },
                "jlink_flash_device": "AE822FA0E5597LS0_M55_HE",
                "expect_dpidr": "0x4C013477",
            },
        }
    ],
}


def test_expect_dpidr_with_full_m_core_coverage_passes(tmp_path, monkeypatch):
    """The real E8 shape: expect_dpidr paired with an attach profile for
    both M55s, and NOTHING for the A32 cluster -- which must not be
    counted as a gap, or the gate would fail the one variant it exists to
    protect."""
    assert _run(tmp_path, monkeypatch, copy.deepcopy(_E8_LIKE_WITH_A32)) == 0


def test_expect_dpidr_without_jlink_device_for_an_m_core_fails(
    tmp_path, monkeypatch
):
    """Drop ONE M-core's attach profile: the pair is now half-armed for
    that core and the gate must say so."""
    doc = copy.deepcopy(_E8_LIKE_WITH_A32)
    del doc["variants"][0]["debug"]["jlink_device"]["m55_hp"]
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_expect_dpidr_with_no_jlink_device_at_all_fails(tmp_path, monkeypatch):
    doc = copy.deepcopy(_E8_LIKE_WITH_A32)
    del doc["variants"][0]["debug"]["jlink_device"]
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_jlink_device_without_expect_dpidr_passes(tmp_path, monkeypatch):
    """The CONVERSE is deliberately legal -- E1M-AEN701's variant today.

    An unmeasured DPIDR leaves the guard unarmed, which is recoverable; a
    guard armed at a guessed ID is not, because a wrong ID that happens to
    match another board on the same bench passes on exactly the board it
    exists to exclude. This gate must never push anyone toward inventing a
    value.

    The attach map here is deliberately INCOMPLETE (m55_he only) as well as
    unmeasured. A `jlink_device`-complete fixture would satisfy a SYMMETRIC
    rule too, so it could not tell the asymmetric rule from a symmetric one
    -- it would assert the converse is legal while deciding nothing about
    it. Measured: making the rule symmetric leaves a complete-map fixture
    green and only this one red.
    """
    doc = copy.deepcopy(_E8_LIKE_WITH_A32)
    del doc["variants"][0]["debug"]["expect_dpidr"]
    del doc["variants"][0]["debug"]["jlink_device"]["m55_hp"]
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_no_debug_probe_facts_at_all_passes(tmp_path, monkeypatch):
    """Neither key, and no attach map either -- the state of a variant
    nobody has put on a bench. Nothing to pair, nothing to refuse."""
    doc = copy.deepcopy(_E8_LIKE_WITH_A32)
    doc["variants"][0]["debug"] = {"pyocd_target": "AE822FA0E5597LS0"}
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_expect_dpidr_gate_names_the_uncovered_core(tmp_path, monkeypatch):
    """A count alone would pass even if the message pointed at the wrong
    core -- pin the diagnostic's content, not just its existence."""
    monkeypatch.setattr(V, "REPO", tmp_path)
    doc = copy.deepcopy(_E8_LIKE_WITH_A32)
    del doc["variants"][0]["debug"]["jlink_device"]["m55_he"]
    p = tmp_path / "soc.json"
    p.write_text(json.dumps(doc))
    failures = V._check_soc_debug_probe_identity([p])
    assert len(failures) == 1
    msgs = " ".join(failures[0][1])
    assert "m55_he" in msgs
    assert "expect_dpidr" in msgs
    # The A32 cluster has no attach profile either, and must NOT be blamed.
    assert "a32_cluster" not in msgs


def test_shipped_e8_publishes_the_measured_dpidr():
    """The value this issue exists to publish, pinned at its source.

    `0x4C013477` is a MEASUREMENT (re-confirmed on silicon 2026-08-10), not a
    derivation -- so it is asserted verbatim
    here, on the variant that carries the E1M-AEN801 SKU.
    """
    doc = json.loads(
        (V.SOCS / "alif" / "ensemble" / "e8.json").read_text(encoding="utf-8"))
    by_sku = {
        sku: v
        for v in doc["variants"]
        for sku in (v.get("alp_module_skus") or [])
    }
    variant = by_sku["E1M-AEN801"]
    assert variant["order_code"] == "AE822FA0E5597LS0"
    assert variant["debug"].get("expect_dpidr") == "0x4C013477"

    # Only the part actually on a bench may publish a measured DPIDR -- a
    # same-family assumption is exactly the guess that turns this guard into
    # a hazard. Asserted over the WHOLE file rather than one named sibling:
    # the sibling this used to name (AE822FA0E5597BS0) was a WLCSP part and
    # was dropped in #1444, which would have turned a `next()` lookup into a
    # StopIteration rather than a statement about the corpus.
    armed = [v["order_code"] for v in doc["variants"]
             if "expect_dpidr" in (v.get("debug") or {})]
    assert armed == ["AE822FA0E5597LS0"], armed


# ---------------------------------------------------------------------
# soc-spec-v1.schema.json's own shape rule for `debug.expect_dpidr`
#
# A DPIDR is a 32-bit register, so the published constant is exactly 8 hex
# digits behind an `0x`. That is not decoration: the value is interpolated
# into a J-Link Commander script line downstream, and a consumer's own
# address validator accepts any hex length -- so a truncated or
# whitespace-padded ID would sail through THERE and silently compare
# against the wrong number of digits. Pin it at the source.
# ---------------------------------------------------------------------


def _soc_validator():
    import jsonschema

    schema = json.loads(
        (Path(V.__file__).resolve().parents[1] / "metadata" / "schemas"
         / "soc-spec-v1.schema.json").read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def _validate_variant_debug(debug) -> list:
    """Validate a minimal soc-spec doc carrying `debug` and return the
    errors that name `expect_dpidr`."""
    doc = {
        "schema_version": 1,
        "vendor": "alif",
        "family": "ensemble",
        "cores": [{"id": "m55_hp", "type": "cortex-m55"}],
        "variants": [{"order_code": "AE822FA0E5597LS0", "debug": debug}],
    }
    return [
        e for e in _soc_validator().iter_errors(doc)
        if "expect_dpidr" in str(e.json_path) or "expect_dpidr" in e.message
    ]


@pytest.mark.parametrize("value", ["0x4C013477", "0x4c013477", "0xAABBCCDD"])
def test_schema_accepts_a_32_bit_dpidr(value):
    assert _validate_variant_debug({"expect_dpidr": value}) == []


@pytest.mark.parametrize("value", [
    "4C013477",      # no 0x prefix
    "0x4C01347",     # 7 digits -- truncated
    "0x4C0134777",   # 9 digits
    "0x4C013477 ",   # trailing whitespace
    "0xZZ013477",    # not hex
    "",
])
def test_schema_rejects_a_malformed_dpidr(value):
    assert _validate_variant_debug({"expect_dpidr": value}), \
        f"schema accepted a malformed expect_dpidr {value!r}"


def test_schema_rejects_an_unknown_debug_key():
    """`debug` is `additionalProperties: false`, so a typo'd
    `expect_dp_idr` is a validation error rather than a key that silently
    never arms anything."""
    errors = list(_soc_validator().iter_errors({
        "schema_version": 1,
        "vendor": "alif",
        "family": "ensemble",
        "cores": [{"id": "m55_hp", "type": "cortex-m55"}],
        "variants": [{
            "order_code": "AE822FA0E5597LS0",
            "debug": {"expect_dp_idr": "0x4C013477"},
        }],
    }))
    assert any("expect_dp_idr" in e.message for e in errors)

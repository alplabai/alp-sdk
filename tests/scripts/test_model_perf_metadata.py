# SPDX-License-Identifier: Apache-2.0
"""`validate_metadata._check_model_perf_semantics` -- the tier-2 model-perf
point contract (issue #1520): metadata/schemas/model-perf-v1.schema.json is
the shape, this gate is the cross-checks a schema can't express.

Every test below runs against one real, schema-valid, semantically-valid
base body -- `tests/fixtures/model_perf/e1m_aen801_ethos_u55_hp.yaml`, keyed
to E1M-AEN801's REAL ethos-u55-256 target and its REAL paired m55_hp core
(not a synthetic SoM) -- so a passing suite proves the checks accept a
genuine SKU/target/hw_rev combination, not just a self-consistent fabricated
one. `test_base_fixture_passes_every_check` is that positive control; every
other test takes a deep copy, mutates exactly ONE field to break exactly ONE
rule, and asserts THAT rule (and no other) fires -- so no check here can go
quietly vacuous.

Run locally:

    python -m pytest tests/scripts/test_model_perf_metadata.py -v
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import jsonschema
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import validate_metadata as V  # noqa: E402

_FIXTURE = (Path(__file__).resolve().parents[1] / "fixtures" / "model_perf"
            / "e1m_aen801_ethos_u55_hp.yaml")


def _base() -> dict:
    return copy.deepcopy(yaml.safe_load(_FIXTURE.read_text(encoding="utf-8")))


def _write(tmp_path: Path, doc: dict, *, dirname: str | None = None,
           stem: str | None = None, under: str | None = None) -> Path:
    """Write @doc under tmp_path at the path the identity rules expect
    (dir == sku, filename == the identity hash of @doc) unless overridden --
    so a mutation to an unrelated field never ALSO trips the path-identity
    check as a side effect."""
    base = tmp_path / under if under else tmp_path
    d = dirname if dirname is not None else str(doc.get("sku"))
    s = stem if stem is not None else V._model_perf_identity_hash(doc)
    p = base / d / f"{s}.yaml"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return p


def _msgs(tmp_path: Path, doc: dict, **write_kwargs) -> list[str]:
    p = _write(tmp_path, doc, **write_kwargs)
    failures = V._check_model_perf_semantics([p])
    if not failures:
        return []
    assert len(failures) == 1                  # one file in, at most one failure entry out
    return failures[0][1]


# --- positive control -------------------------------------------------

def test_base_fixture_passes_every_check(tmp_path):
    assert _msgs(tmp_path, _base()) == []


def test_base_fixture_is_schema_valid():
    schema = json.loads(V.MODEL_PERF_SCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    assert list(validator.iter_errors(_base())) == []


# --- (a) the path reproduces the body ----------------------------------

def test_rejects_directory_not_matching_sku(tmp_path):
    doc = _base()
    msgs = _msgs(tmp_path, doc, dirname="not-the-sku")
    assert any("directory" in m and "!=" in m for m in msgs)


def test_rejects_filename_not_matching_identity_hash(tmp_path):
    doc = _base()
    msgs = _msgs(tmp_path, doc, stem="stale0000")
    assert any("doesn't reproduce this body's measurement-identity hash" in m
               for m in msgs)


def test_a_changed_identity_field_without_a_rename_is_caught(tmp_path):
    """The exact #1520 hazard: edit an identity field, forget to rename the
    file -- the OLD (now-stale) hash stays on disk."""
    doc = _base()
    stale_stem = V._model_perf_identity_hash(doc)
    doc["target"]["core"] = "m55_he"            # would-be different measurement
    doc["target"]["accel_config"] = "ethos-u55-128"
    doc["vela"]["system_config"] = "Ethos_U55_High_End_Embedded_HE"
    p = _write(tmp_path, doc, stem=stale_stem)  # filename NOT updated
    failures = V._check_model_perf_semantics([p])
    assert failures
    assert any("doesn't reproduce this body's measurement-identity hash" in m
               for m in failures[0][1])


# --- (b) the SKU exists --------------------------------------------------

def test_rejects_sku_with_no_som_preset(tmp_path):
    doc = _base()
    doc["sku"] = "E1M-AEN999"                   # matches the sku pattern; preset doesn't exist
    msgs = _msgs(tmp_path, doc)
    assert any("no metadata/e1m_modules/E1M-AEN999.yaml preset" in m for m in msgs)


# --- (c) (backend, accel_config) + core resolve for the SKU -------------

def test_rejects_target_pair_the_sku_does_not_resolve(tmp_path):
    doc = _base()
    doc["target"]["accel_config"] = "ethos-u55-999"   # no such accel_config on E8
    msgs = _msgs(tmp_path, doc)
    assert any("is not a target `E1M-AEN801` actually resolves" in m for m in msgs)


def test_rejects_core_not_in_soms_topology(tmp_path):
    doc = _base()
    doc["target"]["core"] = "bogus_core"
    msgs = _msgs(tmp_path, doc)
    assert any("is not a `topology:` role of `E1M-AEN801`" in m for m in msgs)


def test_rejects_core_mismatched_against_paired_core(tmp_path):
    """m55_he IS a valid E1M-AEN801 topology role -- just not the one
    npus[].paired_core pins ethos-u55-256 to (that's m55_hp)."""
    doc = _base()
    doc["target"]["core"] = "m55_he"
    msgs = _msgs(tmp_path, doc)
    assert any("the core this SoC JSON pins" in m for m in msgs)


# --- (d) hw_rev is in the family table -----------------------------------

def test_rejects_hw_rev_not_in_family_table(tmp_path):
    doc = _base()
    doc["hw_rev"] = "r99"
    msgs = _msgs(tmp_path, doc)
    assert any("is not a key in metadata/e1m_modules/aen/hw-revisions.yaml" in m
               for m in msgs)


# --- (e) an ethos_u point records its vela profile -----------------------

def test_rejects_ethos_u_point_with_no_vela_block(tmp_path):
    doc = _base()
    del doc["vela"]
    msgs = _msgs(tmp_path, doc)
    assert any("`vela:` is missing" in m for m in msgs)


def test_rejects_ethos_u_point_with_empty_vela_field(tmp_path):
    doc = _base()
    doc["vela"]["memory_mode"] = ""
    msgs = _msgs(tmp_path, doc)
    assert any("vela.memory_mode: missing/empty" in m for m in msgs)


# --- (f) req_sram_kib covers arena_bytes ---------------------------------

def test_rejects_req_sram_kib_smaller_than_arena_bytes(tmp_path):
    doc = _base()
    doc["perf"]["req_sram_kib"] = 1              # 1024 B, arena_bytes stays 300000
    msgs = _msgs(tmp_path, doc)
    assert any("is smaller than perf.arena_bytes" in m for m in msgs)


# --- (g) p95 is not below the mean ---------------------------------------

def test_rejects_p95_below_mean(tmp_path):
    doc = _base()
    doc["perf"]["latency_ms"]["p95"] = 1.0        # mean stays 12.4
    msgs = _msgs(tmp_path, doc)
    assert any("is below perf.latency_ms.mean" in m for m in msgs)


# --- (h) the run-count floor ----------------------------------------------

def test_rejects_latency_runs_below_the_floor(tmp_path):
    doc = _base()
    doc["perf"]["latency_ms"]["runs"] = 5
    msgs = _msgs(tmp_path, doc)
    assert any("is below the floor of" in m for m in msgs)


def test_accepts_latency_runs_at_exactly_the_floor(tmp_path):
    doc = _base()
    doc["perf"]["latency_ms"]["runs"] = V._MODEL_PERF_LATENCY_RUN_FLOOR
    assert _msgs(tmp_path, doc) == []


# --- (i) capture.date parses ----------------------------------------------

def test_rejects_capture_date_that_does_not_parse(tmp_path):
    doc = _base()
    doc["capture"]["date"] = "2026-02-30"         # no such day; regex-shaped but invalid
    msgs = _msgs(tmp_path, doc)
    assert any("does not parse as an ISO-8601 date" in m for m in msgs)


# --- (j) the published tree cannot absorb a `_fixture` -------------------
#
# Scoped to the SKU directory name + the filename only (not the full
# absolute path) -- see the matching comment in
# `_check_model_perf_semantics()`. A stray "_fixture" somewhere in the
# ancestor path (e.g. this very test suite's own pytest tmp_path, whose
# prefix is derived from the TEST's name) must NOT trip this rule; only the
# SKU directory or the filename itself should -- that's what the three
# tests below prove, positive and negative.

def test_rejects_fixture_suffixed_sku_directory(tmp_path):
    doc = _base()
    msgs = _msgs(tmp_path, doc, dirname=f"{doc['sku']}_fixture")
    assert any("path contains `_fixture`" in m for m in msgs)


def test_rejects_fixture_suffixed_filename(tmp_path):
    doc = _base()
    real_hash = V._model_perf_identity_hash(doc)
    msgs = _msgs(tmp_path, doc, stem=f"{real_hash}_fixture")
    assert any("path contains `_fixture`" in m for m in msgs)


def test_an_ancestor_directory_named_fixture_is_not_mistaken_for_the_rule(tmp_path):
    """The identity-correct dir/filename sit BELOW an ancestor segment that
    happens to say "fixture" (mirrors a real checkout/CI path) -- must not
    fire; only the sku-dir/filename are ever inspected."""
    doc = _base()
    msgs = _msgs(tmp_path, doc, under="some_fixture_of_a_workspace")
    assert not any("path contains `_fixture`" in m for m in msgs)

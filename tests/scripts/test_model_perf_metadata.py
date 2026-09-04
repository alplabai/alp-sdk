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
other test takes a deep copy, mutates exactly ONE field, and asserts (via
`assert any(...)`) that the rule that mutation targets fires -- NOT that it
is the only rule that fires: `test_rejects_core_not_in_soms_topology`, for
one, legitimately trips a second rule (the paired-core mismatch) on the same
mutation. So no check here can go quietly vacuous, but a test's `any(...)`
is a presence assertion, not an absence one.

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


def test_a_changed_compiler_version_without_a_rename_is_caught(tmp_path):
    """The BLOCKING-1 hazard this field was added to close (issue #1520
    review, PR #1884): a vela upgrade alone -- arena_bytes/latency_ms move,
    every OTHER identity field stays byte-identical -- must not silently
    collide on the same filename as the point it's replacing."""
    doc = _base()
    stale_stem = V._model_perf_identity_hash(doc)
    doc["target"]["compiler_version"] = "vela 5.0.0"   # upgrade; nothing else changes
    p = _write(tmp_path, doc, stem=stale_stem)          # filename NOT updated
    failures = V._check_model_perf_semantics([p])
    assert failures
    assert any("doesn't reproduce this body's measurement-identity hash" in m
               for m in failures[0][1])


def test_compiler_version_alone_changes_the_identity_hash(tmp_path):
    doc_a = _base()
    doc_b = copy.deepcopy(doc_a)
    doc_b["target"]["compiler_version"] = "vela 5.0.0"
    assert V._model_perf_identity_hash(doc_a) != V._model_perf_identity_hash(doc_b)


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


def test_rejects_vela_block_on_a_non_ethos_u_point(tmp_path):
    """A `vela:` block on a `cpu`/`drpai`/`deepx_dxm1` point is meaningless
    (no vela compile ran) and still hashes into the identity for nothing --
    a stray copy-paste from an ethos_u point (issue #1520 review, PR #1884):
    two otherwise-identical `cpu` captures, one with a leftover `vela:`
    block, must not silently collide on two different paths."""
    doc = _base()
    doc["target"]["backend"] = "cpu"
    doc["target"]["accel_config"] = ""
    doc["target"]["compiler_version"] = "passthrough"
    # doc["vela"] is left in place from the base fixture -- the copy-paste bug.
    msgs = _msgs(tmp_path, doc)
    assert any("but `vela:` is present" in m for m in msgs)


def test_accepts_a_cpu_point_with_no_vela_block(tmp_path):
    """Positive control for the rule above: a genuinely `cpu` point with no
    `vela:` block at all is clean."""
    doc = _base()
    doc["target"]["backend"] = "cpu"
    doc["target"]["accel_config"] = ""
    doc["target"]["compiler_version"] = "passthrough"
    del doc["vela"]
    assert _msgs(tmp_path, doc) == []


# --- (f) req_sram_kib covers arena_bytes ---------------------------------

def test_rejects_req_sram_kib_smaller_than_arena_bytes(tmp_path):
    doc = _base()
    doc["perf"]["req_sram_kib"] = 1              # 1024 B, arena_bytes stays 300000
    msgs = _msgs(tmp_path, doc)
    assert any("is smaller than perf.arena_bytes" in m for m in msgs)


def test_accepts_req_sram_kib_exactly_covering_arena_bytes(tmp_path):
    """The exact boundary (issue #1520 review): `req_sram_kib * 1024 ==
    arena_bytes` must pass. Also pins the multiplier itself -- a 1000-vs-1024
    slip would push this below arena_bytes and wrongly fail it."""
    doc = _base()
    doc["perf"]["arena_bytes"] = doc["perf"]["req_sram_kib"] * 1024
    assert _msgs(tmp_path, doc) == []


def test_rejects_arena_bytes_one_byte_over_the_boundary(tmp_path):
    doc = _base()
    doc["perf"]["arena_bytes"] = doc["perf"]["req_sram_kib"] * 1024 + 1
    msgs = _msgs(tmp_path, doc)
    assert any("is smaller than perf.arena_bytes" in m for m in msgs)


# --- (g) p95 is not below the mean; p50 is not above p95 -----------------

def test_rejects_p95_below_mean(tmp_path):
    doc = _base()
    doc["perf"]["latency_ms"]["p95"] = 1.0        # mean stays 12.4
    msgs = _msgs(tmp_path, doc)
    assert any("is below perf.latency_ms.mean" in m for m in msgs)


def test_accepts_p95_exactly_equal_to_mean(tmp_path):
    doc = _base()
    doc["perf"]["latency_ms"]["p95"] = doc["perf"]["latency_ms"]["mean"]
    assert _msgs(tmp_path, doc) == []


def test_accepts_mean_p50_p95_all_equal(tmp_path):
    """A deterministic NPU timed at millisecond resolution can legitimately
    give mean == p50 == p95 -- not an error."""
    doc = _base()
    doc["perf"]["latency_ms"]["mean"] = 12.0
    doc["perf"]["latency_ms"]["p50"] = 12.0
    doc["perf"]["latency_ms"]["p95"] = 12.0
    assert _msgs(tmp_path, doc) == []


def test_rejects_p50_above_p95(tmp_path):
    """The p95-rule's own data-entry class, unpinned before this (issue
    #1520 review): {mean: 12.4, p50: 99.0, p95: 15.8} used to pass clean."""
    doc = _base()
    doc["perf"]["latency_ms"]["p50"] = 99.0       # p95 stays 15.8
    msgs = _msgs(tmp_path, doc)
    assert any("p50" in m and "is above" in m and "p95" in m for m in msgs)


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


# --- the collector: MODEL_PERF.glob("*/*.yaml")'s replacement --------------
#
# Every test above calls `_check_model_perf_semantics([p])` with an
# EXPLICIT path list -- none of them exercises the COLLECTOR that finds
# those paths under metadata/model_perf/ in the first place
# (`V._collect_model_perf_files()`). That collector had zero coverage
# (issue #1520 review, PR #1884): the one-level `glob("*/*.yaml")` it
# replaced never opened `<SKU>/_fixture/<hash>.yaml` -- the precise
# `_fixture` evasion the marker check above exists to catch -- so the gate
# printed a clean `0 failure(s)` for a file nobody looked at.

def test_collector_finds_a_correctly_placed_point(tmp_path):
    p = tmp_path / "E1M-AEN801" / "abc0123456789def.yaml"
    p.parent.mkdir(parents=True)
    p.write_text("schema_version: 1\n", encoding="utf-8")
    found, failures = V._collect_model_perf_files(tmp_path)
    assert found == [p]
    assert failures == []


def test_collector_skips_its_own_readme(tmp_path):
    (tmp_path / "README.md").write_text("# model_perf\n", encoding="utf-8")
    found, failures = V._collect_model_perf_files(tmp_path)
    assert found == []
    assert failures == []


def test_collector_rejects_a_point_nested_one_level_too_deep(tmp_path):
    """The precise `_fixture` evasion this review measured: a file placed
    at metadata/model_perf/<SKU>/_fixture/<hash>.yaml sits THREE segments
    below the tree root -- one deeper than a real point's two. It must
    come back as a FAILURE, not silently vanish from collection."""
    p = tmp_path / "E1M-AEN801" / "_fixture" / "abc0123456789def.yaml"
    p.parent.mkdir(parents=True)
    p.write_text("schema_version: 1\n", encoding="utf-8")
    found, failures = V._collect_model_perf_files(tmp_path)
    assert found == []
    assert len(failures) == 1
    assert "path segment" in failures[0][1][0]


def test_collector_rejects_a_stray_file_directly_under_root(tmp_path):
    p = tmp_path / "stray.yaml"
    p.write_text("schema_version: 1\n", encoding="utf-8")
    found, failures = V._collect_model_perf_files(tmp_path)
    assert found == []
    assert len(failures) == 1
    assert "path segment" in failures[0][1][0]


def test_collector_rejects_a_yml_sibling(tmp_path):
    p = tmp_path / "E1M-AEN801" / "abc0123456789def.yml"
    p.parent.mkdir(parents=True)
    p.write_text("schema_version: 1\n", encoding="utf-8")
    found, failures = V._collect_model_perf_files(tmp_path)
    assert found == []
    assert len(failures) == 1
    assert "extension" in failures[0][1][0]


def test_collector_on_a_missing_root_is_a_silent_no_op(tmp_path):
    found, failures = V._collect_model_perf_files(tmp_path / "does-not-exist")
    assert found == []
    assert failures == []

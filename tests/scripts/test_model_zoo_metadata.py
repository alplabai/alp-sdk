# SPDX-License-Identifier: Apache-2.0
"""`metadata/model_zoo/<id>.yaml` -- the model-zoo v1 data asset (ADR-0028,
alp-sdk#2539). `metadata/schemas/model-zoo-v1.schema.json` is the shape;
`validate_metadata._check_model_zoo_semantics` is the cross-checks a schema
can't express (id/filename join key, `source.bundled` resolves inside
starters/).

Scope note: this file covers alp-sdk's half only -- the schema, the seed
entry, and the validator gate. `scripts/alp_model/zoo.py`
(`load_zoo`/`filter_by_sku`/`fetch_source`) is out of scope per ADR-0028;
it is ported into tan-cli under tan-cli#1286, not here.

Run locally:

    python -m pytest tests/scripts/test_model_zoo_metadata.py -v
"""
from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import jsonschema
import pytest
import yaml

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))

import validate_metadata as V  # noqa: E402

_FIXTURE = _ROOT / "metadata" / "model_zoo" / "example-tiny.yaml"


def _base() -> dict:
    return copy.deepcopy(yaml.safe_load(_FIXTURE.read_text(encoding="utf-8")))


def _schema_errors(doc: dict) -> list:
    schema = json.loads(V.MODEL_ZOO_SCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    return list(validator.iter_errors(doc))


def _write(tmp_path: Path, doc: dict, *, name: str = "example-tiny.yaml") -> Path:
    p = tmp_path / name
    p.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return p


# --- positive controls -----------------------------------------------------

def test_real_fixture_is_schema_valid():
    assert _schema_errors(_base()) == []


def test_real_fixture_has_no_semantic_failures():
    failures = V._check_model_zoo_semantics([_FIXTURE])
    assert failures == []


def test_real_starter_file_exists_and_matches_tiny_int8_fixture():
    """The bundled starter must be the exact bytes of the hermetic
    tests/fixtures/models/tiny_int8.tflite generator output -- not a
    divergent copy -- so its provenance (synthetic weights, no
    redistribution) genuinely applies to the shipped file."""
    starter = V.MODEL_ZOO / "starters" / "example-tiny.tflite"
    source = _ROOT / "tests" / "fixtures" / "models" / "tiny_int8.tflite"
    assert starter.is_file()
    assert starter.read_bytes() == source.read_bytes()


def test_missing_validated_soms_is_rejected():
    doc = _base()
    del doc["validated_soms"]
    errors = _schema_errors(doc)
    assert errors, "validated_soms is a required field even when empty"


def test_duplicate_validated_soms_entries_rejected():
    doc = _base()
    doc["validated_soms"] = ["E1M-AEN801", "E1M-AEN801"]
    errors = _schema_errors(doc)
    assert errors, "uniqueItems must reject a duplicated SKU"


def test_malformed_sku_in_validated_soms_rejected():
    doc = _base()
    doc["validated_soms"] = ["E1M-FOO123"]
    errors = _schema_errors(doc)
    assert errors, "a SKU family not in the real vocabulary must fail the pattern"


# --- source: url requires sha256, exclusively of bundled --------------------

def test_url_source_without_sha256_is_rejected():
    doc = _base()
    doc["source"] = {"url": "https://example.com/model.tflite"}
    errors = _schema_errors(doc)
    assert errors, "a url source with no sha256 must fail schema validation"


def test_url_source_with_sha256_is_accepted():
    doc = _base()
    doc["kind"] = "model"  # kind: fixture's if/then requires a bundled source
    doc["source"] = {
        "url": "https://example.com/model.tflite",
        "sha256": "a" * 64,
    }
    assert _schema_errors(doc) == []


def test_bad_sha256_format_is_rejected():
    doc = _base()
    doc["source"] = {
        "url": "https://example.com/model.tflite",
        "sha256": "not-a-real-hash",
    }
    errors = _schema_errors(doc)
    assert errors, "a malformed sha256 (not 64 lowercase hex chars) must fail"


def test_uppercase_sha256_is_rejected():
    doc = _base()
    doc["source"] = {
        "url": "https://example.com/model.tflite",
        "sha256": "A" * 64,
    }
    errors = _schema_errors(doc)
    assert errors, "sha256 must be lowercase hex per the schema pattern"


def test_file_url_scheme_is_rejected():
    """Published model-zoo data must never carry a local filesystem path --
    only https:// is a valid url source."""
    doc = _base()
    doc["source"] = {
        "url": "file:///home/dev/model.tflite",
        "sha256": "a" * 64,
    }
    errors = _schema_errors(doc)
    assert errors, "file:// must be rejected -- https:// only"


def test_url_and_bundled_together_is_rejected():
    """A source with BOTH url and bundled is ambiguous about which is the
    real provenance -- the two branches must be mutually exclusive."""
    doc = _base()
    doc["source"] = {
        "url": "https://example.com/model.tflite",
        "bundled": "starters/example-tiny.tflite",
    }
    errors = _schema_errors(doc)
    assert errors, "url + bundled together must be rejected (not just url without sha256)"


def test_bundled_and_sha256_together_is_rejected():
    """An unchecked sha256 sitting next to `bundled` (no url to verify it
    against) is a hash nothing ever computes or compares -- reject the
    combination outright rather than silently ignoring the field."""
    doc = _base()
    doc["source"] = {
        "bundled": "starters/example-tiny.tflite",
        "sha256": "a" * 64,
    }
    errors = _schema_errors(doc)
    assert errors, "bundled + sha256 together must be rejected"


def test_url_sha256_and_bundled_all_together_is_rejected():
    doc = _base()
    doc["source"] = {
        "url": "https://example.com/model.tflite",
        "sha256": "a" * 64,
        "bundled": "starters/example-tiny.tflite",
    }
    errors = _schema_errors(doc)
    assert errors, "all three source fields together must be rejected"


# --- additionalProperties: false --------------------------------------------

def test_unknown_top_level_field_is_rejected():
    doc = _base()
    doc["unexpected_field"] = "surprise"
    errors = _schema_errors(doc)
    assert errors, "an unrecognised top-level field must fail (additionalProperties: false)"


def test_unknown_source_field_is_rejected():
    doc = _base()
    doc["source"] = {"bundled": "starters/example-tiny.tflite", "extra": "nope"}
    errors = _schema_errors(doc)
    assert errors, "an unrecognised source field must fail (additionalProperties: false)"


# --- id <-> filename join key -----------------------------------------------

def test_id_mismatched_against_filename_is_caught(tmp_path):
    doc = _base()
    doc["id"] = "some-other-id"
    p = _write(tmp_path, doc)  # filename stays example-tiny.yaml
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("does not match filename" in m for m in failures[0][1])


# --- source.bundled must resolve to a real file inside starters/ -----------

def test_bundled_path_that_does_not_exist_is_rejected(tmp_path):
    doc = _base()
    doc["source"] = {"bundled": "starters/does-not-exist.tflite"}
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("does not resolve to a real file" in m for m in failures[0][1])


def test_bundled_path_escaping_starters_dir_is_rejected(tmp_path):
    """A `../` escape out of starters/ must be refused even if it happens
    to resolve to a real file elsewhere in the repo."""
    doc = _base()
    doc["source"] = {"bundled": "../schemas/model-zoo-v1.schema.json"}
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("resolves outside" in m for m in failures[0][1])


# --- validated_soms SKU must exist as a real SoM preset ---------------------

def test_wellformed_but_nonexistent_sku_is_caught(tmp_path):
    """`E1M-AEN899` matches the schema's family/digit pattern but names no
    real SoM preset (the real AEN SKUs are 301/401/501/601/701/801/803) --
    the schema alone can't catch this, only the semantic cross-check can."""
    doc = _base()
    doc["validated_soms"] = ["E1M-AEN899"]
    assert not (V.SOM_PRESETS / "E1M-AEN899.yaml").is_file(), \
        "test assumption broken: E1M-AEN899 now exists as a real preset"
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("E1M-AEN899" in m and "no metadata/e1m_modules" in m for m in failures[0][1])


def test_real_soms_in_validated_soms_pass(tmp_path):
    """Positive control: a SKU that DOES have a real preset must not trip
    the existence check. kind: model (not fixture) -- a fixture can never
    carry a populated validated_soms by the item-2 cross-check, so this
    positive control is meaningless under kind: fixture."""
    doc = _base()
    doc["kind"] = "model"
    doc["validated_soms"] = ["E1M-AEN801"]
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures == []


# --- main() wiring: a bad model_zoo entry fails the whole run ---------------

def test_main_fails_when_a_model_zoo_entry_is_invalid(monkeypatch, capsys):
    """Exercises the main() wiring itself, not just the helper functions in
    isolation -- a bad entry under metadata/model_zoo/ must flip main()'s
    exit code, proving the model_zoo block's failures actually reach
    total_failures rather than being computed and silently dropped.

    `_check_files()` (shared by every section of this validator, not just
    model_zoo) reports paths relative to REPO, so the scratch dir has to
    live INSIDE the real repo tree -- an unrelated pytest tmp_path would
    trip `path.relative_to(REPO)` for reasons that have nothing to do with
    the model_zoo wiring this test targets."""
    import shutil
    import tempfile

    scratch = Path(tempfile.mkdtemp(dir=_ROOT))
    try:
        bad = scratch / "broken.yaml"
        bad.write_text(
            "id: broken\ntask: example\ndescription: x\nlicense: Apache-2.0\n"
            "source:\n  bundled: starters/does-not-exist.tflite\n"
            "validated_soms: []\n",
            encoding="utf-8",
        )
        monkeypatch.setattr(V, "MODEL_ZOO", scratch)
        monkeypatch.setattr(V, "MODEL_ZOO_STARTERS", scratch / "starters")
        rc = V.main()
        out = capsys.readouterr().out
        assert rc != 0
        assert "broken.yaml" in out
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def test_main_fails_and_prints_a_stray_root_file(monkeypatch, capsys):
    """A stray file directly under metadata/model_zoo/ is a
    `_collect_model_zoo_files` COLLECTOR failure -- counted into
    total_failures already, but (pre-fix) never actually PRINTED, unlike
    every other collector (model_perf's own collector failures get an
    explicit print loop). A human reading the gate's output for this
    specific failure would see the exit code but never the offending
    path -- assert the path actually appears in stdout, not just rc!=0."""
    import shutil
    import tempfile

    scratch = Path(tempfile.mkdtemp(dir=_ROOT))
    try:
        stray = scratch / "stray.txt"
        stray.write_text("x", encoding="utf-8")
        monkeypatch.setattr(V, "MODEL_ZOO", scratch)
        monkeypatch.setattr(V, "MODEL_ZOO_STARTERS", scratch / "starters")
        rc = V.main()
        out = capsys.readouterr().out
        assert rc != 0
        assert "stray.txt" in out
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def test_main_fails_and_prints_an_orphan_starter(monkeypatch, capsys):
    """An orphan starter (referenced by no entry) must be visible in
    main()'s own output, proving `_check_model_zoo_starters` is actually
    wired into main() -- not just unit-testable in isolation."""
    import shutil
    import tempfile

    scratch = Path(tempfile.mkdtemp(dir=_ROOT))
    try:
        good = _base()
        (scratch / "example-tiny.yaml").write_text(
            yaml.safe_dump(good, sort_keys=False), encoding="utf-8")
        starters_dir = scratch / "starters"
        starters_dir.mkdir()
        (starters_dir / "example-tiny.tflite").write_bytes(
            (_ROOT / "tests" / "fixtures" / "models" / "tiny_int8.tflite").read_bytes())
        orphan = starters_dir / "orphan.tflite"
        orphan.write_bytes(b"x")
        monkeypatch.setattr(V, "MODEL_ZOO", scratch)
        monkeypatch.setattr(V, "MODEL_ZOO_STARTERS", starters_dir)
        rc = V.main()
        out = capsys.readouterr().out
        assert rc != 0
        assert "orphan.tflite" in out
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# --- schema_version (contract-freeze, #2539 adversarial review item 1) -----

def test_missing_schema_version_is_rejected():
    doc = _base()
    del doc["schema_version"]
    errors = _schema_errors(doc)
    assert errors, "schema_version is required, same as model-perf-v1"


def test_wrong_schema_version_value_is_rejected():
    doc = _base()
    doc["schema_version"] = 2
    errors = _schema_errors(doc)
    assert errors, "schema_version is const 1 -- no other value is valid yet"


def test_string_schema_version_is_rejected():
    doc = _base()
    doc["schema_version"] = "1"
    errors = _schema_errors(doc)
    assert errors, "schema_version must be an integer, not a string"


# --- kind discriminator (item 2) --------------------------------------------

def test_missing_kind_is_rejected():
    doc = _base()
    del doc["kind"]
    errors = _schema_errors(doc)
    assert errors, "kind is required"


def test_bad_kind_value_is_rejected():
    doc = _base()
    doc["kind"] = "widget"
    errors = _schema_errors(doc)
    assert errors, "kind must be one of the enum values"


def test_fixture_with_nonempty_validated_soms_is_rejected(tmp_path):
    """kind: fixture => validated_soms MUST be empty -- a fixture is never a
    bench-validated hardware claim by construction."""
    doc = _base()
    assert doc["kind"] == "fixture"
    doc["validated_soms"] = ["E1M-AEN801"]
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("kind: fixture" in m and "validated_soms" in m for m in failures[0][1])


def test_fixture_with_url_source_is_rejected(tmp_path):
    """kind: fixture => source MUST be `bundled` -- a fixture never links an
    external upstream model."""
    doc = _base()
    assert doc["kind"] == "fixture"
    doc["source"] = {"url": "https://example.com/model.tflite", "sha256": "a" * 64}
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("kind: fixture" in m and "bundled" in m for m in failures[0][1])


# --- fixture rule enforced at the SCHEMA level too (if/then), not just the
# Python semantic check -- jsonschema-only, no validator involved ----------

def test_schema_rejects_fixture_with_url_source():
    doc = _base()
    doc["kind"] = "fixture"
    doc["source"] = {"url": "https://example.com/model.tflite", "sha256": "a" * 64}
    errors = _schema_errors(doc)
    assert errors, "the schema's own if/then must reject fixture + url, not just the Python check"


def test_schema_rejects_fixture_with_nonempty_validated_soms():
    doc = _base()
    doc["kind"] = "fixture"
    doc["validated_soms"] = ["E1M-AEN801"]
    errors = _schema_errors(doc)
    assert errors, "the schema's own if/then must reject fixture + populated validated_soms"


def test_schema_accepts_model_kind_with_url_and_soms():
    """Positive control: the if/then only fires for kind: fixture."""
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://example.com/model.tflite", "sha256": "a" * 64}
    doc["validated_soms"] = ["E1M-AEN801"]
    assert _schema_errors(doc) == []


def test_model_kind_with_url_source_and_soms_is_accepted():
    """Positive control: kind: model has no such restriction -- a real
    curated entry may carry both a url source and validated_soms."""
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://example.com/model.tflite", "sha256": "a" * 64}
    doc["validated_soms"] = ["E1M-AEN801"]
    assert _schema_errors(doc) == []


# --- compile: aligned with board.schema.json's models[].compile (item 3) ---

def _strip_descriptions(obj):
    """Recursively drop every `description` key. `compile`'s `description`
    legitimately differs between the two schemas (board.schema.json's says
    "Paths are relative to this board.yaml", true for a customer's own
    board.yaml; model-zoo-v1's own copy says paths are relative to the
    CONSUMING project's board.yaml after `tan model add` copies this block
    in -- the zoo itself ships no compile inputs) -- everything else
    (types, required, additionalProperties, nested property shapes) must
    still match exactly."""
    if isinstance(obj, dict):
        return {k: _strip_descriptions(v) for k, v in obj.items() if k != "description"}
    if isinstance(obj, list):
        return [_strip_descriptions(v) for v in obj]
    return obj


def _board_compile_def() -> dict:
    board_schema = json.loads(
        (V.REPO / "metadata" / "schemas" / "board.schema.json").read_text(encoding="utf-8"))
    return (board_schema["properties"]["models"]["items"]["properties"]["compile"])


def test_compile_matches_board_schema_definition():
    """The two `compile` shapes must never structurally drift apart --
    board.schema.json is the shape `tan model add` actually writes into
    board.yaml `models[].compile`, so model-zoo-v1's own `compile` is a
    deliberate COPY (the repo's established pattern for this -- see
    board-preset.schema.json's own comment mirroring board.schema.json's
    route_entry -- rather than a cross-file $ref: every validator in
    scripts/validate_metadata.py is instantiated as
    `jsonschema.Draft202012Validator(schema)` with no resolver/registry
    argument, and no schema in this tree uses a cross-file $ref anywhere,
    so a $ref into board.schema.json would not actually resolve here).
    This test is the drift guard the copy needs in place of a $ref --
    compared with `description` keys stripped (see `_strip_descriptions`),
    since the top-level `compile.description` is legitimately reworded for
    the zoo's own context; every other keyword must match exactly."""
    board_compile = _board_compile_def()
    zoo_schema = json.loads(V.MODEL_ZOO_SCHEMA.read_text(encoding="utf-8"))
    zoo_compile = zoo_schema["properties"]["compile"]
    assert _strip_descriptions(zoo_compile) == _strip_descriptions(board_compile), (
        "model-zoo-v1.schema.json's `compile` has structurally drifted "
        "from board.schema.json's models[].compile -- tan model add "
        "writes into the latter, so the two must stay structurally "
        "identical (descriptions aside)")


def test_compile_drift_guard_still_catches_a_real_structural_difference():
    """Self-test of the test above: stripping `description` must not make
    the equality check vacuous. Mutate a NON-description keyword on a
    deep copy of the real board definition and confirm the
    description-stripped comparison still tells them apart."""
    board_compile = _board_compile_def()
    mutated = copy.deepcopy(board_compile)
    mutated["properties"]["drpai"]["required"] = []  # a real structural difference
    assert _strip_descriptions(mutated) != _strip_descriptions(board_compile)


def test_compile_with_unknown_key_is_rejected():
    doc = _base()
    doc["compile"] = {"not_a_real_backend": {}}
    errors = _schema_errors(doc)
    assert errors, "compile is backend-keyed with additionalProperties: false, same as board.yaml"


def test_compile_drpai_missing_required_spec_is_rejected():
    doc = _base()
    doc["compile"] = {"drpai": {}}
    errors = _schema_errors(doc)
    assert errors, "drpai.spec is required, mirroring board.schema.json"


def test_compile_deepx_dxm1_valid_block_is_accepted():
    doc = _base()
    doc["compile"] = {
        "deepx_dxm1": {"config": "compile/dxm1.json", "calibration": "compile/calib/"}
    }
    assert _schema_errors(doc) == []


# --- url: tightened pattern (item 4) ----------------------------------------

def test_bare_https_scheme_with_nothing_after_is_rejected():
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://", "sha256": "a" * 64}
    doc["validated_soms"] = []
    errors = _schema_errors(doc)
    assert errors, "https:// with no host/path must be rejected"


def test_url_with_embedded_space_is_rejected():
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https:// evil.example.com/x", "sha256": "a" * 64}
    doc["validated_soms"] = []
    errors = _schema_errors(doc)
    assert errors, "a url containing a space must be rejected"


def test_url_with_embedded_newline_is_rejected():
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://example.com/\nmodel.tflite", "sha256": "a" * 64}
    doc["validated_soms"] = []
    errors = _schema_errors(doc)
    assert errors, "a url containing a newline must be rejected"


def test_url_host_with_no_path_is_rejected():
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://evil", "sha256": "a" * 64}
    doc["validated_soms"] = []
    errors = _schema_errors(doc)
    assert errors, "a url with a host but no path segment must be rejected"


def test_well_formed_url_is_still_accepted():
    """Positive control for the tightened pattern."""
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://example.com/model.tflite", "sha256": "a" * 64}
    doc["validated_soms"] = []
    assert _schema_errors(doc) == []


def test_url_with_userinfo_credentials_is_rejected():
    """No username:password@ in a published manifest -- credentials don't
    belong in a public data asset."""
    doc = _base()
    doc["kind"] = "model"
    doc["source"] = {"url": "https://user:tok@host/x", "sha256": "a" * 64}
    doc["validated_soms"] = []
    errors = _schema_errors(doc)
    assert errors, "a url with embedded userinfo/credentials must be rejected"


# --- bundled: tightened in-schema pattern (item 5) --------------------------

def test_bundled_absolute_path_is_rejected_by_schema():
    doc = _base()
    doc["source"] = {"bundled": "/abs/path/model.tflite"}
    errors = _schema_errors(doc)
    assert errors, "an absolute bundled path must fail the schema pattern"


def test_bundled_parent_escape_is_rejected_by_schema():
    doc = _base()
    doc["source"] = {"bundled": "../x"}
    errors = _schema_errors(doc)
    assert errors, "a `../` bundled path must fail the schema pattern, not just the validator"


def test_bundled_nested_subdir_is_rejected_by_schema():
    doc = _base()
    doc["source"] = {"bundled": "starters/sub/x.tflite"}
    errors = _schema_errors(doc)
    assert errors, "bundled must be a flat starters/<file>, no subdirectory"


def test_bundled_trailing_slash_only_is_rejected_by_schema():
    doc = _base()
    doc["source"] = {"bundled": "starters/"}
    errors = _schema_errors(doc)
    assert errors, "bundled with no filename after starters/ must fail the schema pattern"


def test_bundled_well_formed_path_is_still_accepted_by_schema():
    doc = _base()
    doc["source"] = {"bundled": "starters/example-tiny.tflite"}
    assert _schema_errors(doc) == []


# --- validator hardening (item 6) -------------------------------------------

def test_stray_file_directly_under_model_zoo_root_is_rejected(tmp_path):
    (tmp_path / "stray.txt").write_text("x", encoding="utf-8")
    entries, starters, failures = V._collect_model_zoo_files(tmp_path)
    assert entries == []
    assert starters == []
    assert len(failures) == 1
    assert "stray.txt" in failures[0][0]


def test_file_nested_too_deep_under_starters_is_rejected(tmp_path):
    nested = tmp_path / "starters" / "sub" / "x.tflite"
    nested.parent.mkdir(parents=True)
    nested.write_bytes(b"x")
    entries, starters, failures = V._collect_model_zoo_files(tmp_path)
    assert starters == []
    assert len(failures) == 1
    assert "sub/x.tflite" in failures[0][0] or "sub\\x.tflite" in failures[0][0]


def test_collector_accepts_readme_at_root(tmp_path):
    (tmp_path / "README.md").write_text("# model_zoo\n", encoding="utf-8")
    entries, starters, failures = V._collect_model_zoo_files(tmp_path)
    assert entries == []
    assert starters == []
    assert failures == []


def test_collector_finds_entries_and_starters(tmp_path):
    (tmp_path / "a.yaml").write_text("id: a\n", encoding="utf-8")
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir()
    (starters_dir / "a.tflite").write_bytes(b"x")
    entries, starters, failures = V._collect_model_zoo_files(tmp_path)
    assert [p.name for p in entries] == ["a.yaml"]
    assert [p.name for p in starters] == ["a.tflite"]
    assert failures == []


def test_orphan_starter_is_rejected(tmp_path, monkeypatch):
    """A starters/ file that no entry's source.bundled references is
    untracked payload sitting in the published tree.

    `_check_model_zoo_starters` resolves `source.bundled` against the
    module-level `MODEL_ZOO` global (same as `_check_model_zoo_semantics`),
    so every test below monkeypatches it to the tmp tree it builds --
    otherwise `bundled` resolves against the REAL repo's starters/ and
    every comparison is meaningless regardless of what tmp_path holds."""
    monkeypatch.setattr(V, "MODEL_ZOO", tmp_path)
    entry = _base()
    entry_path = _write(tmp_path, entry)  # references starters/example-tiny.tflite
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir(exist_ok=True)
    orphan = starters_dir / "orphan.tflite"
    orphan.write_bytes(b"x")
    failures = V._check_model_zoo_starters([entry_path], [orphan])
    assert failures
    assert any("orphan" in m.lower() for m in failures[0][1])


def test_referenced_starter_is_not_flagged_as_orphan(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "MODEL_ZOO", tmp_path)
    entry = _base()  # references starters/example-tiny.tflite
    entry_path = _write(tmp_path, entry)
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir(exist_ok=True)
    referenced = starters_dir / "example-tiny.tflite"
    referenced.write_bytes(b"x")
    failures = V._check_model_zoo_starters([entry_path], [referenced])
    assert failures == []


def test_starter_over_size_cap_is_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "MODEL_ZOO", tmp_path)
    entry = _base()
    entry["source"] = {"bundled": "starters/big.tflite"}
    entry_path = _write(tmp_path, entry)
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir(exist_ok=True)
    big = starters_dir / "big.tflite"
    big.write_bytes(b"\x00" * (V._MODEL_ZOO_STARTER_MAX_BYTES + 1))
    failures = V._check_model_zoo_starters([entry_path], [big])
    assert failures
    assert any("exceeds" in m for m in failures[0][1])


def test_starter_at_exactly_the_size_cap_is_accepted(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "MODEL_ZOO", tmp_path)
    entry = _base()
    entry["source"] = {"bundled": "starters/exact.tflite"}
    entry_path = _write(tmp_path, entry)
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir(exist_ok=True)
    exact = starters_dir / "exact.tflite"
    exact.write_bytes(b"\x00" * V._MODEL_ZOO_STARTER_MAX_BYTES)
    failures = V._check_model_zoo_starters([entry_path], [exact])
    assert failures == []


def test_starter_stat_oserror_is_reported_not_silently_skipped(tmp_path, monkeypatch):
    """A stat() failure (permissions, a broken symlink, a race with a
    concurrent delete) must surface as a FAILURE, not silently skip the
    size-cap check as if the file were fine."""
    monkeypatch.setattr(V, "MODEL_ZOO", tmp_path)
    entry = _base()
    entry["source"] = {"bundled": "starters/ghost.tflite"}
    entry_path = _write(tmp_path, entry)
    starters_dir = tmp_path / "starters"
    starters_dir.mkdir(exist_ok=True)
    ghost = starters_dir / "ghost.tflite"
    ghost.write_bytes(b"x")

    real_stat = Path.stat

    def _boom(self, *a, **kw):
        if self == ghost:
            raise OSError("simulated stat failure")
        return real_stat(self, *a, **kw)

    monkeypatch.setattr(Path, "stat", _boom)
    failures = V._check_model_zoo_starters([entry_path], [ghost])
    assert failures
    assert any("stat" in m.lower() for m in failures[0][1])


def test_bundled_case_exact_helper_rejects_case_mismatch(tmp_path):
    """Unit-level test of the exact primitive `_check_model_zoo_semantics`
    uses for the case check -- independent of whether THIS host's
    filesystem happens to be case-insensitive (macOS APFS, Windows) or
    case-sensitive (Linux CI), `os.listdir` is always an exact byte
    comparison, so this is portable regardless of host FS behaviour."""
    (tmp_path / "example-tiny.tflite").write_bytes(b"x")
    assert V._bundled_case_exact("example-tiny.tflite", tmp_path) is True
    assert V._bundled_case_exact("Example-Tiny.tflite", tmp_path) is False
    assert V._bundled_case_exact("does-not-exist.tflite", tmp_path) is False


def test_bundled_path_wrong_case_is_rejected(tmp_path):
    """Integration-level: end to end through `_check_model_zoo_semantics`.
    On a case-insensitive host (this dev machine) the wrong-case path
    still resolves to the real file, so this specifically exercises the
    case-exact guard rather than the plain does-not-exist path; on a
    case-sensitive CI host the wrong-case path fails to resolve at all,
    which is ALSO correctly rejected (for a different, equally valid,
    reason) -- so only `failures` truthiness is asserted here, not the
    exact message, to stay meaningful on both."""
    doc = _base()
    doc["source"] = {"bundled": "starters/Example-Tiny.tflite"}
    p = _write(tmp_path, doc)
    failures = V._check_model_zoo_semantics([p])
    assert failures


def test_case_exact_wiring_fires_the_specific_message_on_any_host(tmp_path, monkeypatch):
    """Host-independent coverage of the `_check_model_zoo_semantics` WIRING
    to `_bundled_case_exact` -- forces the case-mismatch branch by
    monkeypatching the helper to always report a mismatch, regardless of
    whether THIS host's filesystem is case-sensitive (Linux CI) or not
    (macOS dev). Uses a real, correctly-cased, existing bundled path (so
    `resolved.is_file()` is True on every host), meaning the ONLY way this
    fires is via the wiring under test -- not a side effect of a
    genuinely-missing file, unlike `test_bundled_path_wrong_case_is_rejected`
    above, which depends on host FS behaviour."""
    doc = _base()  # references the real starters/example-tiny.tflite
    p = _write(tmp_path, doc)
    monkeypatch.setattr(V, "_bundled_case_exact", lambda name, directory: False)
    failures = V._check_model_zoo_semantics([p])
    assert failures
    assert any("case-sensitive" in m for m in failures[0][1])


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))

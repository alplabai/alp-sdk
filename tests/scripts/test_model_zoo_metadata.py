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
    the existence check."""
    doc = _base()
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


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))

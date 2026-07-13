# SPDX-License-Identifier: Apache-2.0
"""Tests for the versioned machine diagnostics format (#610 SS4):

  * scripts/alp_cli/diagnostic_format.py (to_machine_json / to_sarif)
  * metadata/schemas/diagnostic-v1.schema.json
  * the `alp validate --format json|sarif` CLI wiring

These lock: the zero-based LSP range conversion, the one-based SARIF
region conversion, schema self-validity + conformance, and -- the
regression lock the epic explicitly asks for -- that the human `render()`
path is byte-for-byte UNCHANGED by any of this.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import jsonschema
import pytest

from alp_cli.diagnostic import Diagnostic, render
from alp_cli.diagnostic_format import to_machine_json, to_sarif

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "diagnostic-v1.schema.json"
FIX_BAD = REPO / "tests" / "fixtures" / "board_yaml_bad"


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def _validator() -> jsonschema.Draft202012Validator:
    schema = _schema()
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def _diag(**overrides) -> Diagnostic:
    fields = dict(
        severity="error",
        path=Path("board.yaml"),
        line=5,
        col=11,
        span=3,
        code="ALP-B005",
        message="pad 'P21' not present on E1M-AEN801",
        hint="did you mean 'P20'?",
        doc_url=None,
    )
    fields.update(overrides)
    return Diagnostic(**fields)


# ---------------------------------------------------------------------------
# schema self-validity
# ---------------------------------------------------------------------------


def test_schema_is_valid_draft202012():
    jsonschema.Draft202012Validator.check_schema(_schema())


# ---------------------------------------------------------------------------
# to_machine_json
# ---------------------------------------------------------------------------


def test_to_machine_json_has_version_handshake_and_tool():
    doc = to_machine_json([_diag()])
    assert doc["schemaVersion"] == 1
    assert doc["tool"]["name"]
    assert doc["tool"]["version"]


def test_to_machine_json_conforms_to_schema():
    doc = to_machine_json([_diag(), _diag(severity="warning", code="ALP-B010", hint=None)])
    _validator().validate(doc)


def test_to_machine_json_converts_1based_to_0based_range():
    # Diagnostic.line=5, col=11, span=3 (1-based, Rust-style) must become
    # a zero-based LSP range: start=(4,10), end.character=start+span=13.
    diag = _diag(line=5, col=11, span=3)
    doc = to_machine_json([diag])
    rng = doc["diagnostics"][0]["range"]
    assert rng["start"] == {"line": 4, "character": 10}
    assert rng["end"] == {"line": 4, "character": 13}


def test_to_machine_json_omits_absent_hint_includes_documentation_uri():
    diag = _diag(hint=None, doc_url=None)
    entry = to_machine_json([diag])["diagnostics"][0]
    assert "hint" not in entry
    assert entry["documentationUri"] == "docs/diagnostics/ALP-B005.md"


def test_to_machine_json_carries_hint_and_explicit_doc_url():
    diag = _diag(hint="try this", doc_url="https://example.com/ALP-B005")
    entry = to_machine_json([diag])["diagnostics"][0]
    assert entry["hint"] == "try this"
    assert entry["documentationUri"] == "https://example.com/ALP-B005"


def test_to_machine_json_empty_collection_conforms():
    doc = to_machine_json([])
    _validator().validate(doc)
    assert doc["diagnostics"] == []


# ---------------------------------------------------------------------------
# to_sarif
# ---------------------------------------------------------------------------


def test_to_sarif_is_structurally_valid_sarif_210():
    doc = to_sarif([_diag()])
    assert doc["version"] == "2.1.0"
    assert doc["runs"], "expected at least one run"
    run = doc["runs"][0]
    assert run["tool"]["driver"]["name"]
    result = run["results"][0]
    assert result["ruleId"] == "ALP-B005"
    assert result["level"] == "error"
    assert result["message"]["text"]
    loc = result["locations"][0]["physicalLocation"]
    assert loc["artifactLocation"]["uri"]
    assert "region" in loc


def test_to_sarif_region_is_one_based_not_lsp_zero_based():
    # Diagnostic.line/col are already 1-based; SARIF region must reuse
    # them AS-IS (spec is 1-based), the opposite of to_machine_json.
    diag = _diag(line=5, col=11, span=3)
    result = to_sarif([diag])["runs"][0]["results"][0]
    region = result["locations"][0]["physicalLocation"]["region"]
    assert region["startLine"] == 5
    assert region["startColumn"] == 11
    assert region["endColumn"] == 14  # col + span


def test_to_sarif_level_mapping_for_all_severities():
    diags = [
        _diag(severity="error", code="ALP-B001"),
        _diag(severity="warning", code="ALP-B010"),
        _diag(severity="note", code="ALP-B099"),
    ]
    results = to_sarif(diags)["runs"][0]["results"]
    levels = {r["ruleId"]: r["level"] for r in results}
    assert levels == {"ALP-B001": "error", "ALP-B010": "warning", "ALP-B099": "note"}


def test_to_sarif_deduplicates_rules_by_code():
    diags = [_diag(code="ALP-B005"), _diag(code="ALP-B005"), _diag(code="ALP-B010")]
    rules = to_sarif(diags)["runs"][0]["tool"]["driver"]["rules"]
    assert sorted(r["id"] for r in rules) == ["ALP-B005", "ALP-B010"]


# ---------------------------------------------------------------------------
# human render() regression lock -- MUST be byte-identical to before #610.
# ---------------------------------------------------------------------------


def test_human_render_output_is_unchanged_snapshot():
    src = (
        "som:\n"
        "  sku: E1M-AEN801\n"
        "preset: e1m-evk\n"
        "peripherals:\n"
        "  - { pad: P21, signal: I2C0_SCL }\n"
    )
    diag = Diagnostic(
        severity="error",
        path=Path("board.yaml"),
        line=5,
        col=11,
        span=3,
        code="ALP-B005",
        message="pad 'P21' not present on E1M-AEN801",
        hint="did you mean 'P20'? (closest match, distance 1)",
        doc_url=None,
    )
    out = render(diag, source_text=src, color=False)
    expected = (
        "error[ALP-B005]: pad 'P21' not present on E1M-AEN801\n"
        "  --> board.yaml:5:11\n"
        "   |\n"
        " 5 |   - { pad: P21, signal: I2C0_SCL }\n"
        "   |           ^^^\n"
        "   = hint: did you mean 'P20'? (closest match, distance 1)\n"
        "   = see: docs/diagnostics/ALP-B005.md\n"
    )
    assert out == expected


# ---------------------------------------------------------------------------
# CLI wiring: `alp validate --format json|sarif`
# ---------------------------------------------------------------------------


def _run_alp_validate(path: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = str(REPO / "scripts")
    return subprocess.run(
        [sys.executable, "-m", "alp_cli.main", "validate", str(path), *args],
        cwd=REPO, env=env, capture_output=True, text=True, check=False,
    )


def test_cli_format_json_emits_conformant_document_on_stdout():
    fixture = FIX_BAD / "ALP-B005-bad-sku.yaml"
    proc = _run_alp_validate(fixture, "--format", "json")
    doc = json.loads(proc.stdout)
    _validator().validate(doc)
    assert any(d["code"] == "ALP-B005" for d in doc["diagnostics"])
    # stdout carries ONLY the JSON document -- no interleaved human prose.
    assert "error[ALP-B005]" not in proc.stdout


def test_cli_format_sarif_emits_valid_sarif_on_stdout():
    fixture = FIX_BAD / "ALP-B005-bad-sku.yaml"
    proc = _run_alp_validate(fixture, "--format", "sarif")
    doc = json.loads(proc.stdout)
    assert doc["version"] == "2.1.0"
    results = doc["runs"][0]["results"]
    assert any(r["ruleId"] == "ALP-B005" for r in results)
    assert "error[ALP-B005]" not in proc.stdout


def test_cli_default_format_is_human():
    fixture = FIX_BAD / "ALP-B005-bad-sku.yaml"
    proc = _run_alp_validate(fixture, "--no-color")
    assert "error[ALP-B005]" in proc.stdout
    with pytest.raises(json.JSONDecodeError):
        json.loads(proc.stdout)

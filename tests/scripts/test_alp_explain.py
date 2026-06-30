"""Tests for `alp explain` (scripts/alp_cli/explain.py)."""

import json

from click.testing import CliRunner

from alp_cli.main import cli


def test_explain_known_api_error():
    result = CliRunner().invoke(
        cli, ["explain", "ALP_ERR_NO_BACKEND", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "ALP_ERR_NO_BACKEND" in result.output
    assert "api-error" in result.output
    assert "no blob for any backend" in result.output


def test_explain_is_case_insensitive():
    result = CliRunner().invoke(
        cli, ["explain", "alp_err_no_backend", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "ALP_ERR_NO_BACKEND" in result.output


def test_explain_accepts_diagnostic_code_shape():
    result = CliRunner().invoke(cli, ["explain", "alp-b003", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "ALP-B003" in result.output
    assert "runtime-diagnostic" in result.output


def test_explain_json_shape_is_valid():
    result = CliRunner().invoke(
        cli, ["explain", "ALP_ERR_NO_BACKEND", "--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert payload["code"] == "ALP_ERR_NO_BACKEND"
    assert payload["kind"] == "api-error"
    assert payload["doc"] == "include/alp/peripheral.h"
    assert payload["summary"]


def test_explain_unknown_code_is_a_clear_miss():
    result = CliRunner().invoke(
        cli, ["explain", "ALP_ERR_NO_BACKENDD", "--no-color"])
    assert result.exit_code != 0
    assert "unknown code" in result.output
    # The closest real code is suggested.
    assert "ALP_ERR_NO_BACKEND" in result.output


def test_explain_unknown_code_json():
    result = CliRunner().invoke(cli, ["explain", "ZZZ", "--json"])
    assert result.exit_code != 0
    payload = json.loads(result.output)
    assert payload["error"] == "unknown-code"
    assert payload["code"] == "ZZZ"
    assert isinstance(payload["suggestions"], list)


def test_explain_registered_in_top_level_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "explain" in result.output

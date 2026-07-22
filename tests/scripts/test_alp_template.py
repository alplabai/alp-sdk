# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/alp_template.py -- the deterministic render/preview/
validate engine for metadata/templates/catalog-v1.json (epic #610 SS3
follow-up to scripts/check_template_catalog.py, tested in
test_check_template_catalog.py).

Covers: faithful byte-identical copy of `files.user_owned`, exclusion of
`files.generated`, determinism across repeated renders, the dry-run
preview writing nothing, the non-empty-dest/--force contract, parameter
validation (including a synthetic substitution fixture -- no shipped
catalog parameter declares a substitution target today, see
alp_template.py's module docstring), and the temp-dir twister validate()
gate.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

import alp_template  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
HELLO_WORLD = REPO / "examples" / "peripheral-io" / "hello-world"


def _catalog() -> dict:
    return alp_template.load_catalog()


def _minimal_record() -> dict:
    return alp_template.find_template(_catalog(), "minimal")


# --------------------------------------------------------------------------
# render(): faithful copy of files.user_owned, files.generated excluded
# --------------------------------------------------------------------------

def test_render_minimal_writes_every_user_owned_file(tmp_path):
    dest = tmp_path / "out"
    result = alp_template.render("minimal", dest)

    record = _minimal_record()
    expected = sorted(record["files"]["user_owned"])
    assert list(result.files) == expected
    for rel in expected:
        assert (dest / rel).is_file(), rel


def test_render_minimal_is_byte_identical_to_the_example(tmp_path):
    dest = tmp_path / "out"
    result = alp_template.render("minimal", dest)

    for rel in result.files:
        assert (dest / rel).read_bytes() == (HELLO_WORLD / rel).read_bytes(), rel


def test_render_never_copies_generated_files(tmp_path):
    dest = tmp_path / "out"
    alp_template.render("minimal", dest)

    record = _minimal_record()
    for gen in record["files"]["generated"]:
        # Generated paths are build-tree paths (e.g. build/generated/alp.conf)
        # -- assert none of it exists anywhere under dest.
        assert not (dest / gen["path"]).exists(), gen["path"]
    assert not (dest / "build").exists()


# --------------------------------------------------------------------------
# Determinism
# --------------------------------------------------------------------------

def test_render_is_deterministic_across_two_calls(tmp_path):
    dest_a = tmp_path / "a"
    dest_b = tmp_path / "b"
    result_a = alp_template.render("minimal", dest_a)
    result_b = alp_template.render("minimal", dest_b)

    assert result_a.files == result_b.files
    for rel in result_a.files:
        assert (dest_a / rel).read_bytes() == (dest_b / rel).read_bytes(), rel


# --------------------------------------------------------------------------
# Dry-run / preview
# --------------------------------------------------------------------------

def test_dry_run_writes_nothing(tmp_path):
    dest = tmp_path / "out"
    result = alp_template.render("minimal", dest, dry_run=True)

    assert not dest.exists()
    record = _minimal_record()
    assert list(result.files) == sorted(record["files"]["user_owned"])


def test_dry_run_and_real_render_report_the_same_file_list(tmp_path):
    dest = tmp_path / "out"
    preview = alp_template.render("minimal", dest, dry_run=True)
    real = alp_template.render("minimal", dest)
    assert preview.files == real.files


# --------------------------------------------------------------------------
# Non-empty dest / --force
# --------------------------------------------------------------------------

def test_render_refuses_nonempty_dest_without_force(tmp_path):
    dest = tmp_path / "out"
    alp_template.render("minimal", dest)

    with pytest.raises(alp_template.DestinationNotEmptyError):
        alp_template.render("minimal", dest)


def test_render_force_overwrites_nonempty_dest(tmp_path):
    dest = tmp_path / "out"
    alp_template.render("minimal", dest)
    (dest / "stray.txt").write_text("leftover", encoding="utf-8")

    result = alp_template.render("minimal", dest, force=True)
    assert list(result.files) == sorted(_minimal_record()["files"]["user_owned"])
    # force overwrites the declared files; it does not need to prune
    # unrelated leftovers, so just confirm the declared files landed clean.
    for rel in result.files:
        assert (dest / rel).read_bytes() == (HELLO_WORLD / rel).read_bytes(), rel


def test_render_into_empty_existing_dir_is_fine(tmp_path):
    dest = tmp_path / "out"
    dest.mkdir()
    result = alp_template.render("minimal", dest)
    assert list(result.files) == sorted(_minimal_record()["files"]["user_owned"])


# --------------------------------------------------------------------------
# Parameter validation
# --------------------------------------------------------------------------

def test_unknown_template_id_raises():
    with pytest.raises(alp_template.TemplateNotFoundError):
        alp_template.render("does-not-exist", Path("/tmp/wherever"), dry_run=True)


def test_unknown_parameter_name_raises(tmp_path):
    with pytest.raises(alp_template.ParameterError):
        alp_template.render("minimal", tmp_path / "out", {"nope": "x"}, dry_run=True)


def test_enum_parameter_out_of_constraint_raises(tmp_path):
    with pytest.raises(alp_template.ParameterError):
        alp_template.render(
            "peripheral", tmp_path / "out",
            {"button_pin": "NOT_A_REAL_PIN"}, dry_run=True)


def test_integer_parameter_below_minimum_raises(tmp_path):
    with pytest.raises(alp_template.ParameterError):
        alp_template.render(
            "multicore-rpmsg", tmp_path / "out",
            {"rpmsg_carve_out_kb": "1"}, dry_run=True)


def test_minimal_has_no_declared_parameters_so_it_is_a_pure_copy():
    assert _minimal_record()["parameters"] == []


# --------------------------------------------------------------------------
# Parameter substitution -- synthetic fixture.
#
# No parameter in the SHIPPED catalog declares a substitution target
# today (metadata/schemas/template-catalog-v1.schema.json's `parameter`
# def is additionalProperties: false), so every real template is a
# faithful copy regardless of --param overrides (covered above). This
# fixture builds its own catalog + example tree under tmp_path (never
# touching the real repo) to exercise the `substitute` codepath itself.
# --------------------------------------------------------------------------

def _write_fixture_catalog(root: Path) -> Path:
    example_rel = "examples/fixture/knob-app"
    example_dir = root / example_rel
    (example_dir / "src").mkdir(parents=True)
    (example_dir / "board.yaml").write_text(
        "knob: 42\nother: unrelated\n", encoding="utf-8")
    (example_dir / "src" / "main.c").write_text(
        "/* knob value: 42 */\nint knob = 42;\n", encoding="utf-8")

    catalog = {
        "schemaVersion": 1,
        "description": "test fixture catalog",
        "templates": [
            {
                "id": "knob-app",
                "title": "Knob App",
                "archetype": "minimal",
                "example": example_rel,
                "description": "fixture",
                "supported": {
                    "families": ["alif-ensemble"],
                    "som_skus": ["E1M-AEN801"],
                    "core_classes": ["m"],
                    "runtimes": ["zephyr"],
                },
                "requires": {
                    "portable_apis": [],
                    "libraries": [],
                    "chips": [],
                    "routes": [],
                    "generated_artifacts": [],
                    "test_backend": ["native_sim"],
                },
                "files": {
                    "user_owned": ["board.yaml", "src/main.c"],
                    "generated": [],
                },
                "parameters": [
                    {
                        "name": "knob",
                        "type": "integer",
                        "description": "fixture knob",
                        "default": 42,
                        "constraints": {"minimum": 1},
                        "substitute": {"file": "board.yaml", "literal": "42"},
                    }
                ],
                "test": {
                    "testcase_yaml": [],
                    "native_sim_scenarios": [],
                    "cross_compile_matrix": [],
                },
                "status": "preview",
                "note": "fixture only, not a real template",
            }
        ],
    }
    catalog_path = root / "catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
    return catalog_path


def test_parameter_substitution_applies_override_to_the_declared_file(tmp_path):
    catalog_path = _write_fixture_catalog(tmp_path)
    dest = tmp_path / "rendered"

    result = alp_template.render(
        "knob-app", dest, {"knob": "99"},
        catalog_path=catalog_path, base_dir=tmp_path)

    assert (dest / "board.yaml").read_text(encoding="utf-8") == "knob: 99\nother: unrelated\n"
    # Only the file the parameter's `substitute.file` names is touched.
    assert (dest / "src" / "main.c").read_text(encoding="utf-8") == (
        tmp_path / "examples" / "fixture" / "knob-app" / "src" / "main.c"
    ).read_text(encoding="utf-8")
    assert result.substitutions == (("knob", "42", "99"),)


def test_parameter_substitution_is_noop_when_value_equals_default(tmp_path):
    catalog_path = _write_fixture_catalog(tmp_path)
    dest = tmp_path / "rendered"

    result = alp_template.render(
        "knob-app", dest, catalog_path=catalog_path, base_dir=tmp_path)

    assert (dest / "board.yaml").read_text(encoding="utf-8") == "knob: 42\nother: unrelated\n"
    assert result.substitutions == ()


def test_no_shipped_template_declares_a_substitution_target():
    """Locks in the module docstring's claim: today every real catalog
    parameter is inert (no `substitute` key), so real templates never
    take this codepath."""
    for rec in _catalog()["templates"]:
        for spec in rec["parameters"]:
            assert "substitute" not in spec, (rec["id"], spec["name"])


# --------------------------------------------------------------------------
# render_to_envelope() -- --emit scaffold's in-memory capture (issue #864)
# --------------------------------------------------------------------------

def test_render_to_envelope_is_passthrough_for_the_examples_own_sku():
    """E1M-AEN801 is hello-world's own board.yaml `som.sku:` -- rendering
    for that SKU must be byte-identical to the example, same as
    render()."""
    envelope = alp_template.render_to_envelope("minimal", "E1M-AEN801")
    record = _minimal_record()
    assert [p for p, _ in envelope] == sorted(record["files"]["user_owned"])
    for rel, contents in envelope:
        assert contents == (HELLO_WORLD / rel).read_text(encoding="utf-8"), rel


def test_render_to_envelope_substitutes_sku_and_preset():
    envelope = alp_template.render_to_envelope("minimal", "E1M-V2N101")
    by_path = dict(envelope)

    board_yaml = by_path["board.yaml"]
    assert "sku: E1M-V2N101" in board_yaml
    assert "sku: E1M-AEN801" not in board_yaml
    assert "preset: e1m-x-evk" in board_yaml

    # Every other user_owned file is an unmodified copy.
    for rel in ("prj.conf", "CMakeLists.txt", "src/main.c", "testcase.yaml"):
        assert by_path[rel] == (HELLO_WORLD / rel).read_text(encoding="utf-8"), rel


def test_render_to_envelope_rejects_unsupported_sku():
    with pytest.raises(alp_template.SkuNotSupportedError, match="FOO"):
        alp_template.render_to_envelope("minimal", "FOO")


def test_render_to_envelope_unknown_template_raises():
    with pytest.raises(alp_template.TemplateNotFoundError):
        alp_template.render_to_envelope("does-not-exist", "E1M-AEN801")


def test_render_to_envelope_matches_render_for_the_default_sku(tmp_path):
    """render_to_envelope() and render() share `_rendered_bytes()` --
    for the example's own SKU they must produce identical bytes."""
    dest = tmp_path / "out"
    alp_template.render("minimal", dest)
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    for rel in envelope:
        assert (dest / rel).read_text(encoding="utf-8") == envelope[rel], rel


# --------------------------------------------------------------------------
# validate() -- temp-dir render + twister
# --------------------------------------------------------------------------

@pytest.mark.skipif(
    not os.environ.get("ZEPHYR_BASE"),
    reason="ZEPHYR_BASE not set; validate() needs a real Zephyr checkout")
def test_validate_minimal_passes_via_twister():
    result = alp_template.validate("minimal")

    assert not result.skipped
    assert result.passed, (result.returncode, result.stdout, result.stderr)
    assert result.passed_count >= 1
    # The temp dir must be cleaned up afterwards.
    assert result.tmp_dir is not None
    assert not Path(result.tmp_dir).exists()


def test_validate_skips_cleanly_without_zephyr_base():
    result = alp_template.validate("minimal", zephyr_base="")
    assert result.skipped
    assert "ZEPHYR_BASE" in result.reason

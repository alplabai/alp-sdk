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
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

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
    """E1M-AEN801 is hello-world's own board.yaml `som.sku:` -- board.yaml/
    prj.conf/src/main.c stay byte-identical to the example (the app core
    is unchanged too: `_derive_core_renames` is a no-op for the canonical
    sku). CMakeLists.txt/README.md are scaffold-adapted regardless of sku
    (issue #864 follow-up -- see test_render_to_envelope_scaffold_adapts_
    cmakelists_and_readme below); `testcase.yaml` is never in the
    envelope at all (dropped from `files.user_owned`: SDK CI wiring, not
    a user's project file)."""
    envelope = alp_template.render_to_envelope("minimal", "E1M-AEN801")
    record = _minimal_record()
    by_path = dict(envelope)
    assert [p for p, _ in envelope] == sorted(record["files"]["user_owned"])
    assert "testcase.yaml" not in by_path
    for rel in ("board.yaml", "prj.conf", "src/main.c"):
        assert by_path[rel] == (HELLO_WORLD / rel).read_text(encoding="utf-8"), rel
    assert "--core m55_hp" in by_path["CMakeLists.txt"]
    assert "ALP_SDK_ROOT is not set" in by_path["CMakeLists.txt"]


def test_render_to_envelope_substitutes_sku_and_preset():
    envelope = alp_template.render_to_envelope("minimal", "E1M-V2N101")
    by_path = dict(envelope)

    board_yaml = by_path["board.yaml"]
    assert "sku: E1M-V2N101" in board_yaml
    assert "sku: E1M-AEN801" not in board_yaml
    assert "preset: e1m-x-evk" in board_yaml
    # The AEN-only app core (m55_hp, an Alif-only Zephyr cluster) is
    # re-derived to E1M-V2N101's own Zephyr core -- issue #864 follow-up
    # blocker: the pre-fix scaffold baked `cores: m55_hp:` in unchanged,
    # which `alp_project.py --emit zephyr-conf --core m55_hp` rejects
    # against a V2N101 board.yaml ("unknown core id").
    assert "m33_sm:" in board_yaml
    assert "m55_hp" not in board_yaml
    assert "--core m33_sm" in by_path["CMakeLists.txt"]
    assert "--core m55_hp" not in by_path["CMakeLists.txt"]

    # prj.conf / src/main.c carry no sku-specific content -- unmodified.
    for rel in ("prj.conf", "src/main.c"):
        assert by_path[rel] == (HELLO_WORLD / rel).read_text(encoding="utf-8"), rel


def test_render_to_envelope_drops_stale_trailing_comment_on_value_change():
    """Fable review finding: `gpio-button-led`'s board.yaml carries an
    inline comment on BOTH its `sku:` and `preset:` lines describing the
    AEN801/e1m-evk default (`# Alif Ensemble E8 SoM`, `# 35x35 EVK --
    reference board...`). Substituting the VALUE must drop that comment
    too -- leaving it would mislabel a V2N101 scaffold as Alif hardware."""
    envelope = dict(alp_template.render_to_envelope("peripheral", "E1M-V2N101"))
    board_yaml = envelope["board.yaml"]
    assert "sku: E1M-V2N101" in board_yaml
    assert "Alif Ensemble E8 SoM" not in board_yaml
    assert "E1M-AEN801" not in board_yaml
    assert "preset: e1m-x-evk" in board_yaml
    assert "35x35 EVK" not in board_yaml


def test_render_to_envelope_preserves_trailing_comment_when_value_unchanged():
    """The flip side of the above: requesting the example's OWN sku is a
    byte-passthrough, comment included (already covered end-to-end by
    test_render_to_envelope_is_passthrough_for_the_examples_own_sku for
    `minimal`; this pins it for a record whose lines DO carry inline
    comments)."""
    example = REPO / "examples" / "peripheral-io" / "gpio-button-led"
    envelope = dict(alp_template.render_to_envelope("peripheral", "E1M-AEN801"))
    assert envelope["board.yaml"] == (example / "board.yaml").read_text(encoding="utf-8")


# --------------------------------------------------------------------------
# Content adaptation: CMakeLists.txt / README.md (issue #864 follow-up --
# scaffold-flavours these regardless of `sku`, since the SDK-tree-relative
# `ALP_SDK_ROOT` guess and `../`-relative links/self-paths are wrong for a
# scaffold copied out of the SDK tree no matter which sku was requested).
# --------------------------------------------------------------------------

def test_scaffold_cmakelists_requires_alp_sdk_root_explicitly():
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    cmakelists = envelope["CMakeLists.txt"]
    assert "if(DEFINED ENV{ALP_SDK_ROOT})" not in cmakelists
    assert "if(NOT DEFINED ENV{ALP_SDK_ROOT})" in cmakelists
    assert "FATAL_ERROR" in cmakelists
    assert "get_filename_component(ALP_SDK_ROOT" not in cmakelists


def test_scaffold_cmakelists_hardens_the_hardcoded_variant_too():
    """cold-chain-monitor's CMakeLists.txt has NO ALP_SDK_ROOT env-var
    fallback at all -- a hardcoded `${CMAKE_CURRENT_SOURCE_DIR}/../../../
    scripts/alp_project.py` -- worse than the guess-block variant since
    there's no override at all. `_scaffold_cmakelists` must harden this
    shape too."""
    envelope = dict(alp_template.render_to_envelope("edge-ai", "E1M-AEN801"))
    cmakelists = envelope["CMakeLists.txt"]
    assert "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py" not in cmakelists
    assert "${ALP_SDK_ROOT}/scripts/alp_project.py" in cmakelists
    assert "if(NOT DEFINED ENV{ALP_SDK_ROOT})" in cmakelists


def test_scaffold_readme_has_no_dangling_sdk_tree_links_or_self_path():
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    readme = envelope["README.md"]
    assert "examples/peripheral-io/hello-world" not in readme
    assert "../../../docs/" not in readme
    assert "https://github.com/alplabai/alp-sdk/blob/main/docs/" in readme


def test_scaffold_readme_rewrites_sibling_example_links_too():
    """i2c-master's README links a SIBLING example (`../i2c-scanner/`),
    equally dangling once copied out as a standalone scaffold -- not just
    the `../../../docs/...` case."""
    envelope = dict(alp_template.render_to_envelope("sensor", "E1M-AEN801"))
    readme = envelope["README.md"]
    assert "../i2c-scanner/" not in readme
    assert "https://github.com/alplabai/alp-sdk/tree/main/examples/peripheral-io/i2c-scanner" in readme


def test_substitute_board_yaml_sku_rejects_ambiguous_sku_line():
    """More than one line matching the `sku:` pattern is unresolvable --
    which one is the real `som.sku:`? -- so it must hard-error rather
    than silently rewrite the first match and leave a decoy (or the
    real line) untouched."""
    text = "decoy:\n  sku: E1M-AEN801\nsom:\n  sku: E1M-AEN801\npreset: e1m-evk\n"
    with pytest.raises(alp_template.TemplateError, match="exactly one"):
        alp_template._substitute_board_yaml_sku(text, "E1M-V2N101", "e1m-x-evk")


def test_substitute_board_yaml_sku_rejects_ambiguous_preset_line():
    text = "som:\n  sku: E1M-AEN801\npreset: e1m-evk\npreset: e1m-evk\n"
    with pytest.raises(alp_template.TemplateError, match="exactly one"):
        alp_template._substitute_board_yaml_sku(text, "E1M-AEN801", "e1m-x-evk")


# --------------------------------------------------------------------------
# Every catalog template x its supported.som_skus (Fable review: only
# minimal x {AEN801, V2N101} was covered before -- the first untested
# combo, peripheral x V2N101, surfaced the stale-comment MAJOR above).
# --------------------------------------------------------------------------

def _every_template_sku_pair() -> list[tuple[str, str]]:
    return [
        (rec["id"], sku)
        for rec in _catalog()["templates"]
        for sku in rec["supported"]["som_skus"]
    ]


# Combos whose canonical example's board.yaml top-level `pins:` block
# hardcodes an E1M-EVK-only pad NAME AND NUMBER -- not a mechanical
# `_X`-insertion, e.g. the encoder push switch is E1M_GPIO_IO4 on
# E1M-EVK vs E1M_X_GPIO_IO28 on E1M-X-EVK (metadata/boards/e1m-evk.yaml
# vs e1m-x-evk.yaml `e1m_routes:`, keyed only by the shared
# `board_alias:`, e.g. BOARD_PIN_ENCODER_SW) -- a separate, PRE-EXISTING
# example-portability gap this issue's scaffold-adaptation (core /
# CMakeLists.txt / README.md / testcase.yaml) doesn't touch: these
# templates' `pins:` blocks were only ever validated cross-family via a
# native_sim COMPILER DEFINE (testcase.yaml's `-DALP_BOARD_E1M_X_EVK`),
# never by literally swapping som.sku the way `--emit scaffold --sku`
# does. Verified via `alp_project.py --emit zephyr-conf --core <id>`;
# left as a follow-up (would need a `pins:` board_alias cross-reference
# analogous to `_derive_core_renames`, out of scope for #864's core
# blocker). Mirrors check_template_catalog.py's own KNOWN_GAP_EXAMPLES
# allowlist pattern for a tracked, non-hidden content gap.
_KNOWN_PINS_PORTABILITY_GAP = {
    ("peripheral", "E1M-V2N101"),
    ("sensor", "E1M-V2N101"),
    ("edge-ai", "E1M-V2N101"),
}


@pytest.mark.parametrize("template_id,sku", _every_template_sku_pair())
def test_render_to_envelope_every_template_sku_combo(template_id, sku, tmp_path):
    record = alp_template.find_template(_catalog(), template_id)
    example = REPO / record["example"]
    example_board_yaml = (example / "board.yaml").read_text(encoding="utf-8")
    example_doc = yaml.safe_load(example_board_yaml)
    example_sku = example_doc["som"]["sku"]

    board_yaml = dict(alp_template.render_to_envelope(template_id, sku))["board.yaml"]
    parsed = yaml.safe_load(board_yaml)

    assert parsed["som"]["sku"] == sku
    som_doc = yaml.safe_load(
        (alp_template.METADATA_ROOT / "e1m_modules" / f"{sku}.yaml")
        .read_text(encoding="utf-8"))
    assert parsed["preset"] == som_doc["default_board"].lower()

    if example_sku != sku:
        assert example_sku not in board_yaml

    # Every core id `cores:` declares must be valid for `sku`'s own SoM
    # topology -- the #864 follow-up blocker regression test: VERIFIED
    # bug was `alp_project.py --input <board.yaml> --emit zephyr-conf
    # --core m55_hp` against an E1M-V2N101 board.yaml => rc=1, "unknown
    # core id ... did you mean ['a55_cluster', 'm33_sm']".
    topology = som_doc.get("topology") or {}
    declared_cores = list(parsed["cores"].keys())
    assert set(declared_cores) <= set(topology), (template_id, sku)

    board_yaml_path = tmp_path / "board.yaml"
    board_yaml_path.write_text(board_yaml, encoding="utf-8")
    for core_id in declared_cores:
        if not core_id.startswith("m"):
            continue  # a-class clusters are Yocto/`os: off`, not `--core`-buildable
        proc = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml_path),
             "--emit", "zephyr-conf", "--core", core_id],
            capture_output=True, text=True, cwd=REPO, check=False)
        if (template_id, sku) in _KNOWN_PINS_PORTABILITY_GAP:
            assert proc.returncode != 0
            assert "e1m_routes" in proc.stderr, (template_id, sku, proc.stderr)
            continue
        assert proc.returncode == 0, (template_id, sku, core_id, proc.stderr)
        assert "unknown core id" not in proc.stderr


def test_render_to_envelope_rejects_unsupported_sku():
    with pytest.raises(alp_template.SkuNotSupportedError, match="FOO"):
        alp_template.render_to_envelope("minimal", "FOO")


def test_render_to_envelope_unknown_template_raises():
    with pytest.raises(alp_template.TemplateNotFoundError):
        alp_template.render_to_envelope("does-not-exist", "E1M-AEN801")


def test_render_to_envelope_matches_render_for_the_default_sku(tmp_path):
    """render_to_envelope() and render() share `_rendered_bytes()` --
    for the example's own SKU, the files with no scaffold-specific
    content adaptation (board.yaml/prj.conf/src/main.c) must be
    identical bytes; render() stays byte-for-byte faithful to the real
    example (that's what validate()'s twister run proves builds), while
    CMakeLists.txt/README.md diverge -- render_to_envelope() scaffold-
    adapts those regardless of sku (see the content-adaptation tests
    above), and testcase.yaml isn't part of the envelope at all."""
    dest = tmp_path / "out"
    alp_template.render("minimal", dest)
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    assert "testcase.yaml" not in envelope
    for rel in ("board.yaml", "prj.conf", "src/main.c"):
        assert (dest / rel).read_text(encoding="utf-8") == envelope[rel], rel
    assert (dest / "CMakeLists.txt").read_text(encoding="utf-8") != envelope["CMakeLists.txt"]


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

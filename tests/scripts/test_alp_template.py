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


def test_substitute_board_yaml_sku_drops_stale_trailing_comment_on_value_change():
    """Fable review finding: `gpio-button-led`'s board.yaml carries an
    inline comment on BOTH its `sku:` and `preset:` lines describing the
    AEN801/e1m-evk default (`# Alif Ensemble E8 SoM`, `# 35x35 EVK --
    reference board...`). Substituting the VALUE must drop that comment
    too -- leaving it would mislabel a V2N101 scaffold as Alif hardware.

    Exercised directly against `_substitute_board_yaml_sku` (not
    `render_to_envelope("peripheral", "E1M-V2N101")`): that combo is no
    longer in `supported.som_skus` (issue #876 -- peripheral's `pins:`
    block isn't E1M-X-EVK-portable yet), but the sku/preset-comment
    regex behavior this pins doesn't depend on the catalog's supported-
    sku list at all."""
    example = REPO / "examples" / "peripheral-io" / "gpio-button-led"
    text = (example / "board.yaml").read_text(encoding="utf-8")
    board_yaml = alp_template._substitute_board_yaml_sku(text, "E1M-V2N101", "e1m-x-evk")
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
    assert "if(NOT DEFINED ALP_SDK_ROOT AND NOT DEFINED ENV{ALP_SDK_ROOT})" in cmakelists
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
    assert "if(NOT DEFINED ALP_SDK_ROOT AND NOT DEFINED ENV{ALP_SDK_ROOT})" in cmakelists


def test_scaffold_readme_has_no_dangling_sdk_tree_links_or_self_path():
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    readme = envelope["README.md"]
    ref = alp_template._docs_ref(alp_template.REPO)
    assert "examples/peripheral-io/hello-world" not in readme
    assert "../../../docs/" not in readme
    assert f"https://github.com/alplabai/alp-sdk/blob/{ref}/docs/" in readme


def test_scaffold_readme_rewrites_sibling_example_links_too():
    """i2c-master's README links a SIBLING example (`../i2c-scanner/`),
    equally dangling once copied out as a standalone scaffold -- not just
    the `../../../docs/...` case."""
    envelope = dict(alp_template.render_to_envelope("sensor", "E1M-AEN801"))
    readme = envelope["README.md"]
    ref = alp_template._docs_ref(alp_template.REPO)
    assert "../i2c-scanner/" not in readme
    assert (f"https://github.com/alplabai/alp-sdk/tree/{ref}"
            "/examples/peripheral-io/i2c-scanner") in readme


def test_scaffold_readme_extra_zephyr_modules_uses_alp_sdk_root_not_pwd():
    """issue #864 Fable-review MAJOR B: `$(pwd)` only equals the alp-sdk
    checkout root when building IN-TREE; a copied-out scaffold's cwd is
    the scaffold dir, so the alp-sdk Zephyr module (providing
    `CONFIG_ALP_*` / `<alp/*.h>`) never registers and the documented
    `west build` fails."""
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    readme = envelope["README.md"]
    assert "$(pwd)" not in readme
    assert "-DEXTRA_ZEPHYR_MODULES=$ALP_SDK_ROOT" in readme


def test_scaffold_readme_rewrites_board_target_and_som_label_for_cross_family_sku():
    """issue #864 Fable-review MAJOR C: the canonical example's own SoM
    label ("# Example for E1M-AEN801:") and qualified Zephyr board
    target (bare `alp_e1m_aen801_m55_hp`) used to survive a cross-family
    sku swap untouched -- the real E1M-V2N101 label/board target
    appeared nowhere in a V2N101 scaffold."""
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-V2N101"))
    readme = envelope["README.md"]
    assert "E1M-AEN801" not in readme
    assert "alp_e1m_aen801_m55_hp" not in readme
    assert "# Example for E1M-V2N101:" in readme
    assert "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33" in readme


def test_scaffold_readme_upgrades_bare_board_id_even_for_the_passthrough_sku():
    """The canonical example's own README hardcodes the BARE (non-
    qualified) board id, which Zephyr 4.4 can't actually resolve on a
    multi-cluster SoC (issue #720) -- `_scaffold_readme` upgrades it to
    the fully-qualified id even when `sku` is the example's own (a
    bonus correctness fix riding along with MAJOR C's mechanism)."""
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-AEN801"))
    readme = envelope["README.md"]
    assert "west build -b alp_e1m_aen801_m55_hp " not in readme
    assert "alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp" in readme


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
# _derive_core_renames -- app-core candidate selection (issue #864
# Fable-review MAJOR D)
# --------------------------------------------------------------------------

def test_derive_core_renames_picks_the_real_app_core_not_alphabetical_first():
    """E1M-AEN801's `topology:` declares `m55_hp` (the real app core)
    BEFORE `m55_he` (a stock-shim peer core it inherits by default) --
    `m55_he` sorts first alphabetically. Before this fix,
    `_derive_core_renames(["m33_sm"], "E1M-AEN801", ...)` resolved
    `m55_he`: topology-valid (so the blind board-yaml-vs-topology
    subset check couldn't catch it), but the WRONG core -- unreachable
    today (no template's `supported.som_skus` exercises a V2N-canonical
    template swapping onto AEN801), but silently wrong the day one
    does."""
    renames = alp_template._derive_core_renames(
        ["m33_sm"], "E1M-AEN801", alp_template.METADATA_ROOT)
    assert renames == {"m33_sm": "m55_hp"}


# --------------------------------------------------------------------------
# _derive_pin_renames -- cross-EVK pad correspondence via `board_alias:`
# (issue #876: re-adds E1M-V2N101 to peripheral/sensor/edge-ai's
# `supported.som_skus`, dropped as a stopgap by #864/#877)
# --------------------------------------------------------------------------

def test_derive_pin_renames_maps_e1m_evk_pads_to_e1m_x_evk_pads():
    """The three `board_alias:` roles peripheral/sensor exercise --
    metadata/boards/e1m-evk.yaml and metadata/boards/e1m-x-evk.yaml
    both declare the SAME `board_alias:` for the encoder-switch
    button, the red-LED PWM pad, and the sensor I2C bus, so each
    resolves to its E1M-X-EVK counterpart pad."""
    renames = alp_template._derive_pin_renames(
        ["E1M_GPIO_IO4", "E1M_GPIO_PWM3"], "E1M-V2N101", "e1m-evk",
        alp_template.METADATA_ROOT)
    assert renames == {
        "E1M_GPIO_IO4": "E1M_X_GPIO_IO28",
        "E1M_GPIO_PWM3": "E1M_X_GPIO_PWM5",
    }
    assert alp_template._derive_pin_renames(
        ["E1M_I2C0"], "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT
    ) == {"E1M_I2C0": "E1M_X_I2C0"}


def test_derive_pin_renames_is_a_passthrough_for_the_examples_own_family():
    """`sku`'s own default board preset IS `source_preset` (E1M-AEN801
    on its own e1m-evk canonical example) -- byte-identical
    passthrough, nothing to rewrite."""
    assert alp_template._derive_pin_renames(
        ["E1M_GPIO_IO4"], "E1M-AEN801", "e1m-evk", alp_template.METADATA_ROOT
    ) == {}


def test_derive_pin_renames_rejects_a_pad_with_no_board_alias():
    """`E1M_GPIO_IO2` (EVK_PIN_CAM_MUX_SEL) carries no `board_alias:`
    on e1m-evk -- no cross-EVK correspondence declared for that role at
    all, so re-deriving it for a different SoM family is a hard error,
    not a silent best-effort guess."""
    with pytest.raises(alp_template.TemplateError, match="board_alias"):
        alp_template._derive_pin_renames(
            ["E1M_GPIO_IO2"], "E1M-V2N101", "e1m-evk",
            alp_template.METADATA_ROOT)


def test_derive_pin_macro_renames_matches_the_pad_renames():
    """The `macro:` field a `pins:` entry carries alongside `e1m:` must
    be re-derived too -- `alp_orchestrate.loader
    ._validate_topology_cores`'s `pins:` cross-check hard-errors on a
    declared `macro:` that doesn't match the resolved board's own
    macro for the (renamed) pad, not just an unrecognised pad."""
    pins = [
        {"e1m": "E1M_GPIO_IO4", "macro": "EVK_PIN_ENCODER_SW"},
        {"e1m": "E1M_GPIO_PWM3", "macro": "EVK_PIN_LED_RED"},
    ]
    renames = alp_template._derive_pin_macro_renames(
        pins, "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT)
    assert renames == {
        "EVK_PIN_ENCODER_SW": "XEVK_PIN_ENCODER_SW",
        "EVK_PIN_LED_RED": "XEVK_PIN_LED_RED",
    }


# --------------------------------------------------------------------------
# _strip_stale_core_prose -- stale-core-mentioning comments (issue #864
# Fable-review MINOR F)
# --------------------------------------------------------------------------

def test_substitute_board_yaml_core_strips_stale_core_prose_comment():
    """gpio-button-led's board.yaml carries `# Single-core slice:
    M55-HP runs the demo.  M55-HE inherits...` directly above `cores:`
    -- prose naming the OLD core in a different case/hyphenation than
    the YAML key (`M55-HP` vs `m55_hp`), which the key-line rename
    regex alone never touches. Exercised directly (against a swap
    `render_to_envelope("peripheral", ...)` no longer needs, now that
    E1M-V2N101 is back in that template's `supported.som_skus`, issue
    #876) since the prose-stripping behavior doesn't depend on the
    catalog's supported-sku list."""
    example = REPO / "examples" / "peripheral-io" / "gpio-button-led"
    text = (example / "board.yaml").read_text(encoding="utf-8")
    assert "M55-HP" in text  # sanity: the fixture actually has stale prose
    rewritten = alp_template._substitute_board_yaml_core(text, "m55_hp", "m33_sm")
    assert "M55-HP" not in rewritten
    assert "m33_sm:" in rewritten


# --------------------------------------------------------------------------
# _ALP_SDK_ROOT_REQUIRED_BLOCK -- -D vs ENV{} precedence (issue #864
# Fable-review MAJOR E)
# --------------------------------------------------------------------------

def test_alp_sdk_root_required_block_checks_both_d_and_env_and_prefers_d():
    """The FATAL_ERROR guard must only fire when NEITHER the `-D` cache
    variable NOR the env var is set (the message advertises both), and
    the value-assignment must not clobber an already-set `-D` with the
    env var."""
    block = alp_template._ALP_SDK_ROOT_REQUIRED_BLOCK
    assert "if(NOT DEFINED ALP_SDK_ROOT AND NOT DEFINED ENV{ALP_SDK_ROOT})" in block
    assert "if(NOT DEFINED ALP_SDK_ROOT)\n    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})" in block


# --------------------------------------------------------------------------
# render(..., sku=...) / default_sku() -- `alp generate`'s other scaffold
# front door now agrees with `alp emit scaffold` (issue #864 Fable-review
# MINOR G)
# --------------------------------------------------------------------------

def test_default_sku_is_the_examples_own_som_sku():
    assert alp_template.default_sku(_minimal_record()) == "E1M-AEN801"


def test_render_with_sku_matches_render_to_envelope(tmp_path):
    dest = tmp_path / "out"
    alp_template.render("minimal", dest, sku="E1M-V2N101")
    envelope = dict(alp_template.render_to_envelope("minimal", "E1M-V2N101"))
    for rel, contents in envelope.items():
        assert (dest / rel).read_text(encoding="utf-8") == contents, rel


def test_render_with_sku_rejects_unsupported_sku_before_touching_disk(tmp_path):
    dest = tmp_path / "out"
    with pytest.raises(alp_template.SkuNotSupportedError):
        alp_template.render("minimal", dest, sku="FOO")
    assert not dest.exists()


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


@pytest.mark.parametrize("template_id,sku", _every_template_sku_pair())
def test_render_to_envelope_every_template_sku_combo(template_id, sku, tmp_path):
    """Every REMAINING (template, sku) combo the catalog declares
    supported must emit a board.yaml `alp_project.py --emit zephyr-conf`
    actually accepts -- issue #864 Fable-review MAJOR A: `peripheral`/
    `sensor`/`edge-ai`'s `pins:` blocks hardcode E1M-EVK-only pad names
    (e.g. `E1M_GPIO_IO4`, not the E1M-X-EVK equivalent `E1M_X_GPIO_IO28`
    -- not a mechanical `_X` insertion, tracked as issue #876), so
    E1M-V2N101 was DROPPED from those three templates'
    `supported.som_skus` rather than shipping a scaffold that silently
    exits 0 while emitting content `--emit zephyr-conf` then rejects --
    see test_render_to_envelope_rejects_the_dropped_pins_gap_combos
    below for that rejection. Every combo parametrized here is now
    expected to fully succeed."""
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

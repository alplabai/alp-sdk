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
import re
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


def _no_paragraph_break_between(text: str, start_marker: str, end_marker: str) -> bool:
    """True iff the span from `start_marker` to `end_marker` has no blank
    line and no HTML-block opener (`<...`, optionally indented 0-3 spaces
    per CommonMark's rule for HTML blocks types 1-6 -- a `<!--` comment is
    type 2) between them. This is a targeted proxy for the #1794 defect
    shape, NOT a general CommonMark block-boundary oracle: it does not
    detect the other paragraph-interrupting constructs (ATX headings,
    thematic breaks, fenced code, block quotes, list markers, setext
    underlines) -- those are out of scope here. A stray HTML-block opener
    at column 0-3 is how issue #1794 silently split a sentence into two
    `<p>` tags; a 4-space indent is an indented code block and does NOT
    interrupt a paragraph, so it must NOT trip this check. `markdown_it`
    is not a declared project dependency (not in
    pyproject.toml/scripts/requirements.txt, so not installed by the CI
    `pip install -e ".[dev,model-compile]"` step that runs `pytest
    tests/scripts/`) -- this stdlib-only check pins the same fact without
    adding one."""
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    between = text[start + len(start_marker):end]
    if "\n\n" in between:
        return False
    return not any(re.match(r" {0,3}<", line) for line in between.split("\n"))


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
# Path containment (#1126) -- a catalog-declared files.user_owned entry
# must never let render()/render_to_envelope() read outside the example
# root or write outside the destination root, whether it tries via `..`
# traversal, an absolute path, or a symlink placed inside either root.
# `_safe_join` backs BOTH the read site (_rendered_bytes) and the write
# site (render()'s write loop), so testing it directly covers both; the
# render()-level test below additionally proves the wiring end-to-end.
# --------------------------------------------------------------------------

def test_safe_join_allows_a_normal_in_root_path(tmp_path):
    root = tmp_path / "root"
    (root / "sub").mkdir(parents=True)
    (root / "sub" / "file.txt").write_text("ok", encoding="utf-8")
    assert alp_template._safe_join(root, "sub/file.txt", what="x") == (
        root / "sub" / "file.txt").resolve()


def test_safe_join_rejects_parent_traversal(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    (tmp_path / "secret.txt").write_text("outside", encoding="utf-8")
    with pytest.raises(alp_template.PathEscapeError):
        alp_template._safe_join(root, "../secret.txt", what="x")


def test_safe_join_rejects_absolute_path(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    with pytest.raises(alp_template.PathEscapeError):
        alp_template._safe_join(root, "/etc/passwd", what="x")


@pytest.mark.skipif(
    sys.platform == "win32", reason="os.symlink needs elevated privileges on Windows")
def test_safe_join_rejects_symlink_escape(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "secret.txt").write_text("outside", encoding="utf-8")
    (root / "escape_link").symlink_to(outside, target_is_directory=True)
    with pytest.raises(alp_template.PathEscapeError):
        alp_template._safe_join(root, "escape_link/secret.txt", what="x")


def _write_escape_catalog(root: Path, user_owned: list[str]) -> Path:
    """Same shape as `_write_fixture_catalog`, minus the substitution
    parameter -- lets each test declare its own (possibly malicious)
    `files.user_owned` list without a real example tree behind it."""
    example_rel = "examples/fixture/escape-app"
    example_dir = root / example_rel
    example_dir.mkdir(parents=True)
    (example_dir / "board.yaml").write_text("name: escape\n", encoding="utf-8")

    catalog = {
        "schemaVersion": 1,
        "description": "test fixture catalog",
        "templates": [
            {
                "id": "escape-app",
                "title": "Escape App",
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
                "files": {"user_owned": user_owned, "generated": []},
                "parameters": [],
                "test": {
                    "testcase_yaml": [f"{example_rel}/testcase.yaml"],
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


def test_render_valid_in_root_user_owned_file_still_renders(tmp_path):
    catalog_path = _write_escape_catalog(tmp_path, ["board.yaml"])
    dest = tmp_path / "out"
    result = alp_template.render(
        "escape-app", dest, catalog_path=catalog_path, base_dir=tmp_path)
    assert result.files == ("board.yaml",)
    assert (dest / "board.yaml").read_text(encoding="utf-8") == "name: escape\n"


def test_render_rejects_traversal_in_user_owned_path(tmp_path):
    (tmp_path / "secret.txt").write_text("outside", encoding="utf-8")
    catalog_path = _write_escape_catalog(tmp_path, ["../../../secret.txt"])
    with pytest.raises(alp_template.PathEscapeError):
        alp_template.render(
            "escape-app", tmp_path / "out",
            catalog_path=catalog_path, base_dir=tmp_path)


def test_render_rejects_absolute_path_in_user_owned(tmp_path):
    catalog_path = _write_escape_catalog(tmp_path, ["/etc/passwd"])
    with pytest.raises(alp_template.PathEscapeError):
        alp_template.render(
            "escape-app", tmp_path / "out",
            catalog_path=catalog_path, base_dir=tmp_path)


def test_render_rejects_traversal_in_the_example_root_itself(tmp_path):
    """Adversarial-review follow-up (#1126, blocker 1): the catalog's
    `example` field is JUST AS untrusted as `files.user_owned` -- the
    first pass wired `_safe_join()` around the RELATIVE part of every
    join (`files.user_owned` entries) but still trusted `example` itself
    verbatim (`base_dir / record["example"]`), so a catalog naming
    `"example": "../outside"` walked the containment ROOT out of
    `base_dir` and the per-file check then faithfully confined the read
    to that escaped root -- containing nothing. Mirrors the reviewer's
    production PoC (`base_dir == REPO`, `example` walking out to
    `/tmp/X/outside`) with a hermetic `base_dir` instead."""
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "secret.txt").write_text("SECRET\n", encoding="utf-8")

    base_dir = tmp_path / "base"
    base_dir.mkdir()

    catalog = {
        "templates": [{
            "id": "escape-root",
            "example": "../outside",
            "files": {"user_owned": ["secret.txt"], "generated": []},
            "parameters": [],
            "test": {"testcase_yaml": []},
        }],
    }
    catalog_path = tmp_path / "catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    dest = tmp_path / "dest"
    with pytest.raises(alp_template.PathEscapeError):
        alp_template.render(
            "escape-root", dest, catalog_path=catalog_path, base_dir=base_dir)

    # Pre-fix, this is exactly the reviewer's PoC: render() returns clean
    # and dest/secret.txt contains the exfiltrated file. It must not exist.
    assert not dest.exists()


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
    """`_scaffold_cmakelists`' SECOND shape: a hardcoded
    `${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py` with
    NO ALP_SDK_ROOT resolution at all -- worse than the guess block,
    since no override is even possible.

    Issue #1390 gave cold-chain-monitor (the `edge-ai` template's
    source) a real guess block, so NO example carries this shape any
    more and the catalog can no longer reach this branch -- drive it
    from a literal instead of asserting it via a render that now takes
    the other path.
    """
    hardcoded = (
        "# SPDX-License-Identifier: Apache-2.0\n"
        "cmake_minimum_required(VERSION 3.20)\n"
        "\n"
        "execute_process(\n"
        "    COMMAND ${Python3_EXECUTABLE}\n"
        "            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py\n"
        ")\n"
    )
    out = alp_template._scaffold_cmakelists(hardcoded)
    assert "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py" not in out
    assert "${ALP_SDK_ROOT}/scripts/alp_project.py" in out
    assert "if(NOT DEFINED ALP_SDK_ROOT AND NOT DEFINED ENV{ALP_SDK_ROOT})" in out


# --------------------------------------------------------------------------
# The comment ABOVE the block has to move with it (issue #1390 review
# blocker 2): every scaffold-source example but two introduces the guess
# block with prose teaching the in-tree `../../..` fallback, which the
# hardened block deliberately drops.  Substituting only the code shipped a
# scaffold whose comment documented behaviour the emitted file did not have.
# --------------------------------------------------------------------------

@pytest.mark.parametrize(
    "template,sku",
    [
        ("minimal", "E1M-V2N101"),
        ("peripheral", "E1M-V2N101"),
        ("sensor", "E1M-V2N101"),
        ("edge-ai", "E1M-V2N101"),
        ("multicore-mailbox", "E1M-AEN801"),
    ],
)
def test_scaffold_cmakelists_never_documents_the_dropped_fallback(template, sku):
    """No emitted CMakeLists.txt may promise the in-tree fallback."""
    envelope = dict(alp_template.render_to_envelope(template, sku))
    for rel, text in envelope.items():
        if not rel.endswith("CMakeLists.txt"):
            continue
        lowered = text.lower()
        assert "grandparent" not in lowered, rel
        assert "in-tree" not in lowered, rel


def test_scaffold_cmakelists_keeps_unrelated_comment_paragraphs():
    """Only the paragraph describing ALP_SDK_ROOT resolution is
    rewritten. gpio-button-led (the `peripheral` template's source)
    leads its comment run with a banner -- "board.yaml ->
    build/generated/alp.conf at configure time." -- that stays true for
    a scaffold and must survive verbatim."""
    envelope = dict(alp_template.render_to_envelope("peripheral", "E1M-V2N101"))
    cmakelists = envelope["CMakeLists.txt"]
    assert "# board.yaml -> build/generated/alp.conf at configure time." in cmakelists
    assert "# Resolve the alp-sdk root." in cmakelists
    # ... and exactly once -- a second matching paragraph is dropped,
    # never duplicated.
    assert cmakelists.count("# Resolve the alp-sdk root.") == 1


def test_scaffold_cmakelists_invents_no_prose_where_there_was_none():
    """i2c-master (the `sensor` template's source) has NO comment above
    its guess block. The rewrite is a rewrite, not an insertion: it must
    not grow prose the example never had."""
    envelope = dict(alp_template.render_to_envelope("sensor", "E1M-V2N101"))
    cmakelists = envelope["CMakeLists.txt"]
    assert "# Resolve the alp-sdk root." not in cmakelists
    assert "if(NOT DEFINED ALP_SDK_ROOT AND NOT DEFINED ENV{ALP_SDK_ROOT})" in cmakelists


def test_scaffold_cmakelists_leaves_a_detached_comment_run_alone():
    """The rewrite is scoped to the run IMMEDIATELY above the block.
    mproc-mailbox's `peer/CMakeLists.txt` opens with a file-header
    comment separated from the block by `cmake_minimum_required(...)`;
    it is not the block's prose and must survive untouched."""
    envelope = dict(alp_template.render_to_envelope(
        "multicore-mailbox", "E1M-AEN801"))
    peer = envelope["peer/CMakeLists.txt"]
    assert "# HE-side peer image for the mproc-mailbox flagship." in peer
    assert "# Resolve the alp-sdk root." not in peer


def _fake_sdk_checkout(root, version, status, tags=()):
    """A minimal git checkout carrying `metadata/sdk_version.yaml` and
    `tags` -- enough for `_docs_ref` to read and for `git rev-parse` to
    answer against. One empty commit, because a tag needs an object."""
    (root / "metadata").mkdir(parents=True)
    (root / "metadata" / "sdk_version.yaml").write_text(
        f"version: {version}\nstatus:  {status}\n", encoding="utf-8")
    env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
    subprocess.run(["git", "init", "-q", str(root)], check=True, env=env)
    subprocess.run(["git", "-C", str(root), "commit", "-q", "--allow-empty", "-m", "x"],
                   check=True, env=env)
    for tag in tags:
        subprocess.run(["git", "-C", str(root), "tag", tag], check=True, env=env)
    return root


def test_docs_ref_falls_back_to_main_when_the_declared_tag_does_not_exist(tmp_path):
    """#1508. Between an rc cut and its GA tag `metadata/sdk_version.yaml`
    declares `version: 0.16.0` / `status: released` while only
    `v0.16.0-rc1` exists, and pinning on that declared pair alone put
    three unresolvable `blob/v0.16.0/docs/...` links in every scaffolded
    README for the whole window (six days, for v0.15.0).

    The rc tag being present is the point: this is not "no tags at all",
    it is the exact shape that fooled the old check.
    """
    root = _fake_sdk_checkout(tmp_path / "rc", "0.16.0", "released", tags=("v0.16.0-rc1",))
    assert alp_template._docs_ref(root) == "main"


def test_docs_ref_pins_the_tag_once_it_actually_resolves(tmp_path):
    """The other direction, so the fix cannot degrade into "always main"
    -- that would silently drop the stable-docs pin #864 added."""
    root = _fake_sdk_checkout(tmp_path / "ga", "0.16.0", "released",
                              tags=("v0.16.0-rc1", "v0.16.0"))
    assert alp_template._docs_ref(root) == "v0.16.0"


def test_docs_ref_is_main_for_a_development_checkout(tmp_path):
    """Unchanged pre-#1508 behaviour: a non-`released` status never pins,
    tag present or not."""
    root = _fake_sdk_checkout(tmp_path / "dev", "0.17.0", "development", tags=("v0.17.0",))
    assert alp_template._docs_ref(root) == "main"


def test_docs_ref_is_main_outside_a_git_checkout(tmp_path):
    """A tarball export or `--no-tags` clone has the metadata but no refs.
    `_tag_resolves` must degrade to `main`, never raise -- an exception
    here would abort the whole scaffold over a README link."""
    root = tmp_path / "tarball"
    (root / "metadata").mkdir(parents=True)
    (root / "metadata" / "sdk_version.yaml").write_text(
        "version: 0.16.0\nstatus:  released\n", encoding="utf-8")
    assert alp_template._docs_ref(root) == "main"


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


def test_scaffold_readme_cold_chain_models_link_survives_scaffolding():
    """Issues #1688/#1749: cold-chain-monitor's README deliberately links
    `../cold-chain-monitor/models/README.md` -- climbing out of the
    example dir and back in -- because `_RELATIVE_LINK_RE` only matches
    `../`-prefixed links and `models/README.md` is a CHILD of the example
    dir, not a sibling. A future edit that "tidies" the link to the more
    natural `](models/README.md)` would stop matching the rewriter
    entirely and ship a dangling relative link in every scaffold; assert
    on the EMITTED output, not the source text, so this catches that.

    Also pins issue #1798's rendering regression: a URL substring alone
    survives even when an explanatory HTML comment sitting at column 0
    silently splits the "No model is shipped ... See [link]" sentence
    into two paragraphs, so also assert the lead-in and the link render
    in the SAME CommonMark block."""
    envelope = dict(alp_template.render_to_envelope("edge-ai", "E1M-AEN801"))
    readme = envelope["README.md"]
    ref = alp_template._docs_ref(alp_template.REPO)
    assert (f"https://github.com/alplabai/alp-sdk/blob/{ref}"
            "/examples/ai/cold-chain-monitor/models/README.md") in readme
    assert _no_paragraph_break_between(
        readme, "No model is shipped", "[`models/README.md`](")


def test_scaffold_readme_mqtt_native_sim_conf_link_survives_scaffolding():
    """Issue #1794: mqtt-telemetry's README deliberately links
    `../mqtt-telemetry/native_sim.conf` -- climbing out of the example
    dir and back in -- because `_RELATIVE_LINK_RE` only matches
    `../`-prefixed links and `native_sim.conf` is a CHILD of the example
    dir, not a sibling. A future edit that "tidies" the link to the more
    natural `](native_sim.conf)` would stop matching the rewriter
    entirely and ship a dangling relative link in every scaffold; assert
    on the EMITTED output, not the source text, so this catches that.

    Also pins issue #1798's rendering regression: a URL substring alone
    survives even when an explanatory HTML comment sitting at column 0
    silently splits the "turns mbedtls off (see [link])" sentence into
    two paragraphs, so also assert the lead-in and the link render in
    the SAME CommonMark block."""
    envelope = dict(alp_template.render_to_envelope("iot", "E1M-AEN801"))
    readme = envelope["README.md"]
    ref = alp_template._docs_ref(alp_template.REPO)
    assert (f"https://github.com/alplabai/alp-sdk/blob/{ref}"
            "/examples/connectivity/mqtt-telemetry/native_sim.conf") in readme
    assert _no_paragraph_break_between(
        readme, "turns mbedtls off (see", "[`native_sim.conf`](")


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


def test_scaffold_readme_rewrites_bare_mention_alongside_qualified_one():
    """Issue #1266 review MINOR: a README naming the source board BOTH
    ways -- a qualified `west build` line AND a separate bare mention
    (e.g. lvgl-widgets-demo's README:36 `west build -b
    alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp` vs :59's bare
    "target board (`alp_e1m_aen801_m55_hp`)") -- used to only get the
    qualified one rewritten; the bare one silently kept naming the
    source family inside a cross-family scaffold."""
    source_board = "alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp"
    target_board = "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"
    text = (
        "west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp ex\n"
        "west flash\n"
        "\n"
        "this app's target board (`alp_e1m_aen801_m55_hp`) has no alias yet\n"
    )
    out = alp_template._scaffold_readme(
        text, "examples/display/widget", "main",
        source_board=source_board, target_board=target_board,
    )
    assert "alp_e1m_aen801_m55_hp" not in out
    assert out.count(target_board) == 2


def test_scaffold_readme_passthrough_does_not_duplicate_qualified_suffix():
    """The same-sku (source_board == target_board) case must not run the
    short-prefix fallback over a mention the exact-match step already
    left correctly qualified -- that would append the `/<soc>/<core>`
    suffix a second time (`.../rtss_hp/ae822fa0e5597ls0/rtss_hp`)."""
    board = "alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp"
    text = f"west build -b {board} examples/display/widget\n"
    out = alp_template._scaffold_readme(
        text, "examples/display/widget", "main",
        source_board=board, target_board=board,
    )
    assert out.count("ae822fa0e5597ls0/rtss_hp") == 1


def test_scaffold_readme_rewrites_west_flash_after_every_m33_sm_board_line():
    """A two-core V2N/V2M scaffold README can carry more than one
    `<board target>\\nwest flash` pair (one per core) -- every one of
    them needs `--host <board-ip>` appended, not just the first."""
    target_board = "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"
    text = (
        f"west build -b {target_board} ex/core0\n"
        "west flash\n"
        "\n"
        f"west build -b {target_board} ex/core1\n"
        "west flash\n"
    )
    out = alp_template._scaffold_readme(
        text, "examples/multicore/widget", "main",
        source_board="alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp",
        target_board=target_board,
    )
    assert out.count("west flash --host <board-ip>") == 2
    assert "\nwest flash\n" not in out


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
# render_to_envelope's per-core CMakeLists.txt map (issue #1275 item 1) --
# synthetic fixture, since no SHIPPED template today needs a cross-family
# rename on a dual-Zephyr-core template (E1M-AEN801 is the only SKU with
# two Zephyr M cores in the whole catalog, and every dual-Zephyr-core
# template only supports that one SKU -- see multicore-mailbox's
# `supported.som_skus`). Before this fix, ONE re-derived rename
# (`app_core_sub`, keyed off the first m-prefixed core) was applied to
# EVERY `*CMakeLists.txt` file a template owned; on a second Zephyr core
# whose CMakeLists.txt carries a DIFFERENT `--core` literal, that either
# silently mismatched (a wrong `--core` value written) or, as here,
# hard-failed with "must have exactly one `--core <old>`... found 0"
# because the literal being searched for isn't the one that file has.
# --------------------------------------------------------------------------

def _write_dual_core_fixture(root: Path) -> tuple[Path, Path]:
    """A minimal two-Zephyr-core template (root `CMakeLists.txt` baking
    `--core m55_hp`, `peer/CMakeLists.txt` baking `--core m55_he`) plus a
    metadata_root with two SoM presets: the example's own (SRC, same core
    ids) and a target (DST) whose topology renames BOTH cores to
    DIFFERENT ids -- so a correct fix must apply the RIGHT rename to the
    RIGHT file, not one rename to both. Returns (catalog_path, metadata_root)."""
    example_dir = root / "examples" / "fixture" / "dual-core-app"
    (example_dir / "src").mkdir(parents=True)
    (example_dir / "peer").mkdir(parents=True)
    (example_dir / "board.yaml").write_text(
        "som:\n"
        "  sku: E1M-SRCTEST\n"
        "preset: src-preset\n"
        "cores:\n"
        "  m55_hp:\n"
        "    app: ./src\n"
        "  m55_he:\n"
        "    app: ./peer\n",
        encoding="utf-8", newline="\n",
    )
    (example_dir / "src" / "main.c").write_text("/* hp */\n", encoding="utf-8")
    (example_dir / "peer" / "main.c").write_text("/* he */\n", encoding="utf-8")
    (example_dir / "CMakeLists.txt").write_text(
        "# fixture\nalp_project.py --emit zephyr-conf --core m55_hp\n",
        encoding="utf-8", newline="\n",
    )
    (example_dir / "peer" / "CMakeLists.txt").write_text(
        "# fixture\nalp_project.py --emit zephyr-conf --core m55_he\n",
        encoding="utf-8", newline="\n",
    )

    metadata_root = root / "metadata"
    (metadata_root / "e1m_modules").mkdir(parents=True)
    (metadata_root / "e1m_modules" / "E1M-SRCTEST.yaml").write_text(
        "default_board: SRC-PRESET\n"
        "topology:\n"
        "  m55_hp:\n"
        "    board: src_board/soc/m55_hp\n"
        "  m55_he:\n"
        "    board: src_board/soc/m55_he\n",
        encoding="utf-8", newline="\n",
    )
    (metadata_root / "e1m_modules" / "E1M-DSTTEST.yaml").write_text(
        "default_board: DST-PRESET\n"
        "topology:\n"
        "  mX:\n"
        "    board: dst_board/soc/mX\n"
        "  mY:\n"
        "    board: dst_board/soc/mY\n",
        encoding="utf-8", newline="\n",
    )

    catalog = {
        "schemaVersion": 1,
        "description": "test fixture catalog",
        "templates": [
            {
                "id": "dual-core-app",
                "title": "Dual Core App",
                "archetype": "multicore-mailbox",
                "example": "examples/fixture/dual-core-app",
                "description": "fixture",
                "supported": {
                    "families": ["alif-ensemble"],
                    "som_skus": ["E1M-SRCTEST", "E1M-DSTTEST"],
                    "core_classes": ["m"],
                    "runtimes": ["zephyr"],
                },
                "cores": [
                    {"id": "m55_hp", "dir": "./src", "os": "zephyr"},
                    {"id": "m55_he", "dir": "./peer", "os": "zephyr"},
                ],
                "requires": {
                    "portable_apis": [], "libraries": [], "chips": [],
                    "routes": [], "generated_artifacts": [],
                    "test_backend": ["native_sim"],
                },
                "files": {
                    "user_owned": [
                        "board.yaml", "CMakeLists.txt", "src/main.c",
                        "peer/CMakeLists.txt", "peer/main.c",
                    ],
                    "generated": [],
                },
                "parameters": [],
                "test": {
                    "testcase_yaml": [], "native_sim_scenarios": [],
                    "cross_compile_matrix": [],
                },
                "status": "preview",
                "note": "fixture only, not a real template",
            }
        ],
    }
    catalog_path = root / "catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
    return catalog_path, metadata_root


def test_render_to_envelope_renames_each_zephyr_cores_own_cmakelists(tmp_path):
    catalog_path, metadata_root = _write_dual_core_fixture(tmp_path)

    envelope = dict(alp_template.render_to_envelope(
        "dual-core-app", "E1M-DSTTEST",
        catalog_path=catalog_path, base_dir=tmp_path, metadata_root=metadata_root))

    # Each file's OWN core got its OWN rename -- not one rename smeared
    # across both files (which would leave one wrong, or -- as it did
    # before this fix -- raise TemplateError on the second file instead).
    assert "--core mX" in envelope["CMakeLists.txt"]
    assert "m55_hp" not in envelope["CMakeLists.txt"]
    assert "--core mY" in envelope["peer/CMakeLists.txt"]
    assert "m55_he" not in envelope["peer/CMakeLists.txt"]


def test_render_to_envelope_passthrough_keeps_each_cores_own_literal(tmp_path):
    """Same fixture, requesting the example's OWN sku (no rename at
    all): both CMakeLists.txt must stay byte-identical to the source,
    each keeping ITS OWN core's literal -- never swapped."""
    catalog_path, metadata_root = _write_dual_core_fixture(tmp_path)

    envelope = dict(alp_template.render_to_envelope(
        "dual-core-app", "E1M-SRCTEST",
        catalog_path=catalog_path, base_dir=tmp_path, metadata_root=metadata_root))

    assert "--core m55_hp" in envelope["CMakeLists.txt"]
    assert "--core m55_he" in envelope["peer/CMakeLists.txt"]


def test_cmake_core_map_rejects_traversal_in_core_dir(tmp_path):
    """A catalog `cores[].dir` of `../x` must be rejected via the same
    resolve-then-contain guard (#1126) every other catalog-sourced path
    in this file uses -- `_cmake_core_map` used to hand `core["dir"]`
    straight to `_zephyr_app_dir` (no containment check of its own) and
    then `.relative_to(example_dir)`, which raises a bare ValueError
    instead of PathEscapeError/TemplateError when the dir escapes."""
    example_dir = tmp_path / "examples" / "fixture" / "escape-app"
    example_dir.mkdir(parents=True)
    record = {
        "cores": [
            {"id": "m55_hp", "dir": "../escape", "os": "zephyr"},
        ],
    }
    with pytest.raises(alp_template.PathEscapeError):
        alp_template._cmake_core_map(record, example_dir)


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
        ["E1M_GPIO_IO4", "E1M_GPIO_PWM0"], "E1M-V2N101", "e1m-evk",
        alp_template.METADATA_ROOT)
    assert renames == {
        "E1M_GPIO_IO4": "E1M_X_GPIO_IO28",
        "E1M_GPIO_PWM0": "E1M_X_GPIO_PWM5",
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
# Adversarial review follow-up (issue #876 review MAJOR 1/2, MINOR 3/4)
# --------------------------------------------------------------------------

def test_derive_pin_renames_multi_alias_pad_resolves_via_macro_not_dict_inversion():
    """e1m-evk's `E1M_PWM1` carries TWO `board_alias:` roles at two
    DIFFERENT entries -- `BOARD_PWM_LED_BLUE` (macro
    `EVK_PWM_LED_BLUE`) and `BOARD_PWM_ARD1` (macro `EVK_ARD_PWM1`).
    A naive `{pad: alias}` dict inversion collapses to whichever entry
    wins the dict (last-in-iteration-order), silently ignoring the
    pin's own `macro:` -- verified pre-fix: resolved to the Arduino
    pad regardless of which macro the pin actually declared. Matching
    by `macro:` first must resolve `EVK_PWM_LED_BLUE` to the LED-blue
    counterpart, `E1M_X_PWM6` (`XEVK_PWM_LED_BLUE`), never the
    Arduino pad `E1M_X_PWM1`."""
    pins = [{"e1m": "E1M_PWM1", "macro": "EVK_PWM_LED_BLUE"}]
    pad_renames = alp_template._derive_pin_renames(
        pins, "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT)
    assert pad_renames == {"E1M_PWM1": "E1M_X_PWM6"}
    macro_renames = alp_template._derive_pin_macro_renames(
        pins, "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT)
    assert macro_renames == {"EVK_PWM_LED_BLUE": "XEVK_PWM_LED_BLUE"}

    # The OTHER macro on the same shared pad must resolve to ITS OWN
    # (different) target -- proves the fix isn't just "always pick the
    # first entry sharing the pad."
    ard_pins = [{"e1m": "E1M_PWM1", "macro": "EVK_ARD_PWM1"}]
    assert alp_template._derive_pin_renames(
        ard_pins, "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT
    ) == {"E1M_PWM1": "E1M_X_PWM1"}


def test_derive_pin_renames_bare_string_multi_alias_pad_hard_errors():
    """A bare pad-string `pins:` entry (no `macro:` to disambiguate)
    naming a multi-alias pad (`E1M_PWM1`) has nothing to resolve the
    alias with -- hard error, never a silent guess at which of its two
    roles was meant (issue #876 review MINOR 3)."""
    with pytest.raises(alp_template.TemplateError, match="unambiguous"):
        alp_template._derive_pin_renames(
            ["E1M_PWM1"], "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT)


def test_derive_pin_doc_renames_copies_the_target_boards_own_doc():
    """A renamed pin's `doc:` must be re-derived to the TARGET route's
    own `doc:` (issue #876 review MAJOR 2) -- otherwise it keeps
    describing the SOURCE board's electricals/part number, which is
    actively wrong once the physical pad has changed."""
    pins = [{
        "e1m": "E1M_GPIO_IO4", "macro": "EVK_PIN_ENCODER_SW",
        "doc": "Rotary encoder push switch (PEC12R-4222F-S0024), "
               "10k pull-up + 0.1uF debounce",
    }]
    renames = alp_template._derive_pin_doc_renames(
        pins, "E1M-V2N101", "e1m-evk", alp_template.METADATA_ROOT)
    assert renames == {
        "Rotary encoder push switch (PEC12R-4222F-S0024), 10k pull-up + "
        "0.1uF debounce":
            "Rotary encoder (PEC12R-4222F) push switch; pull-up + "
            "RC debounce.",
    }


# --------------------------------------------------------------------------
# _derive_pin_doc_renames collision guard (issue #1394): the two
# assignment sites had NO guard, unlike both siblings -- two `pins:`
# entries sharing one `doc:` string silently overwrote each other, and
# the `None` (DROP the field) branch made the loser lose its
# documentation entirely, with the winner decided by `pins:` ordering.
# Fixture: a synthetic metadata_root, because EVERY aliased route on the
# real metadata/boards/e1m-x-evk.yaml carries a `doc:` of its own, so
# the real tree cannot exercise the string-vs-`None` branch at all.
# --------------------------------------------------------------------------

def _write_shared_doc_fixture(root: Path, second_target_doc: str | None) -> Path:
    """A metadata_root whose SRC board routes two pads (`E1M_A`,
    `E1M_B`) that both carry a `board_alias:` onto a DST board. The
    DST route for `BOARD_A` always has its own `doc:`; the one for
    `BOARD_B` gets `second_target_doc` (a different string, or no
    `doc:` at all when `None`). Returns the metadata_root."""
    metadata_root = root / "metadata"
    (metadata_root / "e1m_modules").mkdir(parents=True)
    (metadata_root / "e1m_modules" / "E1M-SRCTEST.yaml").write_text(
        "default_board: SRC-PRESET\n", encoding="utf-8", newline="\n")
    (metadata_root / "e1m_modules" / "E1M-DSTTEST.yaml").write_text(
        "default_board: DST-PRESET\n", encoding="utf-8", newline="\n")

    (metadata_root / "boards").mkdir(parents=True)
    (metadata_root / "boards" / "src-preset.yaml").write_text(
        "e1m_routes:\n"
        "  gpio:\n"
        "    - e1m: E1M_A\n"
        "      macro: SRC_PIN_A\n"
        "      board_alias: BOARD_A\n"
        "    - e1m: E1M_B\n"
        "      macro: SRC_PIN_B\n"
        "      board_alias: BOARD_B\n",
        encoding="utf-8", newline="\n",
    )
    (metadata_root / "boards" / "dst-preset.yaml").write_text(
        "e1m_routes:\n"
        "  gpio:\n"
        "    - e1m: E1M_X_A\n"
        "      macro: DST_PIN_A\n"
        "      board_alias: BOARD_A\n"
        "      doc: Shared debounce network, DST pad A.\n"
        "    - e1m: E1M_X_B\n"
        "      macro: DST_PIN_B\n"
        "      board_alias: BOARD_B\n"
        + (f"      doc: {second_target_doc}\n" if second_target_doc else ""),
        encoding="utf-8", newline="\n",
    )
    return metadata_root


_SHARED_DOC = "Shared debounce network (10k + 0.1uF), SRC pads A and B."

_SHARED_DOC_PINS = [
    {"e1m": "E1M_A", "macro": "SRC_PIN_A", "doc": _SHARED_DOC},
    {"e1m": "E1M_B", "macro": "SRC_PIN_B", "doc": _SHARED_DOC},
]


def test_derive_pin_doc_renames_rejects_two_pins_sharing_a_doc(tmp_path):
    """Two `pins:` entries sharing one `doc:` string that re-derive to
    two DIFFERENT target strings are ambiguous for the flat
    `{old_doc: new_doc}` map `_substitute_board_yaml_pin_docs` applies
    file-wide -- hard error, exactly as `_derive_pin_renames` and
    `_derive_pin_macro_renames` already did for their own keys
    (issue #1394)."""
    metadata_root = _write_shared_doc_fixture(
        tmp_path, "Shared debounce network, DST pad B.")
    with pytest.raises(alp_template.TemplateError, match="ambiguous"):
        alp_template._derive_pin_doc_renames(
            _SHARED_DOC_PINS, "E1M-DSTTEST", "src-preset", metadata_root)


def test_derive_pin_doc_renames_rejects_a_shared_doc_one_side_drops(tmp_path):
    """The branch that lost data SILENTLY (issue #1394): one entry
    re-derives the shared `doc:` to a target string, the other's
    target route has no `doc:` at all -- `None`, which per this
    function's docstring means DROP the field. Pre-fix the second
    write won unguarded, so the map said `None` and the documentation
    was dropped from BOTH pins, including the one with a perfectly
    good target `doc:`; reverse the `pins:` ordering and the doc
    survived. "Rename it" and "drop it" are contradictory
    instructions for one key, so this is ambiguous too."""
    metadata_root = _write_shared_doc_fixture(tmp_path, None)
    with pytest.raises(alp_template.TemplateError, match="ambiguous"):
        alp_template._derive_pin_doc_renames(
            _SHARED_DOC_PINS, "E1M-DSTTEST", "src-preset", metadata_root)

    # ... and in the reverse `pins:` order too -- the whole point is
    # that the outcome must no longer depend on iteration order.
    with pytest.raises(alp_template.TemplateError, match="ambiguous"):
        alp_template._derive_pin_doc_renames(
            list(reversed(_SHARED_DOC_PINS)), "E1M-DSTTEST", "src-preset",
            metadata_root)


def test_derive_pin_doc_renames_allows_a_shared_doc_that_agrees(tmp_path):
    """A shared `doc:` is only ambiguous when the entries DISAGREE --
    two pins whose target routes carry the SAME `doc:` string yield
    one unambiguous rename, not an error."""
    metadata_root = _write_shared_doc_fixture(
        tmp_path, "Shared debounce network, DST pad A.")
    assert alp_template._derive_pin_doc_renames(
        _SHARED_DOC_PINS, "E1M-DSTTEST", "src-preset", metadata_root
    ) == {_SHARED_DOC: "Shared debounce network, DST pad A."}


def test_derive_pin_doc_renames_keeps_an_unchanged_doc_out_of_the_map(tmp_path):
    """A target `doc:` byte-identical to the entry's own contributes
    NO map entry -- there is nothing to rewrite. Guarding the two
    assignment sites must not fold that case into the `None` (DROP)
    value, which would delete a `doc:` that was already correct
    (the shape the issue's own proposed snippet had)."""
    metadata_root = _write_shared_doc_fixture(tmp_path, None)
    pins = [{
        "e1m": "E1M_A", "macro": "SRC_PIN_A",
        "doc": "Shared debounce network, DST pad A.",
    }]
    assert alp_template._derive_pin_doc_renames(
        pins, "E1M-DSTTEST", "src-preset", metadata_root) == {}


def test_derive_pin_doc_renames_rejects_a_shared_doc_kept_then_dropped(tmp_path):
    """The third contradiction #1394 closes: one entry's target `doc:`
    is byte-identical to the shared string ("keep it" -- no map entry
    at all), the other's target has none ("drop it"). Pre-fix the map
    said `None` unconditionally, so the pin whose doc was ALREADY
    correct lost it to the file-wide substitution with no diagnostic.
    Recording every resolution -- not only the ones that produce a
    rename -- is what makes this reachable."""
    metadata_root = _write_shared_doc_fixture(tmp_path, None)
    pins = [
        {"e1m": "E1M_A", "macro": "SRC_PIN_A",
         "doc": "Shared debounce network, DST pad A."},
        {"e1m": "E1M_B", "macro": "SRC_PIN_B",
         "doc": "Shared debounce network, DST pad A."},
    ]
    with pytest.raises(alp_template.TemplateError, match="ambiguous"):
        alp_template._derive_pin_doc_renames(
            pins, "E1M-DSTTEST", "src-preset", metadata_root)


def test_substitute_board_yaml_pins_rewrites_the_bare_string_list_item_form():
    """The schema also allows a bare pad-string `pins:` entry (no
    `{e1m: ...}` mapping) -- the dict-only `e1m:`-key regex left it
    stale (issue #876 review MINOR 3)."""
    text = "pins:\n  - E1M_I2C0\ncores:\n  m55_hp:\n    app: ./src\n"
    out = alp_template._substitute_board_yaml_pins(
        text, {"E1M_I2C0": "E1M_X_I2C0"}, ["E1M_I2C0"])
    assert "- E1M_X_I2C0" in out
    assert "E1M_I2C0" not in out.replace("E1M_X_I2C0", "")


def test_substitute_board_yaml_pins_mixed_bare_and_dict_for_same_pad():
    """A mixed bare-string + dict entry for the SAME pad must rewrite
    BOTH -- the dict match alone used to satisfy the old any-
    occurrence guard, silently hiding the still-stale bare entry
    (issue #876 review MINOR 3)."""
    text = (
        "pins:\n  - E1M_I2C0\n"
        "  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS }\n"
    )
    original_pins = ["E1M_I2C0", {"e1m": "E1M_I2C0", "macro": "EVK_I2C_BUS_SENSORS"}]
    out = alp_template._substitute_board_yaml_pins(
        text, {"E1M_I2C0": "E1M_X_I2C0"}, original_pins)
    assert out.count("E1M_X_I2C0") == 2
    assert "E1M_I2C0" not in out.replace("E1M_X_I2C0", "")


def test_substitute_readme_pins_skips_a_paragraph_already_naming_both_forms():
    """i2c-master's README already teaches the cross-EVK alias
    resolution explicitly ("resolves to `ALP_E1M_I2C0` on the E1M EVK
    and `ALP_E1M_X_I2C0` on the E1M-X EVK") -- correct, portable prose
    about the mechanism itself, not a stale claim about which pad THIS
    scaffold uses. Blindly substituting would turn it into a duplicate,
    factually wrong sentence (issue #876 review MINOR 4)."""
    text = (
        "The `<alp/board.h>` alias resolves to `ALP_E1M_I2C0` on the E1M\n"
        "EVK and `ALP_E1M_X_I2C0` on the E1M-X EVK.\n"
    )
    out = alp_template._substitute_readme_pins(text, {"E1M_I2C0": "E1M_X_I2C0"})
    assert out == text


def test_substitute_readme_pins_rewrites_a_single_board_prose_paragraph():
    """gpio-button-led's README teaches `ALP_E1M_GPIO_IO4` as THE
    button pin with no cross-EVK caveat -- must become the target
    pad, not survive as stale E1M-EVK-only prose."""
    text = "reads the switch on `ALP_E1M_GPIO_IO4` (active-low) as the button.\n"
    out = alp_template._substitute_readme_pins(
        text, {"E1M_GPIO_IO4": "E1M_X_GPIO_IO28"})
    assert "ALP_E1M_X_GPIO_IO28" in out
    assert "ALP_E1M_GPIO_IO4" not in out


def test_render_to_envelope_peripheral_v2n101_has_no_stale_e1m_evk_pad_mentions():
    """End-to-end: neither board.yaml nor README.md may still name an
    E1M-EVK-only pad once scaffold-adapted for E1M-V2N101 -- the exact
    bar the #876 adversarial review demanded."""
    envelope = dict(alp_template.render_to_envelope("peripheral", "E1M-V2N101"))
    for old in ("E1M_GPIO_IO4", "E1M_GPIO_PWM3"):
        assert old not in envelope["board.yaml"], envelope["board.yaml"]
        assert f"ALP_{old}" not in envelope["README.md"], envelope["README.md"]
    # `\b` (word-boundary) checks, not plain substring `in`: the correct
    # renamed macros (`XEVK_PIN_ENCODER_SW`/`XEVK_PIN_LED_RED`) legitimately
    # CONTAIN the stale macro names as a substring (X-prefixed).
    for old_macro in ("EVK_PIN_ENCODER_SW", "EVK_PIN_LED_RED"):
        assert not re.search(rf"\b{old_macro}\b", envelope["board.yaml"]), \
            (old_macro, envelope["board.yaml"])
    assert "PEC12R-4222F-S0024" not in envelope["board.yaml"]  # stale e1m-evk doc


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
# render(..., sku=...) / default_sku() -- tan-cli's scaffold front doors
# (`tan init`, `tan scaffold`) now agree with `west alp-emit scaffold`
# (issue #864 Fable-review MINOR G)
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


def test_validate_rejects_traversal_in_testcase_yaml(tmp_path, monkeypatch):
    """Adversarial-review follow-up (#1126, blocker 2): same class, same
    file, but the helper was never applied at this site. `validate()`
    derives `rel` from the catalog-controlled `test.testcase_yaml` by
    STRING-stripping the example prefix (`tc[len(example_prefix):]`),
    which preserves a `..` untouched, then joined it straight onto its
    own tmp dir with no containment check. The reviewer's PoC (`tc =
    "<example>/../ALP1126_LOOT.txt"`) lands a file as a *sibling* of the
    twister tmpdir -- outside it -- before validate() ever shells out to
    twister, let alone errors out. `zephyr_base` here is a nonexistent
    path: the write-side bug fires before validate() would ever reach a
    real twister invocation, so no real Zephyr checkout is needed to
    prove it."""
    controlled_tmp = tmp_path / "twister-tmp"
    monkeypatch.setattr(
        alp_template.tempfile, "mkdtemp", lambda *a, **k: str(controlled_tmp))

    example = "examples/peripheral-io/hello-world"
    catalog = {
        "templates": [{
            "id": "escape-testcase",
            "example": example,
            "files": {
                "user_owned": ["board.yaml", "prj.conf", "CMakeLists.txt",
                                "src/main.c", "README.md"],
                "generated": [],
            },
            "parameters": [],
            "test": {
                "testcase_yaml": [f"{example}/../hello-world/testcase.yaml"],
            },
        }],
    }
    catalog_path = tmp_path / "catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    with pytest.raises(alp_template.PathEscapeError):
        alp_template.validate(
            "escape-testcase", catalog_path=catalog_path,
            zephyr_base="/nonexistent-zephyr")

    # The escape target is a sibling of the (deleted) tmpdir -- must never
    # have been created.
    assert not (tmp_path / "hello-world").exists()

# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_library_registry.py (#1197).

The prior gate walked board.schema.json for a per-core `libraries.items.
enum` shape WS6-c retired -- that walk always found an empty set, so the
gate passed vacuously no matter what was actually wrong.  These tests prove
the rewritten gate is non-vacuous: it genuinely inspects the real board.yaml
corpus and the real manifest registry, and it fires on a deliberately
broken input.

Run locally:

    python -m pytest tests/scripts/test_check_library_registry.py -q
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "scripts"))
import check_library_registry as gate  # noqa: E402
from alp_orchestrate import validate as validate_mod  # noqa: E402

_ALIAS_SCHEMA_TEXT = (
    REPO / "metadata/schemas/library-aliases-v1.schema.json"
).read_text(encoding="utf-8")


def _scaffold(tmp_path: Path, *, aliases: dict[str, str],
              manifests: list[str], board_yamls: dict[str, str]) -> Path:
    """A minimal repo tree: alias table + schema, a metadata/libraries/
    manifest per name in `manifests`, and one board.yaml per (relative
    path, top-level `libraries:` YAML body) pair in `board_yamls`."""
    schema_dir = tmp_path / "metadata" / "schemas"
    schema_dir.mkdir(parents=True)
    (schema_dir / "library-aliases-v1.schema.json").write_text(
        _ALIAS_SCHEMA_TEXT, encoding="utf-8")
    (tmp_path / "metadata" / "library-aliases-v1.json").write_text(
        json.dumps({
            "schemaVersion": 1,
            "description": "test fixture",
            "aliases": aliases,
        }),
        encoding="utf-8",
    )
    libdir = tmp_path / "metadata" / "libraries"
    libdir.mkdir()
    for name in manifests:
        (libdir / f"{name}.yaml").write_text(
            f"schema_version: 1\nname: {name}\n", encoding="utf-8")
    for rel, body in board_yamls.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body, encoding="utf-8")
    return tmp_path


# ---------------------------------------------------------------------
# Non-vacuity: the gate must actually inspect the real board.yaml corpus.
# ---------------------------------------------------------------------

def test_real_repo_board_walk_covers_the_full_top_level_corpus():
    """Regression guard for the #1197 vacuity bug: the walk must see a
    real board.yaml corpus and find some declaring a top-level
    `libraries:`, not the empty set the retired per-core `enum` shape
    always produced.  Doesn't pin exact counts -- adding one routine
    board.yaml would flake this for no real defect; the gate's own
    `boards_checked == 0` guard (check_library_registry.py) is the real
    vacuity protection, this just backs it up."""
    all_boards = gate._board_yaml_files(REPO)
    with_libs = [b for b in all_boards if gate._top_level_library_names(b)]
    assert len(all_boards) > 0
    assert len(with_libs) > 0


def test_real_repo_alias_and_board_walk_layers_are_clean():
    """The whole gate must be clean against the real checkout: the alias-
    coverage and board.yaml-resolution layers that existed before #1197,
    and the collision-list layer, whose `_CURATED_LIBRARIES` is now
    derived from the same metadata/libraries/ registry
    (alp_orchestrate/validate.py) rather than hand-maintained, so it can
    no longer drift out from under this gate."""
    problems = gate.find_problems(REPO)
    assert problems == []
    # Non-vacuity: the walk actually inspected real board.yaml content,
    # not the empty corpus the retired per-core `enum` shape always
    # produced (#1197).
    assert any(gate._top_level_library_names(b) for b in gate._board_yaml_files(REPO))


def test_patching_the_hand_list_to_match_the_registry_clears_the_drift(monkeypatch):
    """Proves the collision-list check is actually driven by the real
    registry, not a fixed constant: adding the same tokens
    `_CURATED_LIBRARIES` already derives is a no-op, and the layer stays
    clean."""
    patched = validate_mod._CURATED_LIBRARIES | frozenset({
        "aws_iot", "azure_iot", "canopennode", "cmsis_nn", "lwm2m",
        "micro_ros", "micropython", "ros2", "zcbor",
    })
    monkeypatch.setattr(validate_mod, "_CURATED_LIBRARIES", patched)
    problems = gate.find_problems(REPO)
    assert problems == []


# ---------------------------------------------------------------------
# Fires on a deliberately broken input.
# ---------------------------------------------------------------------

def test_board_yaml_naming_a_nonexistent_library_is_flagged(tmp_path):
    root = _scaffold(
        tmp_path,
        # library-aliases-v1.schema.json requires a non-empty `aliases`;
        # this entry is unrelated to what's under test and resolves cleanly.
        aliases={"lvgl_alias": "lvgl"},
        manifests=["lvgl"],
        board_yamls={
            "examples/widget/board.yaml": (
                "name: widget\n"
                "libraries:\n"
                "  - name: totally-fake-lib\n"
            ),
        },
    )
    problems = gate.find_problems(root)
    assert any("totally-fake-lib" in p and "no metadata/libraries" in p
               for p in problems), problems


def test_alias_naming_a_nonexistent_manifest_is_flagged(tmp_path):
    root = _scaffold(
        tmp_path,
        aliases={"libcoap": "coap"},
        manifests=["lvgl"],   # 'coap' deliberately absent
        board_yamls={},
    )
    problems = gate.find_problems(root)
    assert any("'libcoap' -> 'coap'" in p for p in problems), problems


def test_scaffold_root_does_not_leak_the_real_repos_collision_list(tmp_path):
    """The `_CURATED_LIBRARIES` collision-list layer is Python source, not
    `root`-scoped repo data: `find_problems` only checks it when
    `root == ROOT` (this script's own repo).  Uses a manifest name that
    cannot possibly appear in the real repo's `_CURATED_LIBRARIES` (it
    only exists in this scaffold) -- with the #1197-followup root-
    scoping bug, that name would spuriously surface as an
    `_CURATED_LIBRARIES is missing` problem bled in from the wrong tree
    entirely, even though this scaffold's own alias table + board.yaml
    corpus are fully self-consistent.  Proven, not asserted: calling
    `gate._registry_vs_curated` directly with this scaffold's manifests
    against the REAL repo's `_CURATED_LIBRARIES` (the pre-fix behaviour)
    reports exactly that leaked problem."""
    root = _scaffold(
        tmp_path,
        aliases={"widget_alias": "totally-scaffold-only-widget-lib"},
        manifests=["totally-scaffold-only-widget-lib"],
        board_yamls={
            "examples/widget/board.yaml": (
                "name: widget\nlibraries: [widget_alias]\n"
            ),
        },
    )
    assert root != gate.ROOT

    # Evidence the scenario is real: the pre-fix code path (comparing this
    # scaffold's manifests against the REAL repo's curated set instead of
    # skipping) does leak a problem about a tree that was never asked
    # about.
    leaked = gate._registry_vs_curated(
        gate._manifest_names(root), {"widget_alias": "totally-scaffold-only-widget-lib"},
        validate_mod._CURATED_LIBRARIES)
    assert any("is missing 'widget_alias'" in p for p in leaked), leaked

    # The fixed find_problems(root) does not leak it.
    assert gate.find_problems(root) == []


# ---------------------------------------------------------------------
# Pure-function coverage for the registry/collision-list comparison.
# ---------------------------------------------------------------------

def test_registry_vs_curated_flags_missing_and_stale():
    manifests = {"cmsis-dsp", "zcbor"}
    aliases = {"cmsis_dsp": "cmsis-dsp"}
    curated = frozenset({"cmsis_dsp", "leftover_lib"})
    problems = gate._registry_vs_curated(manifests, aliases, curated)
    assert any("missing 'zcbor'" in p for p in problems)
    assert any("stale entry 'leftover_lib'" in p for p in problems)
    assert len(problems) == 2


def test_registry_vs_curated_clean_when_in_sync():
    manifests = {"cmsis-dsp", "zcbor"}
    aliases = {"cmsis_dsp": "cmsis-dsp"}
    curated = frozenset({"cmsis_dsp", "zcbor"})
    assert gate._registry_vs_curated(manifests, aliases, curated) == []


def test_top_level_library_names_reads_string_and_object_entries(tmp_path):
    by = tmp_path / "board.yaml"
    by.write_text(
        "name: b\n"
        "libraries:\n"
        "  - lvgl\n"
        "  - name: nanopb\n"
        "    cores: [m33]\n",
        encoding="utf-8",
    )
    assert gate._top_level_library_names(by) == ["lvgl", "nanopb"]


def test_top_level_library_names_ignores_malformed_yaml(tmp_path):
    by = tmp_path / "board.yaml"
    by.write_text("libraries: [unterminated\n", encoding="utf-8")
    assert gate._top_level_library_names(by) == []


# ---------------------------------------------------------------------
# _board_yaml_files must not walk build output (the #1197-followup hang:
# root.rglob("board.yaml") from repo root also descended into
# twister-out/ / build/ -- 160k+ files of build artefacts).
# ---------------------------------------------------------------------

def test_board_yaml_files_prunes_build_output_dirs(tmp_path):
    """Same proof for the fallback (non-git) walk this gate's tmp_path
    scaffolds all exercise: a real board.yaml is found, a same-named file
    sitting under a build-output dir is not."""
    real = tmp_path / "examples" / "widget" / "board.yaml"
    real.parent.mkdir(parents=True)
    real.write_text("som:\n  sku: X\n")
    for junk_dir in ("twister-out", "build"):
        junk = tmp_path / junk_dir / "widget" / "board.yaml"
        junk.parent.mkdir(parents=True)
        junk.write_text("not a real source file\n")
    found = gate._board_yaml_files(tmp_path)
    assert real in found
    assert len(found) == 1

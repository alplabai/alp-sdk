# SPDX-License-Identifier: Apache-2.0
"""The build plan's rendered-text config artefacts are byte-identical to the
standalone `alp_project.py --emit <mode> --core <id>` render (ADR-0026 §D,
tan-cli#1216): `tan` consumes the plan's bytes instead of re-rendering them, so
the two must never disagree.

Each mode names the file its artefact carries under the slice's `buildDir` and
the os classes it is emitted for; a slice outside those classes carries none.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import jsonschema
import pytest

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"

sys.path.insert(0, str(REPO / "scripts"))
from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402

#: emit mode -> (artefact file name, os classes that carry it)
MODES = {
    "dts-overlay": ("alp.overlay", ("zephyr", "baremetal")),
    "cmake-args": ("cmake-args.txt", ("zephyr", "baremetal")),
    "hw-info-h": ("alp_hw_info_build.h", ("zephyr", "baremetal")),
    "west-libraries": ("alp-west-libs.yml", ("zephyr", "baremetal")),
}

BOARDS = [
    "examples/multicore/heterogeneous-offload/board.yaml",
    "examples/multicore/rpmsg-aen/board.yaml",
    "examples/multicore/rpmsg-v2n/board.yaml",
    "examples/multicore/mproc-mailbox/board.yaml",
]


def _plan(board: Path) -> dict:
    project = load_board_yaml(board)
    return json.loads(emit_build_plan(
        project, board_yaml=board, build_root=Path("build")))


def _standalone(board: Path, mode: str, core: str) -> str:
    # `--emit cmake-args` prefixes each core with a `# --- core ---` marker the
    # plan artefact does not carry; the rest is the same text.
    done = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "alp_project.py"),
         "--input", str(board), "--emit", mode, "--core", core],
        capture_output=True, text=True, encoding="utf-8", check=True)
    out = done.stdout
    if mode == "cmake-args":
        marker, _, out = out.partition("\n")
        assert marker.startswith("# --- core: ")
    return out


@pytest.mark.parametrize("mode", sorted(MODES))
@pytest.mark.parametrize("board", BOARDS)
def test_plan_bytes_equal_standalone_render(board: str, mode: str) -> None:
    name, classes = MODES[mode]
    path = REPO / board
    plan = _plan(path)
    jsonschema.Draft202012Validator(
        json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))).validate(plan)

    seen = 0
    for sl in plan["slices"]:
        carried = [a for a in sl["configArtefacts"]
                   if a["path"].rsplit("/", 1)[-1] == name]
        if sl["backend"] not in classes:
            assert carried == [], f"{sl['coreId']} ({sl['backend']}) must not carry {name}"
            continue
        assert len(carried) == 1, f"{sl['coreId']}: expected one {name}"
        assert carried[0]["path"] == f"{sl['buildDir']}/{name}"
        assert carried[0]["contents"] == _standalone(path, mode, sl["coreId"]), (
            f"{sl['coreId']}: plan {name} drifted from `--emit {mode} --core`")
        seen += 1
    assert seen, f"no slice of {board} carries {name}; the test proves nothing"


def test_primary_config_artefact_stays_first() -> None:
    """A consumer reading `configArtefacts[0]` still gets alp.conf / local.conf."""
    plan = _plan(REPO / BOARDS[0])
    for sl in plan["slices"]:
        if sl["backend"] == "zephyr":
            assert sl["configArtefacts"][0]["path"].endswith("/alp.conf")
        elif sl["backend"] == "yocto":
            assert sl["configArtefacts"][0]["path"].endswith("/local.conf")


# --- the one downgrade: `dts-overlay-unavailable` ---------------------------


def _plan_with_missing_header(monkeypatch, tmp_path: Path) -> dict:
    import alp_project_emit.dts as dts

    monkeypatch.setattr(dts, "_board_header_path",
                        lambda name, root: tmp_path / "no-such-header.h")
    # `relative_to(REPO)` in the emitter's message needs a path under REPO.
    monkeypatch.setattr(dts, "REPO", tmp_path)
    return _plan(REPO / "examples/multicore/rpmsg-aen/board.yaml")


def test_missing_board_header_downgrades_to_a_warning(monkeypatch, tmp_path):
    plan = _plan_with_missing_header(monkeypatch, tmp_path)
    jsonschema.Draft202012Validator(
        json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))).validate(plan)

    warned = {w["coreId"] for w in plan["warnings"]
              if w["code"] == "dts-overlay-unavailable"}
    carriers = {sl["coreId"] for sl in plan["slices"]
                if sl["backend"] in ("zephyr", "baremetal")}
    assert carriers and warned == carriers
    for sl in plan["slices"]:
        names = {a["path"].rsplit("/", 1)[-1] for a in sl["configArtefacts"]}
        assert "alp.overlay" not in names
        if sl["backend"] in ("zephyr", "baremetal"):
            assert "cmake-args.txt" in names


def test_any_other_overlay_failure_still_fails_the_plan(monkeypatch):
    import alp_orchestrate.buildplan as bp
    from alp_orchestrate import OrchestratorError

    def broken(project, core_id):
        raise OrchestratorError("M33 ownership defect")

    monkeypatch.setattr(bp, "project_m33_overlay", broken)
    with pytest.raises(OrchestratorError, match="M33 ownership defect"):
        _plan(REPO / "examples/multicore/rpmsg-aen/board.yaml")


# --- artefact order (alp-sdk #2777) ------------------------------------------


def test_rendered_artefacts_follow_the_primary_in_fixed_order() -> None:
    """The seam-1 comparator allows exactly this ordered tail."""
    tail = ["alp.overlay", "cmake-args.txt", "alp_hw_info_build.h", "alp-west-libs.yml"]
    for board in BOARDS:
        for sl in _plan(REPO / board)["slices"]:
            if sl["backend"] not in ("zephyr", "baremetal"):
                continue
            names = [a["path"].rsplit("/", 1)[-1] for a in sl["configArtefacts"]]
            assert names[-4:] == tail, (board, sl["coreId"], names)


def test_unrecognised_sku_downgrades_hw_info_to_a_warning(monkeypatch) -> None:
    """A SKU outside the production families has no family for the header:
    the plan warns and omits `alp_hw_info_build.h`, west fragment unaffected."""
    import alp_project_loader

    def no_family(sku: str) -> str:
        raise ValueError(f"unrecognised SoM SKU pattern: {sku}")

    monkeypatch.setattr(alp_project_loader, "_sku_family", no_family)
    plan = _plan(REPO / "examples/multicore/rpmsg-aen/board.yaml")
    jsonschema.Draft202012Validator(
        json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))).validate(plan)
    warned = {w["coreId"] for w in plan["warnings"]
              if w["code"] == "hw-info-unavailable"}
    carriers = {sl["coreId"] for sl in plan["slices"]
                if sl["backend"] in ("zephyr", "baremetal")}
    assert carriers and warned == carriers
    for sl in plan["slices"]:
        names = {a["path"].rsplit("/", 1)[-1] for a in sl["configArtefacts"]}
        assert "alp_hw_info_build.h" not in names
        if sl["backend"] in ("zephyr", "baremetal"):
            assert "alp-west-libs.yml" in names


def test_a_non_sku_value_error_in_hw_info_still_fails_the_plan(monkeypatch) -> None:
    """Only the SKU->family lookup is downgraded. A ValueError from anywhere
    else in the render (a damaged hw-revisions table, a UnicodeDecodeError)
    must fail the plan, not silently drop the artefact."""
    import alp_project_emit.hw_info as hw_info

    def damaged(*args, **kwargs):
        raise ValueError("hw-revisions.yaml is damaged")

    # Patched at the emitter, not `load_family_table`: alp.conf reads the same
    # table and would fail the plan first, hiding whether hw-info downgrades it.
    monkeypatch.setattr(hw_info, "_emit_hw_info_h", damaged)
    with pytest.raises(ValueError, match="hw-revisions.yaml is damaged"):
        _plan(REPO / "examples/multicore/rpmsg-aen/board.yaml")

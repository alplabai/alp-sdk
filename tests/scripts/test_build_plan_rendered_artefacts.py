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

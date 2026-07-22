# SPDX-License-Identifier: Apache-2.0
"""Negative-matrix test for the retuned seam-1 comparator (#874 follow-up).

Guards `_drop_artefact_contents` (config-artefact CONTENT no longer
compared) and the sysbuild-scoped `-DEXTRA_CONF_FILE` strip (MINOR 5:
only non-sysbuild slices get the arg stripped) against a future edit
quietly reintroducing either the pre-retune content diff or the
"strip on every slice regardless of sysbuild" hole: a real plan-SHAPE
regression (command, env, slice-count, probe, artefact added/removed/
moved, a sysbuild slice wrongly gaining `-DEXTRA_CONF_FILE`) must still
fail the comparator; a content-only artefact mutation, and the one
hand-reviewed `debug.probe` delta, must not.

Mirrors what the tan-cli vendored twin of this comparator must stay in
lockstep with.

Run: python -m pytest tests/parity/test_seam1_field_diff.py -q
"""
from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import seam1_field_diff as s  # noqa: E402

_ORACLE_DIR = Path(__file__).resolve().parent / "oracle"


def _load(name: str) -> dict:
    return json.loads((_ORACLE_DIR / f"{name}.build-plan.json").read_text())


def _fails(oracle: dict, mutated: dict) -> bool:
    _, failing = s.diff_plans(s.normalize_plan(oracle), s.normalize_plan(mutated))
    return bool(failing)


def test_mutated_command_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["command"]["tool"] = "not-cmake"
    assert _fails(oracle, mutated)


def test_mutated_env_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["env"]["ALP_SDK_ROOT"] = "/something/else/entirely"
    assert _fails(oracle, mutated)


def test_slice_count_change_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"].pop()
    assert _fails(oracle, mutated)


def test_disallowed_probe_transition_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        if sl.get("debug", {}).get("probe") == "openocd":
            sl["debug"]["probe"] = "jlink"
            break
    assert _fails(oracle, mutated)


def test_allowed_probe_transition_passes():
    """The one hand-reviewed delta (openocd -> null, #848) must still pass."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        if sl.get("debug", {}).get("probe") == "openocd":
            sl["debug"]["probe"] = None
    assert not _fails(oracle, mutated)


def test_artefact_added_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["configArtefacts"].append(
        {"path": "build/extra/alp.conf", "contents": "# new\n"})
    assert _fails(oracle, mutated)


def test_artefact_removed_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["configArtefacts"] = []
    assert _fails(oracle, mutated)


def test_artefact_path_moved_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["configArtefacts"][0]["path"] += ".moved"
    assert _fails(oracle, mutated)


def test_content_only_mutation_passes():
    """The intended retune: config-artefact CONTENT alone is no longer
    diffed -- covered by the emit-snapshot goldens instead (see
    tests/parity/README.md)."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        for art in sl.get("configArtefacts", []):
            art["contents"] = "# totally different content\n"
    assert not _fails(oracle, mutated)


def test_sysbuild_slice_wrongly_gaining_extra_conf_file_fails():
    """MINOR 5: the `-DEXTRA_CONF_FILE` strip is scoped to non-sysbuild
    slices only.  Both `connectivity_iot-fleet-ota` slices are sysbuild
    (`--sysbuild` in command.args); if one wrongly gained
    `-DEXTRA_CONF_FILE` (the Option-A per-core-config-drop bug ADR-0020
    Amendment item 4 warns about -- it would land on the sysbuild image,
    not the app, silently dropping the per-core alp.conf), that must
    still fail rather than get silently stripped away like a non-sysbuild
    slice's legitimate `-DEXTRA_CONF_FILE`."""
    oracle = _load("connectivity_iot-fleet-ota")
    mutated = copy.deepcopy(oracle)
    sl = mutated["slices"][0]
    assert "--sysbuild" in sl["command"]["args"]
    sl["command"]["args"] = list(sl["command"]["args"]) + [
        "-DEXTRA_CONF_FILE=/should/not/be/on/a/sysbuild/slice/alp.conf"]
    assert _fails(oracle, mutated)


def test_non_sysbuild_slice_extra_conf_file_still_stripped():
    """Sanity check for the scoping itself: a NON-sysbuild slice's real
    `-DEXTRA_CONF_FILE` arg (the #863/#871 intended delta) is still
    stripped and does not, on its own, fail the comparator."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    sl = mutated["slices"][0]
    assert "--sysbuild" not in (sl["command"].get("args") or [])
    sl["command"]["args"] = list(sl["command"]["args"]) + [
        "-DEXTRA_CONF_FILE=/some/path/alp.conf"]
    assert not _fails(oracle, mutated)

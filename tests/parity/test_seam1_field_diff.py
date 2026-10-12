# SPDX-License-Identifier: Apache-2.0
"""Negative-matrix test for the retuned seam-1 comparator (#874 follow-up).

Guards `_drop_artefact_contents` (config-artefact CONTENT no longer
compared) and the sysbuild-scoped `-DEXTRA_CONF_FILE` strip (MINOR 5:
only non-sysbuild slices get the arg stripped) against a future edit
quietly reintroducing either the pre-retune content diff or the
"strip on every slice regardless of sysbuild" hole: a real plan-SHAPE
regression (command, env, slice-count, probe, artefact added/removed/
moved, a sysbuild slice wrongly gaining `-DEXTRA_CONF_FILE`) must still
fail the comparator; a content-only artefact mutation, and the
hand-reviewed deltas (`debug.probe`, the #1344 additive keys at their
inert default, and the #1360 one-segment west `build/` relocation on a
zephyr slice's six artifact paths), must not.

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
    return json.loads((_ORACLE_DIR / f"{name}.build-plan.json").read_text(encoding="utf-8"))


def _fails(oracle: dict, mutated: dict) -> bool:
    _, failing = s.diff_plans(s.normalize_plan(oracle), s.normalize_plan(mutated))
    return bool(failing)


def _first_commanded_slice(plan: dict) -> int:
    """Index of the first slice that actually carries a command.

    NOT hardcoded to 0: tan-cli#1223 made the AEN A32 `a32_cluster` slice
    resolve `command: null` (the Yocto MACHINE is unbuildable), and that
    slice is index 0 in these fixtures.  Selecting by property keeps these
    tests pinned to what they mean -- "a slice with a real command" -- so
    the next re-vendor that refuses a different slice cannot silently turn
    them into `NoneType` errors the way it did here.
    """
    for i, sl in enumerate(plan.get("slices", [])):
        if sl.get("command") is not None:
            return i
    raise AssertionError("fixture has no slice with a command")


def test_mutated_command_fails():
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    # slices[0] (a32_cluster) carries `command: null` -- issue #1982, the
    # MACHINE is known-non-buildable; slices[1] (m55_he) is this fixture's
    # real, non-null command.
    mutated["slices"][1]["command"]["tool"] = "not-cmake"
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


def test_deferred_placeholders_is_not_diffed():
    """#2696: derived from artefact contents, which are not diffed either."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["deferredPlaceholders"] = ["MENDER_TENANT_TOKEN"]
    assert not _fails(oracle, mutated)


def test_other_new_top_level_key_still_fails():
    """Dropping `deferredPlaceholders` must not become a blanket rule."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["someNewTopLevelKey"] = []
    assert _fails(oracle, mutated)


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
    # slices[0] (a32_cluster) carries `command: null` (#1982); slices[1]
    # (m55_he) is this fixture's real, non-sysbuild command.
    sl = mutated["slices"][1]
    assert "--sysbuild" not in (sl["command"].get("args") or [])
    sl["command"]["args"] = list(sl["command"]["args"]) + [
        "-DEXTRA_CONF_FILE=/some/path/alp.conf"]
    assert not _fails(oracle, mutated)


# ---------------------------------------------------------------------
# #1344 / alplabai/tan-cli#550 -- the two keys the 97ad481b oracle
# predates entirely, allowed through ONLY at their inert default.
# ---------------------------------------------------------------------


def test_additive_keys_at_their_inert_default_pass():
    """`postCommands: []` and `artifacts.outputDir: null` on a zephyr /
    yocto slice are the whole live-vs-oracle delta #1344 introduces --
    the oracle carries neither key. Without this allowance the
    comparator exits 1 on all five oracle boards."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        sl["postCommands"] = []
        sl["artifacts"]["outputDir"] = None
    assert not _fails(oracle, mutated)


def test_additive_key_with_a_real_value_still_fails():
    """The allowance is keyed on the exact inert value, not on "the
    oracle lacked this key": a slice that really grows a build step (a
    baremetal slice) is an unreviewed shape delta against an oracle that
    emitted none, and must still FAIL."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        sl["postCommands"] = []
        sl["artifacts"]["outputDir"] = None
    mutated["slices"][0]["postCommands"] = [
        {"tool": "cmake", "args": ["--build", "."], "cwd": "build/x"}]
    assert _fails(oracle, mutated)

    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        sl["postCommands"] = []
        sl["artifacts"]["outputDir"] = None
    mutated["slices"][0]["artifacts"]["outputDir"] = "build/x/output"
    assert _fails(oracle, mutated)


def test_an_unrelated_new_key_is_not_swept_in_by_the_allowance():
    """Negative control against over-broadness: an allowance shaped as
    "oracle `<missing>` plus a falsy live value is fine" would blind the
    gate to every future additive key. Only the two NAMED keys pass."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["someBrandNewKey"] = []
    assert _fails(oracle, mutated)
    mutated = copy.deepcopy(oracle)
    mutated["slices"][0]["artifacts"]["someBrandNewArtifact"] = None
    assert _fails(oracle, mutated)


def test_a_changed_existing_value_is_never_an_allowed_additive():
    """The allowance requires the oracle to genuinely LACK the key
    (`<missing>`). A key the oracle carries with a real value, blanked to
    the same inert default, is a capability loss -- still a failure."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = copy.deepcopy(oracle)
    for sl in mutated["slices"]:
        sl["postCommands"] = []
        sl["artifacts"]["outputDir"] = None
    # slices[1] is the zephyr slice -- the one whose `compileCommands`
    # the oracle carries as a real path (slices[0] is yocto, null there).
    assert oracle["slices"][1]["artifacts"]["compileCommands"]
    mutated["slices"][1]["artifacts"]["compileCommands"] = None
    assert _fails(oracle, mutated)


# ── alp-sdk #1360: the west `build/` level ───────────────────────────────────

def _nest(plan: dict) -> dict:
    """Apply the #1360 relocation to every zephyr slice of `plan`: insert the
    one `build/` level `west build` writes, immediately before each artifact
    field's fixed Zephyr tail. The exact transformation the live planner now
    emits and the comparator must forgive."""
    out = copy.deepcopy(plan)
    for sl in out["slices"]:
        arts = sl["artifacts"]
        for key, tail in (("elf", "zephyr/zephyr.elf"),
                          ("map", "zephyr/zephyr.map"),
                          ("bin", "zephyr/zephyr.bin"),
                          ("sizeReport", "zephyr/zephyr.stat"),
                          ("symbols", "zephyr/zephyr.symbols"),
                          ("compileCommands", "compile_commands.json")):
            old = arts.get(key)
            if isinstance(old, str) and old.endswith("/" + tail):
                arts[key] = old[:-len(tail)] + "build/" + tail
    return out


def test_west_build_level_relocation_passes():
    """The whole #1360 delta: a zephyr slice's six artifact paths gain the
    `build/` level west actually writes (`build/m55_he-zephyr/zephyr/
    zephyr.elf` -> `build/m55_he-zephyr/build/zephyr/zephyr.elf`). Without
    this allowance the comparator exits 1 on all five oracle boards."""
    oracle = _load("multicore_rpmsg-aen")
    assert (oracle["slices"][1]["artifacts"]["elf"]
            == "build/m55_he-zephyr/zephyr/zephyr.elf")
    mutated = _nest(oracle)
    assert (mutated["slices"][1]["artifacts"]["elf"]
            == "build/m55_he-zephyr/build/zephyr/zephyr.elf")
    assert not _fails(oracle, mutated)


def test_two_build_levels_still_fails():
    """Keyed on the EXACT one-segment insertion. A second `build/` -- the
    `-d <buildDir>` double-nest finding M14 warns about -- is a different
    path and must still fail."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = _nest(oracle)
    mutated["slices"][1]["artifacts"]["elf"] = \
        "build/m55_he-zephyr/build/build/zephyr/zephyr.elf"
    assert _fails(oracle, mutated)


def test_build_level_inserted_elsewhere_still_fails():
    """The insertion point is pinned too: `build/` prepended at the ROOT
    rather than before the field's tail is a different path, not the
    reviewed relocation."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = _nest(oracle)
    mutated["slices"][1]["artifacts"]["elf"] = \
        "build/build/m55_he-zephyr/zephyr/zephyr.elf"
    assert _fails(oracle, mutated)


def test_renamed_artifact_under_the_new_level_still_fails():
    """The filename is Zephyr's own CMake layout to pick, never this
    planner's. A relocated path whose tail changed is a shape delta a
    human still has to look at."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = _nest(oracle)
    mutated["slices"][1]["artifacts"]["elf"] = \
        "build/m55_he-zephyr/build/zephyr/app.elf"
    assert _fails(oracle, mutated)


def test_relocation_allowance_does_not_reach_outputdir_or_other_fields():
    """Negative control against over-broadness: the allowance names six
    fields. `artifacts.outputDir` gaining a real path, or a non-artifact
    field taking the same one-segment relocation, must still fail."""
    oracle = _load("multicore_rpmsg-aen")
    mutated = _nest(oracle)
    mutated["slices"][1]["artifacts"]["outputDir"] = \
        "build/m55_he-zephyr/build/zephyr/zephyr.elf"
    assert _fails(oracle, mutated)

    mutated = _nest(oracle)
    mutated["slices"][1]["command"]["cwd"] = "build/m55_he-zephyr/build"
    assert _fails(oracle, mutated)


def test_sysbuild_slice_image_scoped_extra_conf_file_stripped():
    """#866: a sysbuild slice's `-D<image>_EXTRA_CONF_FILE=` is the intended
    plan-native delta and does not, on its own, fail the comparator."""
    oracle = _load("connectivity_iot-fleet-ota")
    mutated = copy.deepcopy(oracle)
    sl = mutated["slices"][0]
    args = list(sl["command"]["args"])
    app = args[args.index("--sysbuild") - 1]
    image = app.rstrip("/").rsplit("/", 1)[-1]
    sl["command"]["args"] = args + [
        f"-D{image}_EXTRA_CONF_FILE=/some/path/alp.conf"]
    assert not _fails(oracle, mutated)


def test_sysbuild_slice_wrong_image_extra_conf_file_still_fails():
    """#866: a prefix naming another image (e.g. mcuboot) is a regression."""
    oracle = _load("connectivity_iot-fleet-ota")
    mutated = copy.deepcopy(oracle)
    sl = mutated["slices"][0]
    sl["command"]["args"] = list(sl["command"]["args"]) + [
        "-Dmcuboot_EXTRA_CONF_FILE=/some/path/alp.conf"]
    assert _fails(oracle, mutated)


# --- the rendered reference artefacts (alp-sdk #2771, #2777) ---

_FULL_TAIL = ("alp.overlay", "cmake-args.txt", "alp_hw_info_build.h", "alp-west-libs.yml")


def _with_extras(plan: dict, *names: str) -> dict:
    mutated = copy.deepcopy(plan)
    sl = mutated["slices"][_first_commanded_slice(mutated)]
    base = sl["configArtefacts"][0]["path"].rsplit("/", 1)[0]
    for name in names:
        sl["configArtefacts"].append({"path": f"{base}/{name}", "contents": "x\n"})
    return mutated


def test_rendered_artefacts_after_the_oracles_entries_pass():
    oracle = _load("multicore_rpmsg-aen")
    allowed, failing = s.diff_plans(
        s.normalize_plan(oracle),
        s.normalize_plan(_with_extras(oracle, *_FULL_TAIL)))
    assert not failing
    assert any(p.endswith("[+rendered]") for p, _, _ in allowed)


def test_cmake_args_only_no_header_case_passes():
    oracle = _load("multicore_rpmsg-aen")
    assert not _fails(oracle, _with_extras(oracle, *_FULL_TAIL[1:]))


def test_downgraded_tails_pass():
    oracle = _load("multicore_rpmsg-aen")
    assert not _fails(oracle, _with_extras(
        oracle, "alp.overlay", "cmake-args.txt", "alp-west-libs.yml"))
    assert not _fails(oracle, _with_extras(oracle, "cmake-args.txt", "alp-west-libs.yml"))


def test_partial_tails_that_never_occur_fail():
    oracle = _load("multicore_rpmsg-aen")
    for tail in (_FULL_TAIL[:1], _FULL_TAIL[:2], _FULL_TAIL[:3],
                 ("cmake-args.txt",), ("alp.overlay",),
                 ("alp.overlay", "cmake-args.txt"),
                 _FULL_TAIL[1:3], _FULL_TAIL[2:], ("alp.overlay", "alp_hw_info_build.h", "alp-west-libs.yml"),
                 ("alp.overlay", "alp-west-libs.yml"),
                 ("alp_hw_info_build.h", "alp-west-libs.yml")):
        assert _fails(oracle, _with_extras(oracle, *tail)), tail


def test_an_unknown_extra_artefact_fails():
    oracle = _load("multicore_rpmsg-aen")
    assert _fails(oracle, _with_extras(oracle, *_FULL_TAIL, "surprise.txt"))
    assert _fails(oracle, _with_extras(oracle, "alp.overlay", "surprise.txt"))
    assert _fails(oracle, _with_extras(oracle, "alp-link-itcm.conf"))


def test_a_modified_primary_artefact_fails_even_with_the_extras():
    oracle = _load("multicore_rpmsg-aen")
    mutated = _with_extras(oracle, *_FULL_TAIL)
    sl = mutated["slices"][_first_commanded_slice(mutated)]
    sl["configArtefacts"][0]["path"] += ".moved"
    assert _fails(oracle, mutated)


def test_reordered_extras_fail():
    oracle = _load("multicore_rpmsg-aen")
    assert _fails(oracle, _with_extras(oracle, "cmake-args.txt", "alp.overlay"))
    assert _fails(oracle, _with_extras(
        oracle, "alp.overlay", "cmake-args.txt", "alp-west-libs.yml", "alp_hw_info_build.h"))


def test_extras_inserted_before_the_oracles_entries_fail():
    oracle = _load("multicore_rpmsg-aen")
    mutated = _with_extras(oracle, *_FULL_TAIL)
    sl = mutated["slices"][_first_commanded_slice(mutated)]
    sl["configArtefacts"].reverse()
    assert _fails(oracle, mutated)

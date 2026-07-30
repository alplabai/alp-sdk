#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Byte-parity gate: a Zephyr example's own `CMakeLists.txt` (which calls
`alp_sdk_zephyr_conf(<id>)` from `cmake/alp.cmake` at configure time) and the
planner's build-plan `configArtefacts` (`--emit build-plan`, consumed by
`tan`) MUST materialise the identical `alp.conf` for the same core.

THIS GATE NO LONGER RUNS EITHER LIVE CODE PATH.

It used to: both call sites route through `alp_orchestrate.kconfig.
_slice_alp_conf`, and the gate imported that function plus `load_board_yaml`
to invoke both paths itself (a live `subprocess` call to `alp_project.py` on
one side, a direct function call on the other) and diff the output.
`scripts/alp_orchestrate/` is being deleted -- the planner it fronted now
lives in the tan repository -- so nothing left in this repo can run that
comparison directly.

What alp-sdk CAN still see are two pieces of evidence ALREADY COMMITTED by
`check_emit_snapshots.py`, each pinning one side of the invariant as of the
last time both paths were observed to agree:

  * `tests/fixtures/emit-snapshots/proj-*.zephyr-conf.snap` -- the per-core
    `alp.conf` rendering (the `CMakeLists.txt` / `alp_sdk_zephyr_conf()`
    side). A `proj-<id>.zephyr-conf.snap` file is the UNSCOPED
    (`--core`-less) emit -- ADR-0020's own per-core-sum spelling -- so it
    holds one `# --- core: <id> (zephyr) ---` section per Zephyr core the
    fixture's board.yaml declares.
  * the `configArtefacts` entry inside `tests/fixtures/emit-snapshots/
    *.build-plan.snap` whose `path` ends in `alp.conf` -- the planner's
    build-plan side, one per Zephyr slice.

This gate diffs those two ALREADY-COMMITTED artefacts, byte-for-byte, for
every (board.yaml, core id) pair that appears in BOTH families. No code runs
on either side any more.

WHAT WAS LOST, and it needs saying rather than discovering later: the gate no
longer proves the two LIVE code paths still agree -- it proves the two
COMMITTED artefacts do. If a golden is ever refreshed (`check_emit_snapshots.
py --update`) without a human noticing the two paths had actually forked in
the process, this gate goes green over a real divergence. The live
invariant -- "the code that renders each of these still agrees" -- moves to
whoever renders them now, i.e. `tan`; nothing left in alp-sdk can stand in
for that.

CURRENT STATE OF THE FIXTURE CORPUS, also worth saying rather than
discovering later: as of this writing the overlap is EMPTY. `check_emit_
snapshots.py`'s `_PROJ_BOARDS` (aen-analog-validate, v2n-power-monitor,
spi-slave -- exercises `alp_project.py`'s single/multi-Zephyr-core `--emit`
surfaces) and its `CASES` ORCH boards (rpmsg-aen/-v2n/-imx93,
heterogeneous-offload, audio/i2s-tone, connectivity/iot-fleet-ota --
exercises `alp_orchestrate`'s multicore surfaces) were built for two separate
purposes and share no board.yaml. The guard below fails loudly on that
instead of reporting a false "0 pairs, OK". Expanding the corpus -- adding a
board.yaml that appears in both `_PROJ_BOARDS` and `CASES` -- is what would
restore real coverage; that edit belongs to `check_emit_snapshots.py`, out of
scope here.

Usage:

    python scripts/check_zephyr_conf_parity.py [--snapshot-dir PATH]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAPSHOT_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"

# Which board.yaml a `proj-<id>.zephyr-conf.snap` fixture was rendered from --
# the .snap file itself is plain alp.conf text with no board.yaml field of its
# own (unlike a build-plan .snap, which carries `boardYaml`). Mirrors
# `check_emit_snapshots.py`'s `_PROJ_BOARDS` (the generator of these exact
# fixtures) -- keep in sync if a PROJ board is added, renamed, or dropped
# there; an id missing here is a hard failure below, not a silent skip.
_PROJ_BOARD_YAML = {
    "aen": "examples/aen/aen-analog-validate/board.yaml",
    "v2n": "examples/v2n/v2n-power-monitor/board.yaml",
    "nsim": "examples/peripheral-io/spi-slave/board.yaml",
}

# A `proj-*.zephyr-conf.snap` file concatenates one section per declared
# Zephyr core: `# --- core: <id> (zephyr) ---` followed by that core's
# rendered `alp.conf`.
_CORE_HEADER_RE = re.compile(r"(?m)^# --- core: (\S+) \(zephyr\) ---\n")

# The core id an example's CMakeLists.txt scopes its emit to. Two spellings
# are recognised:
#
#   * `alp_sdk_zephyr_conf(<core> [BOARD_YAML <path>])` -- the shared
#     `cmake/alp.cmake` helper every example calls today.
#   * a raw `--emit zephyr-conf --core <id>` invocation -- the pre-helper
#     shape, still matched so a hand-rolled example (or one copied from an
#     older release) stays inside this gate instead of silently dropping out
#     of the corpus.
_CORE_RE = re.compile(
    r"alp_sdk_zephyr_conf\(\s*([^\s)]+)"
    r"|--emit\s+zephyr-conf\s+--core\s+(\S+)")
# Any zephyr-conf emit, `--core`-scoped or not. When a file's count of these
# exceeds its `--core`-scoped count (`_CORE_RE`), some invocation is UNSCOPED
# -- the cross-core Kconfig sum ADR-0020's addendum retired. `alp_sdk_zephyr_
# conf()` FATAL_ERRORs on an empty core argument, so the helper spelling can
# only ever be scoped; the raw spelling still can't.
_EMIT_RE = re.compile(r"alp_sdk_zephyr_conf\(|--emit\s+zephyr-conf\b")


def _code(text: str) -> str:
    """`text` with every whole-line `#` comment dropped. `examples/**` carries
    CMakeLists.txt files whose comments MENTION the emit precisely to say
    they do NOT invoke it, plus several that name `alp_sdk_zephyr_conf()` in
    prose -- a bare text match reads those as invocations and fails the gate
    on a comment."""
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#"))


def _find_unscoped_emits(repo: Path = REPO) -> list[Path]:
    """CMakeLists.txt files with a `--emit zephyr-conf` invocation that is
    NOT `--core`-scoped -- the cross-core Kconfig leak ADR-0020's addendum
    retired. The byte-parity check below no longer scans CMakeLists.txt at
    all (it diffs committed snapshot artefacts instead), so without this
    guard a re-introduced unscoped emit would go completely uncaught while
    shipping cross-core-contaminated firmware. Fail loudly instead."""
    leaks = []
    for cmakelists in sorted(repo.glob("examples/**/CMakeLists.txt")):
        text = _code(cmakelists.read_text(encoding="utf-8"))
        if len(_EMIT_RE.findall(text)) > len(_CORE_RE.findall(text)):
            leaks.append(cmakelists)
    return leaks


def _zephyr_conf_pairs(snapshot_dir: Path) -> dict[tuple[str, str], str]:
    """{(board.yaml, core id): rendered alp.conf} for every
    `proj-*.zephyr-conf.snap` under `snapshot_dir`.

    Splitting a file on `_CORE_HEADER_RE` re-derives each core's own content,
    except that every chunk but the LAST in a multi-core file carries one
    extra trailing newline -- the blank-line separator the aggregate emit
    inserts before the next `# --- core: ... ---` header, no part of any
    single core's own rendered `alp.conf`. Stripped here so the comparison is
    against the same bytes a `--core`-scoped emit would have produced (the
    last chunk in a file needs no stripping: a single-core fixture like
    `proj-v2n.zephyr-conf.snap` already ends exactly where its build-plan
    counterpart's `contents` value does).
    """
    out: dict[tuple[str, str], str] = {}
    for snap in sorted(snapshot_dir.glob("proj-*.zephyr-conf.snap")):
        bid = snap.name[len("proj-"): -len(".zephyr-conf.snap")]
        if bid not in _PROJ_BOARD_YAML:
            raise SystemExit(
                f"check_zephyr_conf_parity: {snap.name} has no entry in "
                f"_PROJ_BOARD_YAML -- add its (id, board.yaml) pair (mirror "
                f"check_emit_snapshots.py's _PROJ_BOARDS) rather than "
                f"silently dropping it from the corpus.")
        board_yaml = _PROJ_BOARD_YAML[bid]
        text = snap.read_text(encoding="utf-8")
        parts = _CORE_HEADER_RE.split(text)
        cores = list(zip(parts[1::2], parts[2::2]))
        for i, (core_id, chunk) in enumerate(cores):
            if i != len(cores) - 1 and chunk.endswith("\n"):
                chunk = chunk[:-1]
            out[(board_yaml, core_id)] = chunk
    return out


def _build_plan_alp_confs(snapshot_dir: Path) -> dict[tuple[str, str], str]:
    """{(board.yaml, core id): configArtefact contents} for the `alp.conf`
    artefact of every Zephyr slice in every `*.build-plan.snap` under
    `snapshot_dir`."""
    out: dict[tuple[str, str], str] = {}
    for snap in sorted(snapshot_dir.glob("*.build-plan.snap")):
        doc = json.loads(snap.read_text(encoding="utf-8"))
        board_yaml = doc.get("boardYaml")
        for slc in doc.get("slices", []):
            for art in slc.get("configArtefacts", []):
                if art.get("path", "").endswith("alp.conf"):
                    out[(board_yaml, slc["coreId"])] = art["contents"]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--snapshot-dir", type=Path, default=SNAPSHOT_DIR)
    args = ap.parse_args()

    leaks = _find_unscoped_emits()
    if leaks:
        print("check_zephyr_conf_parity: unscoped `--emit zephyr-conf` "
              "(no --core) -- the cross-core Kconfig leak ADR-0020 retired; "
              "scope each to the one Zephyr core its CMakeLists.txt builds:",
              file=sys.stderr)
        for leak in leaks:
            print(f"  · {leak.relative_to(REPO).as_posix()}", file=sys.stderr)
        return 1

    zephyr_conf = _zephyr_conf_pairs(args.snapshot_dir)
    build_plan = _build_plan_alp_confs(args.snapshot_dir)
    pairs = sorted(set(zephyr_conf) & set(build_plan))

    if not pairs:
        print(f"check_zephyr_conf_parity: 0 board/core pairs found in both "
              f"proj-*.zephyr-conf.snap ({len(zephyr_conf)} core(s) "
              f"catalogued) and *.build-plan.snap ({len(build_plan)} core(s) "
              f"catalogued) under {args.snapshot_dir} -- an empty comparison "
              f"proves nothing; see the module docstring's \"CURRENT STATE "
              f"OF THE FIXTURE CORPUS\".", file=sys.stderr)
        return 1

    failures: list[str] = []
    for board_yaml, core_id in pairs:
        want = zephyr_conf[(board_yaml, core_id)]
        got = build_plan[(board_yaml, core_id)]
        if want != got:
            failures.append(
                f"{board_yaml} (core {core_id}): proj-*.zephyr-conf.snap != "
                f"build-plan configArtefact -- the two committed evidence "
                f"artefacts have diverged")
        else:
            print(f"OK   {board_yaml} (core {core_id})")

    if failures:
        print(f"\ncheck_zephyr_conf_parity: {len(failures)} mismatch(es):",
              file=sys.stderr)
        for f in failures:
            print(f"  · {f}", file=sys.stderr)
        return 1

    print(f"\ncheck_zephyr_conf_parity: {len(pairs)} board/core pair(s), "
          f"proj-*.zephyr-conf.snap <-> build-plan alp.conf byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

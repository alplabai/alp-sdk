#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Seam-1 comparator: live alp-sdk build-plan emit vs. the frozen oracle.

ADR-0020 (alp-sdk#855 amendment) requires a two-seam parity gate before the
`tan`-is-sole-executor migration can release. This is seam 1: **plan-shape**
parity -- does a live `--emit build-plan` from the alp-sdk checkout under test
still match the frozen oracle, field for field? Seam 2 (materialise byte-check
+ a real build + a Renode smoke test) is a documented follow-up that needs a
Linux toolchain runner -- see `tests/parity/README.md` and the `seam2`
placeholder job in `.github/workflows/parity.yml`.

The oracle (`tests/parity/oracle/*.build-plan.json`) was captured at alp-sdk
`df312cec^` == `97ad481b` ("feat(build-plan): publish envAppendPath +
executionPolicy (ADR-0020 Phase 1, additive)", #847) -- the last SHA that
carries *both* `fan_out` (the retired in-repo executor, still alive as a
build oracle at that point) and the Phase-1 fields (`envAppendPath`,
`executionPolicy`) `tan` now depends on. `df312cec` (#848) retired `fan_out`
and every SDK-side executor, so nothing after it can be diffed against an
in-repo oracle again -- this is the last frame where that comparison exists.

Build plans are NOT hermetic: they embed the emitting checkout's absolute
root path (`env.ALP_SDK_ROOT`, `envAppendPath.*`, per-slice `appDir`), the
emitting commit (`sdkCommit`), and the emitting host's Python interpreter
path (each cmake/sysbuild slice's `-DPython3_EXECUTABLE=<path>` arg, pinned
by `orchestrator.py`'s `sys.executable`). All three are real signal for a
human but pure noise for a parity diff -- normalize them before comparing:

  * any string carrying the checkout root as a prefix -> the root prefix is
    replaced with the literal token ``__SDKROOT__`` (root discovered from the
    plan's own ``slices[0].env.ALP_SDK_ROOT`` -- no path is hardcoded);
  * ``sdkCommit`` -> the literal token ``__SHA__``;
  * a ``command.args`` entry matching ``-DPython3_EXECUTABLE=<path>`` -> the
    path is replaced with the literal token ``<PYEXE>``.

Issue #865 flips a LIVE plan's emit from those baked-in absolute paths to
literal ``${SDK_ROOT}``/``${PROJECT_ROOT}``/``${PYTHON}`` tokens (tan-cli,
PR #24, substitutes them at materialise time). The frozen 97ad481b oracle
predates that and stays absolute -- `normalize_plan` reconciles the two
shapes onto the same normalized form; see its docstring for the mapping.

The ONLY semantic delta allowed to pass without failing the gate is
``slices[*].debug.probe`` going from ``"openocd"`` (the oracle, at 97ad481b)
to ``null`` (df312cec and later). That is #848's hand-reviewed, intentional
change: the SDK-side executor named a debug-probe runner because it drove
`west`/OpenOCD itself; post-ADR-0020 the SDK doesn't own flashing at all, so
`probe` is no longer a value it can honestly assert -- `tan` (or the caller)
picks the probe. `debug.probe` moving to `null` is a downgrade to "SDK is not
claiming" and not a hidden capability loss. See ADR-0020's Amendment: "the
only 97ad481b<->df312cec emit delta is `debug.probe` 'openocd'->null,
hand-reviewed." Any OTHER diff -- a changed command, a changed env value, a
changed slice count, a probe change to anything other than that exact
openocd->null transition -- FAILS the gate.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterator

_SDKROOT_TOKEN = "__SDKROOT__"
_SHA_TOKEN = "__SHA__"
_PYEXE_TOKEN = "<PYEXE>"
_PYEXE_ARG_RE = re.compile(r"^-DPython3_EXECUTABLE=.*$")

# The one delta ADR-0020 hand-reviewed and allows through the gate:
# debug.probe "openocd" (oracle, 97ad481b) -> null (df312cec+, #848).
_ALLOWED_OLD_PROBE = "openocd"
_ALLOWED_NEW_PROBE = None


class ComparatorError(RuntimeError):
    """Raised for setup/emit failures (not diffs -- diffs are reported)."""


def _discover_sdk_root(plan: dict) -> str | None:
    """Find the checkout-root absolute path a plan embeds.

    Every slice's ``env.ALP_SDK_ROOT`` carries it; the first slice that has
    one is enough (`buildplan.py` derives it once, from the module's own
    file location, so it is constant across slices in a single plan).
    """
    for slice_ in plan.get("slices", []):
        root = slice_.get("env", {}).get("ALP_SDK_ROOT")
        if root:
            return root
    return None


def _normalize_strings(node: Any, sdk_root: str | None,
                        project_token: str | None = None) -> Any:
    """Deep-copy `node`, replacing every checkout-root occurrence in any
    string, and tokenizing any `-DPython3_EXECUTABLE=<path>` command arg.

    A plain prefix check isn't enough: some fields embed the root
    mid-string alongside other content (e.g. a sysbuild `-DSB_CONF_FILE=
    <root>/a;<root>/b` arg carries the root twice, neither at index 0), so
    this does a global substring replace rather than a prefix-only one.

    `-DPython3_EXECUTABLE=<path>` is a second, unrelated non-hermetic field:
    it carries the emitting host's interpreter path (`sys.executable`, not
    the checkout root), so it needs its own match -- tokenizing the whole
    arg, not just a substring, keeps the shape check (the arg must still be
    present) while dropping the host-specific value. This also already
    covers a tokened live plan's literal `${PYTHON}` (issue #865): the
    regex matches on the `-DPython3_EXECUTABLE=` prefix alone, so it fires
    regardless of what follows.

    `project_token`, when set (a #865 tokened plan -- see `normalize_plan`),
    substitutes `${PROJECT_ROOT}`/`${SDK_ROOT}` literal tokens; a `;`-joined
    value (e.g. `SB_CONF_FILE`) can legitimately carry one of each, so both
    substitutions always run rather than branching on which is present.
    """
    if isinstance(node, dict):
        return {k: _normalize_strings(v, sdk_root, project_token)
                for k, v in node.items()}
    if isinstance(node, list):
        return [_normalize_strings(v, sdk_root, project_token)
                for v in node]
    if isinstance(node, str):
        if _PYEXE_ARG_RE.match(node):
            return f"-DPython3_EXECUTABLE={_PYEXE_TOKEN}"
        if project_token is not None:
            return (node.replace("${PROJECT_ROOT}", project_token)
                        .replace("${SDK_ROOT}", _SDKROOT_TOKEN))
        if sdk_root:
            return node.replace(sdk_root, _SDKROOT_TOKEN)
    return node


def _project_relpath(plan: dict) -> str:
    """Directory holding `boardYaml`, e.g. `examples/audio/i2s-tone` for a
    `boardYaml` of `examples/audio/i2s-tone/board.yaml`.

    `boardYaml` is deliberately NOT tokenized by the #865 planner change
    (it stays repo-relative, as-passed) precisely so it can anchor this:
    every harness fixture lives UNDER the SDK root, so once `${SDK_ROOT}`
    is substituted in, `${PROJECT_ROOT}` == `__SDKROOT__/<this relpath>`.
    """
    return os.path.dirname(plan.get("boardYaml") or "")


# The #863/#871 planner change adds two fields to a live plan that the frozen
# 97ad481b oracle predates -- both are the intended, hand-reviewed ADR-0020
# delta (see the ADR-0020 amendment), analogous to the debug.probe
# openocd->null allowance. Strip them from the live plan before diffing so the
# oracle BYTES stay frozen (the frame is never re-captured) while every OTHER
# field still gets a byte-exact parity check:
#   1. a `-DEXTRA_CONF_FILE=<per-core alp.conf>` arg on each NON-sysbuild
#      Zephyr slice's command (the per-core config wiring, #863). Sysbuild
#      slices deliberately do NOT carry it (Option A, #871: a bare
#      -DEXTRA_CONF_FILE lands on the sysbuild image not the app), so there is
#      nothing to strip on those.
#   2. the `# lib.loader` per-library HW-accelerator Kconfig block folded into
#      each Zephyr slice's configArtefacts alp.conf.
# KEEP IN LOCKSTEP with tan-cli's vendored copy of this comparator.
def _strip_lib_loader_block(contents):
    """Drop the `# ...lib.loader...` block (marker line through its trailing
    blank line) from an alp.conf `contents` string."""
    out, skipping = [], False
    for line in contents.split("\n"):
        if line.lstrip().startswith("#") and "lib.loader" in line:
            skipping = True
            continue
        if skipping:
            if line.strip() == "":
                skipping = False
            continue
        out.append(line)
    return "\n".join(out)


def _strip_863_additions(plan):
    """Remove the intended #863/#871 additions from a (normalized) plan dict."""
    for slice_ in plan.get("slices", []) or []:
        cmd = slice_.get("command")
        if isinstance(cmd, dict) and isinstance(cmd.get("args"), list):
            cmd["args"] = [a for a in cmd["args"]
                           if not (isinstance(a, str)
                                   and a.startswith("-DEXTRA_CONF_FILE="))]
        for art in slice_.get("configArtefacts", []) or []:
            c = art.get("contents")
            if isinstance(c, str):
                art["contents"] = _strip_lib_loader_block(c)
    return plan


def normalize_plan(plan: dict) -> dict:
    """Return a checkout/host-independent copy of a build-plan dict.

    Replaces the embedded checkout-root absolute path with ``__SDKROOT__``,
    ``sdkCommit`` with ``__SHA__``, and any ``-DPython3_EXECUTABLE=<path>``
    command arg's path with ``<PYEXE>`` -- the three fields that legitimately
    differ between the oracle's capture checkout/host and whatever
    checkout/host the live SDK is emitted from, without being a real parity
    break.

    Issue #865 flipped a LIVE plan's emit to carry literal
    ``${SDK_ROOT}``/``${PROJECT_ROOT}``/``${PYTHON}`` tokens instead of this
    checkout's absolute paths (tan-cli, PR #24, substitutes them at
    materialise time -- KEEP THIS RECONCILIATION IN LOCKSTEP with tan-cli's
    vendored twin of this comparator). The frozen 97ad481b oracle predates
    that and still carries real absolute paths, so a tokened live plan is
    mapped to the SAME normalized form the oracle collapses to rather than
    diffed as a foreign shape:

      * ``${SDK_ROOT}``     -> ``__SDKROOT__`` (the oracle's own absolute
        checkout root also collapses here, via ``_discover_sdk_root``).
      * ``${PROJECT_ROOT}`` -> ``__SDKROOT__/<project relpath>``, where
        ``<project relpath>`` is ``boardYaml``'s own directory (see
        ``_project_relpath``) -- the harness fixtures live under the SDK
        root, so this lands on the identical string the oracle's absolute
        ``sdk_root/<project relpath>/...`` prefix collapses to.
      * ``${PYTHON}``       -> already handled by the existing
        ``-DPython3_EXECUTABLE=`` regex match in `_normalize_strings`
        (it matches on the prefix alone, regardless of what follows).

    Gated on ``planPathMode == "tokened"`` -- a legacy (non-tokened) plan
    keeps the pre-#865 absolute-path normalization untouched.
    """
    if plan.get("planPathMode") == "tokened":
        project_token = f"{_SDKROOT_TOKEN}/{_project_relpath(plan)}".rstrip("/")
        normalized = _normalize_strings(plan, sdk_root=None,
                                         project_token=project_token)
    else:
        sdk_root = _discover_sdk_root(plan)
        normalized = _normalize_strings(plan, sdk_root)
    if "sdkCommit" in normalized:
        normalized["sdkCommit"] = _SHA_TOKEN
    normalized = _strip_863_additions(normalized)
    # `planPathMode` is itself a #865 addition the oracle predates (like
    # the #863/#871 fields above) -- drop it rather than diff it; the
    # token-vs-absolute SHAPE it flags is already reconciled above.
    normalized.pop("planPathMode", None)
    return normalized


def _walk_diff(path: str, old: Any, new: Any) -> Iterator[tuple[str, Any, Any]]:
    """Yield (path, old_value, new_value) for every leaf mismatch."""
    if isinstance(old, dict) and isinstance(new, dict):
        keys = sorted(set(old) | set(new))
        for key in keys:
            child_path = f"{path}.{key}" if path else key
            if key not in old:
                yield (child_path, "<missing>", new[key])
            elif key not in new:
                yield (child_path, old[key], "<missing>")
            else:
                yield from _walk_diff(child_path, old[key], new[key])
        return
    if isinstance(old, list) and isinstance(new, list):
        if len(old) != len(new):
            yield (f"{path}[len]", len(old), len(new))
            return
        for i, (old_item, new_item) in enumerate(zip(old, new)):
            yield from _walk_diff(f"{path}[{i}]", old_item, new_item)
        return
    if old != new:
        yield (path, old, new)


def diff_plans(oracle: dict, live: dict) -> tuple[list[tuple[str, Any, Any]], list[tuple[str, Any, Any]]]:
    """Split the normalized diff into (allowed, failing) delta lists."""
    allowed: list[tuple[str, Any, Any]] = []
    failing: list[tuple[str, Any, Any]] = []
    for path, old, new in _walk_diff("", oracle, live):
        is_probe_field = path.endswith(".debug.probe")
        if (is_probe_field and old == _ALLOWED_OLD_PROBE
                and new == _ALLOWED_NEW_PROBE):
            allowed.append((path, old, new))
        else:
            failing.append((path, old, new))
    return allowed, failing


def emit_live_plan(sdk_root: Path, board_yaml: str) -> dict:
    """Run the live SDK planner and return the parsed build-plan JSON.

    Mirrors `board_yaml`'s working directory assumption: `ALP_SDK_ROOT` and
    every path the emitter anchors (`appDir`, config-artefact paths) are
    resolved relative to `cwd=sdk_root`, so `board_yaml` is passed as the
    same repo-relative path the oracle's own `boardYaml` field records.
    """
    env = dict(os.environ)
    env["PYTHONPATH"] = str(sdk_root / "scripts")
    proc = subprocess.run(
        [sys.executable, "-m", "alp_orchestrate",
         "--input", board_yaml, "--emit", "build-plan"],
        cwd=sdk_root, env=env, capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise ComparatorError(
            f"emit failed for {board_yaml!r} (exit {proc.returncode}): "
            f"{proc.stderr.strip()}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise ComparatorError(
            f"emit for {board_yaml!r} did not produce valid JSON: {e}") from e


def _discover_boards(oracle_dir: Path) -> list[str]:
    return sorted(p.stem.removesuffix(".build-plan")
                  for p in oracle_dir.glob("*.build-plan.json"))


def run(sdk: Path, oracle_dir: Path, boards: list[str]) -> bool:
    """Run the seam-1 comparison for `boards`; return True iff all pass."""
    all_ok = True
    for board in boards:
        oracle_path = oracle_dir / f"{board}.build-plan.json"
        if not oracle_path.is_file():
            print(f"FAIL {board}: no oracle fixture at {oracle_path}")
            all_ok = False
            continue

        oracle_plan = json.loads(oracle_path.read_text())
        board_yaml = oracle_plan.get("boardYaml")
        if not board_yaml:
            print(f"FAIL {board}: oracle fixture has no boardYaml field")
            all_ok = False
            continue

        try:
            live_plan = emit_live_plan(sdk, board_yaml)
        except ComparatorError as e:
            print(f"FAIL {board}: {e}")
            all_ok = False
            continue

        allowed, failing = diff_plans(normalize_plan(oracle_plan),
                                       normalize_plan(live_plan))

        if failing:
            print(f"FAIL {board}: {len(failing)} disallowed diff(s)")
            for path, old, new in failing:
                print(f"    {path}: oracle={old!r} live={new!r}")
            all_ok = False
        else:
            note = f" ({len(allowed)} allowed debug.probe delta)" if allowed else ""
            print(f"PASS {board}{note}")
        for path, old, new in allowed:
            print(f"    (allowed) {path}: oracle={old!r} live={new!r}")

    return all_ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path,
                         help="Path to the alp-sdk checkout to emit live "
                              "build-plans from.")
    parser.add_argument("--oracle", type=Path,
                         default=Path(__file__).parent / "oracle",
                         help="Directory of frozen oracle *.build-plan.json "
                              "fixtures (default: tests/parity/oracle next "
                              "to this script).")
    parser.add_argument("--boards", nargs="+", default=None,
                         help="Board keys to check (oracle filename minus "
                              "'.build-plan.json', e.g. 'audio_i2s-tone'). "
                              "Default: every fixture in --oracle.")
    args = parser.parse_args(argv)

    sdk_root = args.sdk.resolve()
    if not (sdk_root / "scripts" / "alp_orchestrate").is_dir():
        print(f"error: {sdk_root} does not look like an alp-sdk checkout "
              f"(no scripts/alp_orchestrate)", file=sys.stderr)
        return 2

    oracle_dir = args.oracle.resolve()
    boards = args.boards or _discover_boards(oracle_dir)
    if not boards:
        print(f"error: no oracle fixtures found in {oracle_dir}", file=sys.stderr)
        return 2

    ok = run(sdk_root, oracle_dir, boards)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

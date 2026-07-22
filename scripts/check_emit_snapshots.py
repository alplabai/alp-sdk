#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Byte-for-byte `--emit` snapshot regression gate.

The planner of record (`scripts/alp_orchestrate.py`) is being modularized
into a layered package (issue #285).  A refactor must change *shape*, never
*behaviour*: the `--emit` surfaces the IDE/CLI consume (ADR 0014) have to
stay byte-identical.  This gate pins them.

For each (board.yaml, emit-mode) case it runs the emitter and compares the
output, byte-for-byte, to a committed golden under
`tests/fixtures/emit-snapshots/`. Machine-specific absolute paths (the SDK
checkout root and orchestrator Python) are normalised so the goldens are
portable.

This is the explicit behaviour-pin the #285 extractions land against: an
extraction that moves code but keeps `--emit` identical is green here; one
that alters a byte fails immediately.

Usage:

    python3 scripts/check_emit_snapshots.py            # verify against goldens
    python3 scripts/check_emit_snapshots.py --update     # (re)write the goldens
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAP_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"
# The orchestrator is now a package; invoke it as a module.  scripts/ goes on
# PYTHONPATH so the package + its `alp_project` sibling import both resolve
# (replaces the old file-path call to scripts/alp_orchestrate.py).
ORCH = [sys.executable, "-m", "alp_orchestrate"]
PROJ = [sys.executable, str(REPO / "scripts" / "alp_project.py")]

# (snapshot id, tool, board.yaml, emit mode).  Deterministic, write-free
# emits across the three vendor families -- the contract surfaces a refactor
# must preserve.  build-plan carries the SDK-root abs path (normalised below).
#
# Two families of cases:
#   * ORCH (`-m alp_orchestrate`) -- the multicore system-manifest / build-plan
#     surfaces.  build-plan embeds each slice's rendered `alp.conf`, so it also
#     pins `kconfig.py::_slice_alp_conf` byte-for-byte.
#   * PROJ (`alp_project.py`) -- the single-core `--emit` surfaces rendered by
#     `alp_project_emit.py` (dts overlay, native-sim overlay, hw-info header,
#     west libraries, carrier route/netlist) plus `zephyr-conf` (alp.conf for a
#     single core) and `os-topology` (the `loader.py::load_board_yaml`
#     resolution).  These are the seams the #673 alp_project_emit / kconfig /
#     loader splits move; pinning them here is the Phase-0 characterization net
#     those extractions land against.
_PROJ_BOARDS = [
    # (short id, board.yaml).  One AEN E8 SKU, one V2N SKU, one native_sim.
    ("aen", "examples/aen/aen-analog-validate/board.yaml"),
    ("v2n", "examples/v2n/v2n-power-monitor/board.yaml"),
    ("nsim", "examples/peripheral-io/spi-slave/board.yaml"),
]
# Project-wide emit modes valid for every board above (zephyr-targeted).
_PROJ_MODES = [
    "zephyr-conf", "dts-overlay", "native-sim-overlay",
    "hw-info-h", "west-libraries",
]
# Carrier routing / topology emits -- meaningful only for the SoM boards with a
# resolved carrier (the native_sim board has none), so kept off `nsim`.
_PROJ_CARRIER_MODES = ["os-topology", "composed-route-table", "carrier-netlist"]

CASES = [
    ("rpmsg-aen.system-manifest",   ORCH, "examples/multicore/rpmsg-aen/board.yaml",            "system-manifest"),
    ("rpmsg-aen.build-plan",        ORCH, "examples/multicore/rpmsg-aen/board.yaml",            "build-plan"),
    ("rpmsg-v2n.system-manifest",   ORCH, "examples/multicore/rpmsg-v2n/board.yaml",            "system-manifest"),
    ("rpmsg-v2n.build-plan",        ORCH, "examples/multicore/rpmsg-v2n/board.yaml",            "build-plan"),
    ("rpmsg-imx93.system-manifest", ORCH, "examples/multicore/rpmsg-imx93/board.yaml",          "system-manifest"),
    ("rpmsg-imx93.build-plan",      ORCH, "examples/multicore/rpmsg-imx93/board.yaml",          "build-plan"),
    ("hetero-offload.system-manifest", ORCH, "examples/multicore/heterogeneous-offload/board.yaml", "system-manifest"),
    ("hetero-offload.build-plan",      ORCH, "examples/multicore/heterogeneous-offload/board.yaml", "build-plan"),
]
for _bid, _board in _PROJ_BOARDS:
    for _mode in _PROJ_MODES:
        CASES.append((f"proj-{_bid}.{_mode}", PROJ, _board, _mode))
    if _bid != "nsim":
        for _mode in _PROJ_CARRIER_MODES:
            CASES.append((f"proj-{_bid}.{_mode}", PROJ, _board, _mode))

# --emit scaffold (issue #864): a render_to_envelope(template, sku) render is
# a pure function of --template/--sku, not of a board.yaml -- `board` below
# is unused (alp_project.py dispatches scaffold before --input is ever
# read), kept as a real path only so `_emit`'s fixed `--input <board> --emit
# <mode>` invocation shape needs no special-casing. Pins the minimal
# template's E1M-V2N101 SKU-substitution (som.sku + preset) byte-for-byte.
CASES.append((
    "scaffold.minimal-v2n101", PROJ,
    "examples/peripheral-io/hello-world/board.yaml", "scaffold",
    ("--template", "minimal", "--sku", "E1M-V2N101"),
))
# peripheral's canonical board.yaml carries trailing inline comments on
# BOTH its `sku:` and `preset:` lines describing the AEN801/e1m-evk
# default (Fable review finding) -- this pins that the substitution
# drops the stale comment along with the value, not just the token.
# E1M-V2N101 is back in `peripheral`'s `supported.som_skus` (issue
# #876: `_derive_pin_renames` re-derives the `pins:` block's
# E1M-EVK-only pads to their E1M-X-EVK equivalents via the boards'
# shared `board_alias:` join, so this is a buildable scaffold again,
# not the dead-on-arrival one #864/#877 dropped this case for).
CASES.append((
    "scaffold.peripheral-v2n101", PROJ,
    "examples/peripheral-io/gpio-button-led/board.yaml", "scaffold",
    ("--template", "peripheral", "--sku", "E1M-V2N101"),
))
# sensor/edge-ai both re-derive a single `E1M_I2C0` -> `E1M_X_I2C0`
# pin (issue #876) -- new golden coverage, no prior case existed.
CASES.append((
    "scaffold.sensor-v2n101", PROJ,
    "examples/peripheral-io/i2c-master/board.yaml", "scaffold",
    ("--template", "sensor", "--sku", "E1M-V2N101"),
))
CASES.append((
    "scaffold.edge-ai-v2n101", PROJ,
    "examples/ai/cold-chain-monitor/board.yaml", "scaffold",
    ("--template", "edge-ai", "--sku", "E1M-V2N101"),
))


def _normalize_path(text: str, path: str, token: str) -> str:
    """Replace raw, JSON-escaped, and forward-slash forms of one path."""
    for variant in (
        path.replace("\\", "\\\\"),  # JSON-escaped Windows path (do first)
        path,                            # raw path (POSIX / Windows)
        path.replace("\\", "/"),      # forward-slash Windows path
    ):
        text = text.replace(variant, token)
    return text


# A tokenised path's *tail* keeps the host separator: replacing the checkout
# root turns ``C:\...\alp-sdk\firmware\x`` into ``<SDK_ROOT>\\firmware\\x``
# (backslashes doubled by the JSON encoder) while the goldens -- written on
# Linux -- carry ``<SDK_ROOT>/firmware/x``.  Rewrite the separators that follow
# a token so the two hosts agree.  Only a token-led run is touched, so JSON
# escapes elsewhere in the payload are left alone; on POSIX the run contains no
# backslashes and the substitution is a no-op.
_TOKEN_TAIL_RE = re.compile(r"(<SDK_ROOT>|<PYTHON_EXECUTABLE>)((?:\\{1,2}[^\\\"\s,]+)+)")


def _normalize_token_tails(text: str) -> str:
    """Force forward slashes inside paths already reduced to a token."""
    return _TOKEN_TAIL_RE.sub(
        lambda m: m.group(1) + re.sub(r"\\{1,2}", "/", m.group(2)), text)


# `sdkCommit` is the emitting checkout's short git commit (build-plan
# provenance) -- it moves every commit, and a host without git emits `null`
# instead of a hash, so the value can never be byte-stable across the two
# hosts that write vs. check the goldens.  Reduce any value (hash or `null`)
# to a token.  `sdkVersion` is deliberately left real: it comes from
# metadata/sdk_version.yaml and only moves on a release bump, so it stays a
# meaningful, stable part of the golden.
_SDK_COMMIT_RE = re.compile(r'("sdkCommit":\s*)("[0-9a-f]+"|null)')


def _normalize_provenance(text: str) -> str:
    """Replace the volatile per-commit ``sdkCommit`` value with a stable token."""
    return _SDK_COMMIT_RE.sub(r'\1"<SDK_COMMIT>"', text)


def _normalize_host_paths(text: str, repo: str, python: str) -> str:
    """Replace host-specific SDK and Python paths with stable tokens.

    ``build-plan`` output is JSON, so a Windows checkout root
    (``C:\\Users\\...\\alp-sdk``) appears with its backslashes doubled inside
    JSON strings.  Normalising only the raw ``repo`` string leaves those
    JSON-escaped absolute paths in the emitted snapshot, which breaks the
    byte-for-byte gate on Windows even though the committed goldens use
    ``<SDK_ROOT>``.  Cover the JSON-escaped form and the forward-slash form as
    well as the raw path. The Python executable added to Zephyr commands by
    issue #787 needs the same treatment. On POSIX the variants collapse to
    their raw paths, so the Linux output is unchanged apart from tokenisation.
    The per-commit ``sdkCommit`` provenance value is tokenised too so the
    goldens don't churn on every commit.
    """
    text = _normalize_path(text, repo, "<SDK_ROOT>")
    text = _normalize_path(text, python, "<PYTHON_EXECUTABLE>")
    text = _normalize_provenance(text)
    return _normalize_token_tails(text)


def _emit(tool: Path, board: str, mode: str, extra: tuple[str, ...] = ()) -> str:
    """Run the emitter from the repo root; normalise the SDK-root path.

    `extra` appends extra CLI args after `--emit <mode>` -- e.g.
    scaffold's `--template <id> --sku <SKU>` (issue #864).
    """
    env = {**os.environ}
    scripts_dir = str(REPO / "scripts")
    env["PYTHONPATH"] = (
        scripts_dir + os.pathsep + env["PYTHONPATH"]
        if env.get("PYTHONPATH") else scripts_dir
    )
    rv = subprocess.run(
        [*tool, "--input", board, "--emit", mode, *extra],
        capture_output=True, text=True, cwd=REPO, check=False, env=env)
    if rv.returncode != 0:
        raise SystemExit(f"check_emit_snapshots: emit failed for {board} "
                         f"--emit {mode} (rc={rv.returncode}):\n{rv.stderr}")
    return _normalize_host_paths(rv.stdout, str(REPO), sys.executable)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the golden snapshots instead of checking")
    args = ap.parse_args()
    SNAP_DIR.mkdir(parents=True, exist_ok=True)

    stale: list[str] = []
    for case in CASES:
        snap_id, tool, board, mode, *rest = case
        extra: tuple[str, ...] = rest[0] if rest else ()
        got = _emit(tool, board, mode, extra)
        golden = SNAP_DIR / f"{snap_id}.snap"
        if args.update:
            golden.write_text(got, encoding="utf-8")
            print(f"wrote {golden.relative_to(REPO)}")
            continue
        if not golden.is_file():
            stale.append(f"{snap_id}: no golden (run --update)")
            print(f"MISS {snap_id}")
        elif golden.read_text(encoding="utf-8") != got:
            stale.append(f"{snap_id}: emit differs from golden")
            print(f"DIFF {snap_id}")
        else:
            print(f"OK   {snap_id}")

    if args.update:
        print(f"\nupdated {len(CASES)} snapshot(s).")
        return 0
    if stale:
        print(f"\ncheck_emit_snapshots: {len(stale)} snapshot(s) changed -- a "
              f"refactor must keep --emit byte-identical.  If the change is "
              f"intentional, re-run with --update and review the diff.",
              file=sys.stderr)
        for s in stale:
            print(f"  · {s}", file=sys.stderr)
        return 1
    print(f"\ncheck_emit_snapshots: {len(CASES)} --emit surface(s) byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

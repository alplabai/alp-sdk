#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build-plan emission -- the Wave C consumer contract.

`emit_build_plan` renders the machine-readable JSON build plan the `alp` CLI
materialises; the shared helpers `_slice_build_dir` / `_slice_config_artefact` /
`_shared_artefacts` are the single source the Orchestrator's materialise path and
the plan MUST agree on byte-for-byte (the CLI reads what the Orchestrator writes).
Extracted as the #285 build-plan emit seam. The per-slice config emitters come
from kconfig.py, the header/secure artefacts from headers.py / secure.py; the
orchestrator-side slice-command bits (_slice_command, STOCK_SHIM_APP) are
lazy-imported from the package (they stay inline until orchestrator.py).
"""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from typing import Any, Optional

from .headers import emit_dts_partitions, emit_dts_reservations, emit_ipc_contract_h
from .kconfig import _slice_alp_conf, _slice_cmake_args, _slice_local_conf
from .models import BoardProject, Slice
from .paths import REPO
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf


def _slice_build_dir(build_root: Path, slice_: Slice) -> Path:
    """Per-slice build directory: build/<core>-<os>/."""
    return Path(build_root) / f"{slice_.core_id}-{slice_.os}"


def _slice_config_artefact(
    project: BoardProject,
    slice_: Slice,
) -> Optional[tuple[str, str]]:
    """(filename, contents) of the slice's config artefact, or None
    when the os has none.

    Single source for both the Orchestrator's materialise step and
    `emit_build_plan` -- the two MUST agree byte-for-byte (the CLI
    consumer byte-writes the plan's contents and trusts them to match
    what we'd write ourselves).
    """
    if slice_.os == "zephyr":
        return ("alp.conf", _slice_alp_conf(project, slice_))
    if slice_.os == "yocto":
        return ("local.conf", _slice_local_conf(project, slice_))
    if slice_.os == "baremetal":
        return ("cmake-args.txt", _slice_cmake_args(project, slice_))
    return None


def _shared_artefacts(
    project: BoardProject,
    build_root: Path,
) -> list[tuple[Path, str]]:
    """(path, contents) of every shared generated artefact.

    Single source for `_materialise_shared` and `emit_build_plan`
    (same byte-parity contract as `_slice_config_artefact`).
    Conditional artefacts (sysbuild / TF-M) follow absence-emits-
    nothing: they only appear when their emit is non-empty.
    """
    build_root = Path(build_root)
    gen = build_root / "generated"
    out: list[tuple[Path, str]] = [
        # `<alp/system_ipc.h>` is the canonical include path consumers
        # use (see include/alp/rpc.h §usage and the per-slice main.c
        # references) -- the header sits in an `alp/` subdir so slice
        # CMakeLists add `generated/` straight to the include path.
        (gen / "alp" / "system_ipc.h", emit_ipc_contract_h(project)),
        (gen / "dts-reservations.dtsi", emit_dts_reservations(project)),
        # Apps that don't declare storage[] still get a stub file with
        # a "nothing to emit" comment so downstream #include resolves.
        (gen / "dts-partitions.dtsi", emit_dts_partitions(project)),
    ]
    sysbuild_conf = emit_sysbuild_conf(project)
    if sysbuild_conf:
        out.append((build_root / "alp_sysbuild.conf", sysbuild_conf))
    tfm_conf = emit_tfm_sysbuild_conf(project)
    if tfm_conf:
        out.append((build_root / "sysbuild" / "tfm" / "tfm.conf",
                    tfm_conf))
    return out


def emit_build_plan(
    project: BoardProject,
    *,
    board_yaml: Path,
    build_root: Path,
) -> str:
    """Emit the machine-readable build plan as JSON (Wave C contract).

    Consumed by the `alp` CLI (alp-sdk-vscode), which materialises the
    plan's files, runs each slice's command, and owns scheduling /
    caching / progress UX on top -- instead of re-implementing this
    planner.  Agreed 2026-06-04 with the alp-sdk-vscode team; their
    docs/PROPOSAL-alp-build-core.md records the settlement.

    Contract notes (locked with the consumer -- bump `schemaVersion`
    and flag in the CHANGELOG before changing the shape):

      * camelCase keys; `schemaVersion` is independent of board.yaml's
        schema version.
      * Every artefact carries its `contents` so the consumer's
        materialise step stays pure IO.  `_shared_artefacts` /
        `_slice_config_artefact` are the single sources both this emit
        and the Orchestrator's own materialise step read, so the two
        cannot drift.
      * No `inputHash` (the consumer computes its own cache key over
        the plan) and no `sequential` (parallelism policy belongs to
        the consumer's scheduler).
      * One slice per non-`off` core, sorted by coreId.  A slice this
        script cannot build yet (e.g. no `app:`) is carried with
        `command: null` plus a `no-command` warning -- never dropped,
        so the consumer can still report the core.
      * Write-free: nothing is created on disk.  (Command resolution
        stats the app dir to pick the CMakeLists.txt convention --
        read-only, same as the build itself.)
    """
    # Orchestrator-side (stay inline until orchestrator.py); lazy to avoid
    # a buildplan<->package import cycle.
    from .orchestrator import STOCK_SHIM_APP, _slice_command
    build_root = Path(build_root)
    slices_out: list[dict[str, Any]] = []
    warnings: list[dict[str, Any]] = []

    for slice_ in sorted(project.cores.values(),
                         key=lambda s: s.core_id):
        if slice_.os == "off":
            continue
        build_dir = _slice_build_dir(build_root, slice_)
        # `replace` keeps this emit side-effect free: _slice_command
        # reads `build_dir` off the slice (baremetal -B), and the
        # project's own Slice objects must stay untouched.
        cmd = _slice_command(
            project, replace(slice_, build_dir=build_dir))
        if cmd is None:
            if slice_.os == "zephyr" and slice_.app == STOCK_SHIM_APP:
                warnings.append({
                    "code":    "stock-shim-unimplemented",
                    "coreId":  slice_.core_id,
                    "message": (f"core '{slice_.core_id}' uses the stock "
                                f"M-core shim (app: {STOCK_SHIM_APP}); its "
                                f"image body is not in the SDK tree yet "
                                f"(issue #49). Override "
                                f"cores.{slice_.core_id}.app with a real app "
                                f"to build this core."),
                })
            else:
                warnings.append({
                    "code":    "no-command",
                    "coreId":  slice_.core_id,
                    "message": (f"no build command for core "
                                f"'{slice_.core_id}' (os: {slice_.os}) "
                                f"-- missing app/board/image"),
                })
        config_artefacts: list[dict[str, str]] = []
        artefact = _slice_config_artefact(project, slice_)
        if artefact is not None:
            name, contents = artefact
            config_artefacts.append({
                "path":     (build_dir / name).as_posix(),
                "contents": contents,
            })
        slices_out.append({
            "coreId":          slice_.core_id,
            "backend":         slice_.os,
            "buildDir":        build_dir.as_posix(),
            "configArtefacts": config_artefacts,
            "command": None if cmd is None else {
                "tool": cmd[0],
                "args": cmd[1:],
                "cwd":  build_dir.as_posix(),
            },
            # Native host-path form: the value is handed to the slice
            # subprocess environment verbatim.
            "env": {"ALP_SDK_ROOT": str(REPO)},
        })

    plan: dict[str, Any] = {
        "schemaVersion":   1,
        "generatedBy":     "scripts/alp_orchestrate.py",
        "boardYaml":       Path(board_yaml).as_posix(),
        "sku":             project.sku,
        "buildRoot":       build_root.as_posix(),
        "slices":          slices_out,
        "sharedArtefacts": [
            {"path": p.as_posix(), "contents": c}
            for p, c in _shared_artefacts(project, build_root)
        ],
        "warnings":        warnings,
    }
    return json.dumps(plan, indent=2) + "\n"

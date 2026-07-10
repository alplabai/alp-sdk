# SPDX-License-Identifier: Apache-2.0
"""The orchestrator CLI: argparse + main().

Kept in-package (imported by __init__ for the public `main` re-export, and by
__main__ as the `python -m alp_orchestrate` entry) so nothing has to import
__main__ -- importing __main__ re-enters the package under runpy and warns.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterable, Optional

from . import (
    Orchestrator,
    OrchestratorError,
    emit_build_plan,
    emit_dts_partitions,
    emit_dts_reservations,
    emit_ipc_contract_h,
    emit_storage_mounts_c,
    emit_system_manifest,
    emit_tfm_sysbuild_conf,
    load_board_yaml,
)


def main(argv: Optional[Iterable[str]] = None) -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="Fan-out orchestrator for board.yaml.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml.")
    parser.add_argument("--build-root", type=Path,
                        default=Path("build"),
                        help="Build root directory.")
    parser.add_argument("--core", default=None,
                        help="Limit fan-out to a single core ID.")
    parser.add_argument("--no-parallel", action="store_true",
                        help="Force sequential dispatch.")
    parser.add_argument("--emit", default=None,
                        choices=["system-manifest", "ipc-contract-h",
                                 "dts-reservations", "dts-partitions",
                                 "storage-mounts-c",
                                 "tfm-sysbuild-conf", "build-plan"],
                        help="Skip the build; just emit one of the "
                             "generated artefacts to stdout.")
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        project = load_board_yaml(args.input)
    except OrchestratorError as e:
        print(f"alp-orchestrate: {e}", file=sys.stderr)
        return 1

    if args.emit:
        try:
            if args.emit == "system-manifest":
                sys.stdout.write(emit_system_manifest(project))
            elif args.emit == "ipc-contract-h":
                sys.stdout.write(emit_ipc_contract_h(project))
            elif args.emit == "dts-reservations":
                sys.stdout.write(emit_dts_reservations(project))
            elif args.emit == "dts-partitions":
                sys.stdout.write(emit_dts_partitions(project))
            elif args.emit == "storage-mounts-c":
                sys.stdout.write(emit_storage_mounts_c(project))
            elif args.emit == "tfm-sysbuild-conf":
                sys.stdout.write(emit_tfm_sysbuild_conf(project))
            elif args.emit == "build-plan":
                sys.stdout.write(emit_build_plan(
                    project, board_yaml=args.input,
                    build_root=args.build_root))
        except OrchestratorError as e:
            print(f"alp-orchestrate: {e}", file=sys.stderr)
            return 1
        return 0

    orchestrator = Orchestrator(project, args.build_root,
                                board_yaml=args.input)
    try:
        manifest = orchestrator.fan_out(
            only_core=args.core, parallel=not args.no_parallel)
    except OrchestratorError as e:
        print(f"alp-orchestrate: {e}", file=sys.stderr)
        return 1

    # Surface per-slice status to the console.
    failed = 0
    for s in manifest.slices:
        marker = {
            "ok":      "[OK ]",
            "failed":  "[FAIL]",
            "skipped": "[SKIP]",
            "pending": "[??? ]",
        }.get(s.status, "[??? ]")
        extra = f" -- {s.reason}" if s.reason else ""
        print(f"{marker} {s.core_id}/{s.os}{extra}")
        if s.status == "failed":
            failed += 1
    print(f"alp-orchestrate: manifest at "
          f"{(args.build_root / 'system-manifest.yaml')}")
    return 1 if failed > 0 else 0

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
    OrchestratorError,
    emit_build_plan,
    emit_dts_partitions,
    emit_dts_reservations,
    emit_ipc_contract_h,
    emit_kconfig,
    emit_storage_mounts_c,
    emit_system_manifest,
    emit_tfm_sysbuild_conf,
    load_board_yaml,
)


def main(argv: Optional[Iterable[str]] = None) -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="Planner/emit CLI for board.yaml (the executor was "
                     "retired -- ADR-0020 Phase 4).")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml.")
    parser.add_argument("--build-root", type=Path,
                        default=Path("build"),
                        help="Build root directory (used by --emit build-plan).")
    parser.add_argument("--core", default=None,
                        help="Core id to scope a per-core emit mode to "
                             "(required by --emit kconfig; every other "
                             "mode ignores it).")
    parser.add_argument("--emit", default=None,
                        choices=["system-manifest", "ipc-contract-h",
                                 "dts-reservations", "dts-partitions",
                                 "storage-mounts-c",
                                 "tfm-sysbuild-conf", "build-plan",
                                 "kconfig"],
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
            elif args.emit == "kconfig":
                sys.stdout.write(emit_kconfig(project, args.core))
        except OrchestratorError as e:
            print(f"alp-orchestrate: {e}", file=sys.stderr)
            return 1
        return 0

    # ADR-0020 Phase 4 (preview): the SDK-side executor was retired --
    # this module only plans/emits now.  Building is an external
    # consumer's job (against `--emit build-plan`'s JSON contract).
    print("alp-orchestrate: no executor -- pass --emit to print a "
          "generated artefact (e.g. --emit build-plan)", file=sys.stderr)
    return 2

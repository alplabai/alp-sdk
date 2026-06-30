# SPDX-License-Identifier: Apache-2.0
"""`west alp-emit` -- print one generated config artefact from board.yaml.

A read-only, west-native front door to the orchestrator's `--emit` surface (the
ADR-0014 seam the CLI + IDE consume): the system-manifest, the build-plan, and
the per-slice dts/conf shapes. It runs no build and writes nothing -- it just
fans `board.yaml` through `alp_orchestrate --emit <mode>` and prints the result
to stdout, so you can inspect exactly what a tool would consume.

Examples:
    west alp-emit system-manifest
    west alp-emit build-plan --board-yaml path/to/board.yaml
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from west import log                              # type: ignore[import-not-found]
from west.commands import WestCommand             # type: ignore[import-not-found]

# Allow `from _alp_common import ...` regardless of how west loads the wrapper.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import (                         # noqa: E402
    env_with_sdk,
    find_sdk_root,
    python_exe,
    resolve_board_yaml,
)

# Mirror the orchestrator's --emit choices (alp_orchestrate.cli).  Kept here as
# the user-facing list; the orchestrator validates it again, so a drift just
# surfaces as its error, never a silent wrong emit.
_EMIT_MODES = [
    "system-manifest",
    "ipc-contract-h",
    "dts-reservations",
    "dts-partitions",
    "storage-mounts-c",
    "tfm-sysbuild-conf",
    "build-plan",
]


class AlpEmit(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-emit",
            "Print a generated config artefact from board.yaml (no build)",
            ("\n".join(__doc__.splitlines()[2:])
             if __doc__ else ""),
        )

    def do_add_parser(self, parser_adder):       # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument(
            "mode", choices=_EMIT_MODES,
            help="Which generated artefact to emit to stdout.")
        parser.add_argument(
            "--board-yaml", default=None,
            help="Path to board.yaml (default: ./board.yaml).")
        parser.add_argument(
            "--build-root", default=None,
            help="Build root used for build-plan slice paths (default: build).")
        return parser

    def do_run(self, args, _unknown):            # type: ignore[no-untyped-def]
        sdk_root = find_sdk_root()
        if sdk_root is None:
            log.die("alp-emit: not inside an alp-sdk workspace "
                    "(set ALP_SDK_ROOT or run from the SDK).")

        board_yaml = resolve_board_yaml(Path.cwd(), args.board_yaml)
        if not board_yaml.is_file():
            log.die(f"alp-emit: board.yaml not found at {board_yaml}")

        # Same module-form invocation as `west alp-build`; env_with_sdk() puts
        # scripts/ on PYTHONPATH so `-m alp_orchestrate` resolves.
        cmd = [
            python_exe(),
            "-m", "alp_orchestrate",
            "--input", str(board_yaml),
            "--emit", args.mode,
        ]
        if args.build_root:
            cmd += ["--build-root", args.build_root]
        return subprocess.call(cmd, env=env_with_sdk(sdk_root))

# SPDX-License-Identifier: Apache-2.0
"""
`west alp-build` -- fan out a board.yaml project into one
build slice per non-`off` core.

Replaces the v1 single-OS `west alp-build` (which only ever
invoked Zephyr's `west build`) with a heterogeneous-aware driver
that dispatches Yocto + Zephyr + baremetal slices in parallel via
scripts/alp_orchestrate.py.

Customer flow (per spec §5.4):

    # Default -- build every non-off slice declared in board.yaml:
    west alp-build examples/rpmsg-v2n

    # Iterate on one slice only (skips Yocto's hour-long rebuild):
    west alp-build examples/rpmsg-v2n --core m33_sm

    # Force a sequential dispatch (easier to read on Windows):
    west alp-build examples/rpmsg-v2n --no-parallel

The orchestrator writes:

  - <app>/build/system-manifest.yaml      (single source of truth)
  - <app>/build/generated/alp_system_ipc.h
  - <app>/build/generated/dts-reservations.dtsi
  - <app>/build/<core>-<os>/...           (per-slice build dirs)
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from west import log                                # type: ignore[import-not-found]
from west.commands import WestCommand               # type: ignore[import-not-found]

# Allow `from _alp_common import ...` regardless of how west loads
# the wrapper.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import (                           # noqa: E402
    env_with_sdk,
    find_sdk_root,
    python_exe,
    resolve_board_yaml,
)


class AlpBuild(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-build",
            "Fan out a board.yaml project into per-core build slices",
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
            "app_path",
            help="Path to the application source directory.")
        parser.add_argument(
            "--board-yaml", default=None,
            help="Override path to board.yaml (default: <app_path>/board.yaml).")
        parser.add_argument(
            "--build-root", default=None,
            help="Override the build root (default: <app_path>/build).")
        parser.add_argument(
            "--core", default=None,
            help="Limit fan-out to one core ID.")
        parser.add_argument(
            "--no-validate", action="store_true",
            help="Skip the validate_board_yaml.py pre-flight check.")
        parser.add_argument(
            "--no-parallel", action="store_true",
            help="Force sequential slice dispatch (easier on Windows).")
        return parser

    def do_run(self, args, _unknown):           # type: ignore[no-untyped-def]
        sdk_root = find_sdk_root()
        if sdk_root is None:
            log.die("Cannot locate alp-sdk root.  Set ALP_SDK_ROOT or "
                    "import alp-sdk as a Zephyr module.")
            return 1

        app_path = Path(args.app_path).resolve()
        if not app_path.is_dir():
            log.die(f"App path not found: {app_path}")
            return 1

        board_yaml = resolve_board_yaml(app_path, args.board_yaml)
        if not board_yaml.is_file():
            log.die(f"board.yaml not found at {board_yaml}")
            return 1

        build_root = (Path(args.build_root).resolve()
                      if args.build_root
                      else app_path / "build")

        python = python_exe()
        env = env_with_sdk(sdk_root)

        # Step 1 -- validate (defaults: v2 schema; v1 inputs fail).
        if not args.no_validate:
            log.inf(f"alp-build: validating {board_yaml}")
            rv = subprocess.call([
                python,
                str(sdk_root / "scripts" / "validate_board_yaml.py"),
                "--input", str(board_yaml),
            ], env=env)
            if rv != 0:
                log.die(f"validate_board_yaml.py failed (rv={rv})")
                return rv

        # Step 2 -- fan out via the orchestrator.
        cmd = [
            python,
            str(sdk_root / "scripts" / "alp_orchestrate.py"),
            "--input", str(board_yaml),
            "--build-root", str(build_root),
        ]
        if args.core:
            cmd += ["--core", args.core]
        if args.no_parallel:
            cmd += ["--no-parallel"]
        log.inf(f"alp-build: $ {' '.join(cmd)}")
        return subprocess.call(cmd, env=env, cwd=str(app_path))

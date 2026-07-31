# SPDX-License-Identifier: Apache-2.0
"""`west alp-emit` -- print one generated config artefact from board.yaml.

A read-only, west-native front door to the planner's `--emit` surface (the
ADR-0014 seam the CLI + IDE consume): the system-manifest, the build-plan, and
the per-slice dts/conf shapes. It runs no build and writes nothing -- it just
fans `board.yaml` through the planner and prints the result to stdout, so you
can inspect exactly what a tool would consume.

The planner is tan's, not the SDK's (ADR-0020): this spawns
`python -m tan.planner_cli`, the relocated argv entry for the package that used
to be `scripts/alp_orchestrate/`. Same flags, same eight modes, same bytes on
stdout -- `tan/planner/cli.py` IS the module that moved.

NOT `tan generate`: that command and this one cover disjoint artefact sets
(docs/cli.md's "disjoint artefact sets" note). `tan generate --target` reaches
the board-derived config targets and writes them to FILES via `--output`; none
of the eight modes below is one of them, and no `tan` subcommand streams them
to stdout.

Requires the tan Python distribution (`pip install alp-tan`) -- a frozen `tan`
binary alone cannot serve this, since `python -m` needs the importable package.

Examples:
    west alp-emit system-manifest
    west alp-emit build-plan --board-yaml path/to/board.yaml
"""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

try:
    from west import log                          # type: ignore[import-not-found]
    from west.commands import WestCommand         # type: ignore[import-not-found]
    _HAVE_WEST = True
except ImportError:  # pragma: no cover - unit tests run without west installed
    # Import-safe without west so `_EMIT_MODES` stays importable by the
    # alp/west emit-parity test (tests/scripts/test_alp_cli_emit.py).
    _HAVE_WEST = False

    class WestCommand:  # type: ignore[no-redef]
        """Minimal shim so this module imports without west (unit tests)."""

        def __init__(self, *args, **kwargs):  # noqa: D401,ANN002,ANN003
            pass

    class _StubLog:
        """west.log stand-in; only die() is reachable outside west."""

        @staticmethod
        def inf(msg: str) -> None: print(msg)
        @staticmethod
        def die(msg: str) -> None:
            print(f"FATAL: {msg}", file=sys.stderr)
            sys.exit(1)

    log = _StubLog()  # type: ignore[assignment]

# Allow `from _alp_common import ...` regardless of how west loads the wrapper.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import (                         # noqa: E402
    env_with_sdk,
    find_sdk_root,
    python_exe,
    resolve_board_yaml,
)

# The planner's argv entry: tan's relocation of `alp_orchestrate/__main__.py`,
# deliberately a sibling module rather than `tan.planner.__main__` (its own
# docstring: `python -m <pkg>` imports the PACKAGE first, which would bind the
# SDK root before anything could set it).  A module form, not the `tan`
# executable, because no `tan` subcommand exposes the modes below.
_PLANNER_MODULE = "tan.planner_cli"

#: What to tell a user who has no importable tan (see the module docstring).
_PLANNER_INSTALL_HINT = "pip install alp-tan"


def _planner_importable() -> bool:
    """Whether `_PLANNER_MODULE` can be imported by this interpreter.

    `find_spec` on a SUBMODULE imports its parent package to read
    `__path__`, so a missing `tan` RAISES ModuleNotFoundError here rather
    than returning None -- which is the whole case this probe exists for.
    ValueError covers a parent that is importable but has no `__spec__`.
    """
    try:
        return importlib.util.find_spec(_PLANNER_MODULE) is not None
    except (ImportError, ValueError):
        return False


# Mirror the planner's --emit choices (tan/planner/cli.py, the relocated
# alp_orchestrate.cli).  Kept here as the user-facing list; the planner
# validates it again, so a drift just surfaces as its error, never a silent
# wrong emit.
_EMIT_MODES = [
    "system-manifest",
    "ipc-contract-h",
    "dts-reservations",
    "dts-partitions",
    "storage-mounts-c",
    "tfm-sysbuild-conf",
    "build-plan",
    "kconfig",
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
        parser.add_argument(
            "--core", default=None,
            help="Core id (required by --emit kconfig; every other mode "
                 "ignores it).")
        return parser

    def do_run(self, args, _unknown):            # type: ignore[no-untyped-def]
        sdk_root = find_sdk_root()
        if sdk_root is None:
            log.die("alp-emit: not inside an alp-sdk workspace "
                    "(set ALP_SDK_ROOT or run from the SDK).")

        board_yaml = resolve_board_yaml(Path.cwd(), args.board_yaml)
        if not board_yaml.is_file():
            log.die(f"alp-emit: board.yaml not found at {board_yaml}")

        # Probed here, in-process, rather than left to the spawn: python_exe()
        # is sys.executable (its `python3` fallback needs an interpreter with
        # no sys.executable, which west never is), so a spec found here is a
        # spec the child will find too -- and a bare `python -m
        # tan.planner_cli` that misses lands as a raw ModuleNotFoundError
        # traceback with no install line anywhere in it.
        if not _planner_importable():
            log.die(f"alp-emit: the tan planner ({_PLANNER_MODULE}) is not "
                    f"importable from {python_exe()} -- install it with "
                    f"`{_PLANNER_INSTALL_HINT}`. A frozen `tan` binary on PATH "
                    f"is not enough: this needs the importable package.")

        # --sdk-root wins over ALP_SDK_ROOT and over the planner's own walk-up
        # (tan/planner_cli.py's _bootstrap), so the root west resolved is the
        # root the planner binds -- never a checkout the CWD happens to sit in.
        cmd = [
            python_exe(),
            "-m", _PLANNER_MODULE,
            "--sdk-root", str(sdk_root),
            "--input", str(board_yaml),
            "--emit", args.mode,
        ]
        if args.build_root:
            cmd += ["--build-root", args.build_root]
        if args.core:
            cmd += ["--core", args.core]
        return subprocess.call(cmd, env=env_with_sdk(sdk_root))

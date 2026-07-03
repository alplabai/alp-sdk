# SPDX-License-Identifier: Apache-2.0
"""
`west alp-clean` -- tear down the per-project build directory and
the orchestrator's state cache.  Single-purpose; no Zephyr or
Yocto-specific cleaners are invoked (the per-slice build dirs are
self-contained so removing them is enough).

Customer flow:

    west alp-clean examples/multicore/rpmsg-v2n
    west alp-clean examples/multicore/rpmsg-v2n --dry-run    # list, don't delete

Also runnable standalone (`python alp_clean.py <app>`) -- the path the
`alp clean` CLI verb delegates to, mirroring alp_flash.py.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Optional

try:
    from west import log                     # type: ignore[import-not-found]
    from west.commands import WestCommand    # type: ignore[import-not-found]
    _HAS_WEST = True
except ImportError:                          # pragma: no cover
    # Allow `python alp_clean.py ...` outside a west workspace (the
    # `alp clean` delegation path + unit tests).
    _HAS_WEST = False

    class _StubLog:
        @staticmethod
        def inf(msg: str) -> None: print(msg)
        @staticmethod
        def wrn(msg: str) -> None: print(f"WARN: {msg}", file=sys.stderr)
        @staticmethod
        def err(msg: str) -> None: print(f"ERROR: {msg}", file=sys.stderr)
        @staticmethod
        def die(msg: str) -> None:
            print(f"FATAL: {msg}", file=sys.stderr)
            sys.exit(1)

    log = _StubLog()                         # type: ignore[assignment]

    class WestCommand:                       # type: ignore[no-redef]
        """Stand-in so the module imports cleanly without west."""

        def __init__(self, *a, **kw) -> None: pass

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root               # noqa: E402


def _add_arguments(parser: argparse.ArgumentParser) -> None:
    """Shared argparse wiring; used by both WestCommand and the
    ``python alp_clean.py ...`` standalone path."""
    parser.add_argument(
        "app_path",
        help="Path to the application source directory.")
    parser.add_argument(
        "--build-root", default=None,
        help="Override the build root (default: <app_path>/build).")
    parser.add_argument(
        "--dry-run", action="store_true",
        help="List paths that would be removed, don't delete.")


def run(args: argparse.Namespace) -> int:
    """Remove the build root + orchestrator state cache (or list them)."""
    if find_sdk_root() is None:
        log.die("Cannot locate alp-sdk root.")
        return 1

    app_path = Path(args.app_path).resolve()
    build_root = (Path(args.build_root).resolve()
                  if args.build_root
                  else app_path / "build")

    targets = [
        build_root,
        app_path / ".alp-build-state.json",
    ]

    removed = 0
    for t in targets:
        if t.is_dir():
            if args.dry_run:
                log.inf(f"[DRY] would rmtree {t}")
            else:
                log.inf(f"alp-clean: removing {t}")
                shutil.rmtree(t, ignore_errors=True)
                removed += 1
        elif t.is_file():
            if args.dry_run:
                log.inf(f"[DRY] would unlink {t}")
            else:
                log.inf(f"alp-clean: removing {t}")
                t.unlink()
                removed += 1

    if removed == 0 and not args.dry_run:
        log.inf("alp-clean: nothing to remove")
    return 0


class AlpClean(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-clean",
            "Remove the per-project build dir + orchestrator cache",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):    # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        _add_arguments(parser)
        return parser

    def do_run(self, args, _unknown):        # type: ignore[no-untyped-def]
        return run(args)


# ---------------------------------------------------------------------
# Standalone CLI entry (`python alp_clean.py <app>`)
# ---------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    """Standalone entry -- the `alp clean` delegation path.  When
    invoked under west, the AlpClean.do_run path is used instead."""
    parser = argparse.ArgumentParser(
        prog="alp-clean",
        description="Remove the per-project build dir + orchestrator cache.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_arguments(parser)
    args = parser.parse_args(argv)
    return run(args)


if __name__ == "__main__":                    # pragma: no cover
    raise SystemExit(main())

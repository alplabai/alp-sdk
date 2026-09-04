#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-quality` -- run the quality-task registry for a profile (#610 §5).

    west alp-quality --profile pr [--json r.json --junit r.xml --sarif r.sarif]
"""
from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_quality
import alp_quality  # noqa: E402

try:
    from west.commands import CommandError, WestCommand  # type: ignore
except ImportError:  # standalone / CI without west
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...

    class CommandError(RuntimeError):  # type: ignore[no-redef]
        """Mirrors `west.commands.CommandError` for the no-west path.

        Same two-line shape west uses: a `returncode` west exits with. The
        shim exists so `do_run`'s raise below stays importable when this
        module is exercised standalone (the `main()` entry point at the
        bottom, and the tests that call it), exactly as the `WestCommand`
        shim above already does.
        """

        def __init__(self, returncode: int = 1) -> None:
            super().__init__()
            self.returncode = returncode


def _add_args(parser) -> None:
    parser.add_argument("--profile", required=True,
                        choices=("quick", "pr", "full", "release"),
                        help="quality profile to run")
    parser.add_argument("--json", help="write a JSON report to this path")
    parser.add_argument("--junit", help="write a JUnit XML report to this path")
    parser.add_argument("--sarif", help="write a SARIF report to this path")


def run(args) -> int:
    return alp_quality.main([
        "--profile", args.profile,
        *(["--json", args.json] if args.json else []),
        *(["--junit", args.junit] if args.junit else []),
        *(["--sarif", args.sarif] if args.sarif else []),
    ])


class AlpQuality(WestCommand):
    def __init__(self) -> None:
        super().__init__("alp-quality",
                         "Run the quality-task registry for a profile (JSON/JUnit/SARIF)",
                         "\n".join(__doc__.splitlines()[2:]) if __doc__ else "")

    def do_add_parser(self, parser_adder):  # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        _add_args(parser)
        return parser

    def do_run(self, args, _unknown):  # type: ignore[no-untyped-def]
        # west DISCARDS whatever `do_run` returns -- `WestApp.run_extension`
        # calls it for effect and derives the process exit status from
        # exceptions only. So `return run(args)` silently threw the exit code
        # away: `west alp-quality --profile pr` printed
        #
        #     alp-quality profile=pr: 54/55 passed
        #       [FAIL] emit-kconfig-contract (scripts/check_emit_kconfig_contract.py)
        #
        # and still exited 0, which `tan quality` then faithfully forwarded
        # (tan-cli#721 was filed against tan for that; tan is the messenger).
        # Anything gating on this command -- a pre-commit hook, a CI step, a
        # release checklist -- passed with a failed gate check on screen.
        #
        # `CommandError(rc)` is west's own mechanism for exactly this: west
        # catches it and exits with that returncode. `alp_quality.main()`
        # already computes the right value (`0 if rep.ok() else 1`, where
        # `ok()` is "every GATE task passed"); nothing about which checks
        # count is re-decided here.
        rc = run(args)
        if rc:
            raise CommandError(rc)
        return rc


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="run the quality-task registry")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())

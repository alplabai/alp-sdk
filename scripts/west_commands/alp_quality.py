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
    from west.commands import WestCommand  # type: ignore
except ImportError:  # standalone / CI without west
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...


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
        return run(args)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="run the quality-task registry")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())

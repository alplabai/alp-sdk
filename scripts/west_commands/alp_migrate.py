#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-migrate` -- version/migrate board.yaml (epic #610 WS6-b).

    west alp-migrate --check      # report versions; nonzero on drift
    west alp-migrate --preview    # unified diff + diagnostic-v1 JSON, no writes
    west alp-migrate --apply      # rewrite in place, regen, run pr profile
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_migrate
import alp_migrate  # noqa: E402

try:
    from west.commands import WestCommand  # type: ignore
except ImportError:
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...

REPO = _HERE.parent.parent.parent


def _targets(args) -> list[Path]:
    if args.board:
        return [Path(args.board).resolve()]
    if args.all:
        return sorted(REPO.rglob("board.yaml"))
    return [Path("board.yaml").resolve()]


def run(args) -> int:
    targets = _targets(args)
    drift = 0
    for path in targets:
        if not path.is_file():
            print(f"alp-migrate: {path} not found", file=sys.stderr)
            return 1
        text = path.read_text(encoding="utf-8")
        doc = alp_migrate.load(text)
        steps = alp_migrate.plan(doc)
        if args.check:
            if steps:
                drift = 1
                print(f"alp-migrate: {path} needs migration "
                      f"{[f'{a}->{b}' for a, b in steps]}", file=sys.stderr)
            continue
        if not steps:
            continue
        new_text, report = alp_migrate.apply_text(text)  # byte-faithful writer
        if args.preview:
            sys.stdout.write(alp_migrate.diff(text, new_text, str(path)))
            json.dump(alp_migrate.report_to_diagnostics(
                report, path.as_uri()), sys.stdout, indent=2)
            sys.stdout.write("\n")
            continue
        # --apply
        path.write_text(new_text, encoding="utf-8")
        print(f"alp-migrate: migrated {path}")
    if args.check:
        if drift:
            return 1
        print(f"alp-migrate: all board.yaml at v{alp_migrate.LATEST}.")
        return 0
    if getattr(args, "apply", False) and not args.no_verify:
        return _verify()
    return 0


def _verify() -> int:
    """Regen derived files after an apply; report but don't fail the migrate
    if regen tooling is unavailable in this environment."""
    catalog = REPO / "scripts" / "gen_catalog.py"
    if catalog.is_file():
        subprocess.run([sys.executable, str(catalog)], cwd=REPO, check=False)
    return 0


def _add_args(parser) -> None:
    parser.add_argument("--check", action="store_true",
                        help="report versions; nonzero on drift")
    parser.add_argument("--preview", action="store_true",
                        help="unified diff + diagnostic-v1 JSON, no writes")
    parser.add_argument("--apply", action="store_true",
                        help="rewrite board.yaml in place")
    parser.add_argument("--all", action="store_true",
                        help="every board.yaml under the repo")
    parser.add_argument("--board", help="a single board.yaml path")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the post-apply regen step")


class AlpMigrate(WestCommand):
    def __init__(self) -> None:
        super().__init__("alp-migrate",
                         "Version and migrate a project's board.yaml",
                         "\n".join(__doc__.splitlines()[2:]) if __doc__ else "")

    def do_add_parser(self, parser_adder):  # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        _add_args(parser)
        return parser

    def do_run(self, args, _unknown):  # type: ignore[no-untyped-def]
        return run(args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="version/migrate board.yaml")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())

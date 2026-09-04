#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-migrate` -- version/migrate board.yaml (epic #610 WS6-b).

    west alp-migrate --check      # report versions; nonzero on drift
    west alp-migrate --preview    # unified diff + diagnostic-v1 JSON, no writes
    west alp-migrate --apply      # rewrite in place + regen derived files
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_migrate
import alp_migrate  # noqa: E402

try:
    from west.commands import CommandError, WestCommand  # type: ignore
except ImportError:
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...

    class CommandError(RuntimeError):  # type: ignore[no-redef]
        """Mirrors `west.commands.CommandError` for the no-west path."""

        def __init__(self, returncode: int = 1) -> None:
            super().__init__()
            self.returncode = returncode

REPO = _HERE.parent.parent.parent

# Fallback-only: dirs to prune when REPO isn't a git worktree (see
# `_all_board_yaml_files`). git itself needs no such list -- `--exclude-
# standard` already reads `.gitignore`, the one source of truth for
# "this is build output, not a source".
_FALLBACK_SKIP_DIRS = frozenset({".git", ".west", "build", "twister-out"})


def _all_board_yaml_files(root: Path) -> list[Path]:
    """Every board.yaml under `root` for `--all`. Same fix as
    check_library_registry.py's `_board_yaml_files`, same defect class: a
    raw `root.rglob("board.yaml")` here also walked `twister-out/`/`build/`
    unbounded from repo root."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others",
             "--exclude-standard", "--", "*board.yaml"],
            check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        found: list[Path] = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in _FALLBACK_SKIP_DIRS]
            if "board.yaml" in filenames:
                found.append(Path(dirpath) / "board.yaml")
        return sorted(found)
    return sorted(root / line for line in proc.stdout.splitlines() if line)


def _targets(args) -> list[Path]:
    if args.board:
        return [Path(args.board).resolve()]
    if args.all:
        return _all_board_yaml_files(REPO)
    return [Path("board.yaml").resolve()]


def run(args) -> int:
    targets = _targets(args)
    drift = 0
    for path in targets:
        if not path.is_file():
            print(f"alp-migrate: {path} not found", file=sys.stderr)
            return 1
        text = path.read_text(encoding="utf-8")
        try:
            doc = alp_migrate.load(text)
            steps = alp_migrate.plan(doc)
        except alp_migrate.MigrateError as e:
            print(f"alp-migrate: {path}: {e}", file=sys.stderr)
            return 1
        if args.check:
            if steps:
                drift = 1
                print(f"alp-migrate: {path} needs migration "
                      f"{[f'{a}->{b}' for a, b in steps]}", file=sys.stderr)
            continue
        if not steps:
            continue
        try:
            new_text, report = alp_migrate.apply_text(text)
        except alp_migrate.MigrateError as e:
            print(f"alp-migrate: {path}: {e}", file=sys.stderr)
            return 1
        if args.preview:
            sys.stdout.write(alp_migrate.diff(text, new_text, str(path)))
            json.dump(alp_migrate.report_to_diagnostics(report, path.as_uri()),
                      sys.stdout, indent=2)
            sys.stdout.write("\n")
            continue
        if args.apply:
            path.write_text(new_text, encoding="utf-8", newline="")
            print(f"alp-migrate: migrated {path}")
    if args.check:
        if drift:
            return 1
        print(f"alp-migrate: all board.yaml at v{alp_migrate.LATEST}.")
        return 0
    if args.apply and not args.no_verify:
        return _verify()
    return 0


def _verify() -> int:
    """Regen derived files after an apply. Propagate a regen failure instead
    of reporting a false success."""
    catalog = REPO / "scripts" / "gen_catalog.py"
    if catalog.is_file():
        cp = subprocess.run([sys.executable, str(catalog)], cwd=REPO, check=False)
        if cp.returncode != 0:
            print(f"alp-migrate: post-apply regen (gen_catalog.py) failed "
                  f"(exit {cp.returncode})", file=sys.stderr)
            return cp.returncode
    return 0


def _add_args(parser) -> None:
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true",
                      help="report versions; nonzero on drift")
    mode.add_argument("--preview", action="store_true",
                      help="unified diff + diagnostic-v1 JSON, no writes")
    mode.add_argument("--apply", action="store_true",
                      help="rewrite board.yaml in place")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--all", action="store_true",
                        help="every board.yaml under the repo")
    target.add_argument("--board", help="a single board.yaml path")
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
        # west DISCARDS `do_run`'s return value and derives the exit status
        # from exceptions only, so `return run(args)` threw the code away --
        # `west alp-migrate --check` could report an out-of-date board.yaml
        # and still exit 0. Same defect and same fix as
        # `alp_quality`/`alp_lock`; `run()` already computes the right value.
        rc = run(args)
        if rc:
            raise CommandError(rc)
        return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="version/migrate board.yaml")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-lock` -- write/self-check alp.lock (epic #610 WS6-a).

    west alp-lock            # write ./alp.lock for the workspace
    west alp-lock --check    # recompute + schema-validate; nonzero on a
                              # broken generator or a local-path leak

alp.lock is generated on demand, not committed (#1576): its
`digests.metadata` is a single hash over the whole `metadata/**` tree, so
any two PRs touching different metadata files rewrote the same line from
different bases and conflicted by construction. `--check` therefore no
longer diffs against a tracked copy -- there isn't one -- it proves the
generator itself still produces a schema-valid lock.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_lock
import alp_lock  # noqa: E402

try:
    from west.commands import CommandError, WestCommand  # type: ignore
except ImportError:  # standalone / CI without west
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...

    class CommandError(RuntimeError):  # type: ignore[no-redef]
        """Mirrors `west.commands.CommandError` for the no-west path."""

        def __init__(self, returncode: int = 1) -> None:
            super().__init__()
            self.returncode = returncode


def _workspace_root(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    # Default: the alp-sdk repo root (scripts/west_commands/ -> repo). alp-sdk
    # is its own west manifest repo, so its root IS the workspace topdir here.
    # If alp-sdk were ever a sub-project of a larger workspace, pass --workspace
    # (or resolve the real `west topdir`) instead.
    return _HERE.parent.parent.parent


def _dump(lock: dict) -> str:
    return json.dumps(lock, indent=2, sort_keys=True) + "\n"


def run(args) -> int:
    root = _workspace_root(args.workspace)
    board = Path(args.board).resolve() if args.board else None
    if args.check:
        # Generate in memory and schema-validate -- no committed alp.lock to
        # diff against any more (#1576). `build_lock` already routes every
        # leaf through `_reject_local`, so a local/abs path leak surfaces as
        # a `LockError` here, same as it always did.
        try:
            lock = alp_lock.build_lock(root, board)
        except alp_lock.LockError as e:
            print(f"alp-lock: {e}", file=sys.stderr)
            return 1
        # The schema describes what THIS generator emits, so it comes from
        # the repo this script itself lives in -- not from `--workspace`,
        # which in tests points at a synthetic fixture dir that doesn't
        # carry a copy of it.
        schema_path = (_HERE.parent.parent.parent / "metadata" / "schemas"
                       / "alp-lock-v1.schema.json")
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        try:
            jsonschema.validate(lock, schema)
        except jsonschema.ValidationError as e:
            print(f"alp-lock: generated lock fails schema validation: {e.message}",
                  file=sys.stderr)
            return 1
        print("alp-lock: generator produces a schema-valid lock.")
        return 0
    lock_path = root / "alp.lock"
    lock = alp_lock.build_lock(root, board)
    lock_path.write_text(_dump(lock), encoding="utf-8", newline="")
    print(f"alp-lock: wrote {lock_path}")
    return 0


def _add_args(parser) -> None:
    parser.add_argument("--check", action="store_true",
                        help="generate + schema-validate in memory (nonzero on a "
                             "broken generator or a local-path leak)")
    parser.add_argument("--workspace", help="workspace root (default: west topdir)")
    parser.add_argument("--board", help="board.yaml to record resolution.board from")


class AlpLock(WestCommand):
    def __init__(self) -> None:
        super().__init__("alp-lock",
                         "Write or verify the project's alp.lock dependency lock",
                         "\n".join(__doc__.splitlines()[2:]) if __doc__ else "")

    def do_add_parser(self, parser_adder):  # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        _add_args(parser)
        return parser

    def do_run(self, args, _unknown):  # type: ignore[no-untyped-def]
        # west DISCARDS `do_run`'s return value and derives the exit status
        # from exceptions only, so `return run(args)` threw the code away --
        # `west alp-lock --check` could report drift and still exit 0. Same
        # defect and same fix as `alp_quality`/`alp_migrate`; `run()` already
        # computes the right value, nothing is re-decided here.
        rc = run(args)
        if rc:
            raise CommandError(rc)
        return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="write/self-check alp.lock")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())

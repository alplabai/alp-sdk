#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compatibility CLI for the shared board.yaml validator.

`alp validate` is the canonical diagnostic-rich entry point.  This script stays
as the historical pre-flight command used by west wrappers, MCP tooling, and
docs.  It deliberately calls the same validator and then the orchestrator loader
so schema/xref/compat diagnostics and hard cross-field consistency errors match
the build preflight.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from alp_cli.diagnostic import render
from alp_cli.validator import validate_board_yaml
from alp_orchestrate import OrchestratorError, load_board_yaml


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate board.yaml with the shared Alp validator.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--metadata-root", type=Path, default=None,
                        help="Override the metadata search root for orchestrator consistency checks.")
    parser.add_argument("--no-presets", action="store_true",
                        help="Run only the rich diagnostic validator; skip orchestrator consistency checks.")
    parser.add_argument("--no-color", action="store_true",
                        help="Disable ANSI colour in rendered diagnostics.")
    args = parser.parse_args()

    if not args.input.is_file():
        print(f"FAIL {args.input}: file not found", file=sys.stderr)
        return 1

    source_text = args.input.read_text(encoding="utf-8")
    collector = validate_board_yaml(args.input)
    for diag in collector:
        print(render(diag, source_text=source_text, color=not args.no_color),
              file=sys.stderr)
    if collector.has_errors():
        return 1

    if not args.no_presets:
        try:
            if args.metadata_root is None:
                load_board_yaml(args.input)
            else:
                load_board_yaml(args.input, metadata_root=args.metadata_root)
        except OrchestratorError as exc:
            print(f"FAIL consistency: {exc}", file=sys.stderr)
            return 1

    warnings = sum(1 for diag in collector if diag.severity == "warning")
    if warnings:
        print(f"{args.input}: clean ({warnings} warning(s))")
    else:
        print(f"{args.input}: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

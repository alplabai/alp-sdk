#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compatibility CLI for the shared board.yaml validator.

`tan validate` is the canonical diagnostic-rich entry point, and spawns this
script as its non-`--offline` subprocess (`tan-cli` `VALIDATOR_SCRIPT`).  This
script also stays as the historical pre-flight command used by west wrappers,
MCP tooling, and docs.  It deliberately calls the same validator and then the
orchestrator loader so schema/xref/compat diagnostics and hard cross-field
consistency errors match the build preflight.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from alp_cli.diagnostic import render
from alp_cli.validator import validate_board_yaml
from alp_orchestrate import (OrchestratorError, SdkRevisionNotBuildable,
                             SdkRevisionUnknown, SdkRevisionUnsupported,
                             load_board_yaml)

# `metadata/sdk_version.yaml` documents this script as running the hw_rev
# compatibility check with "exit code 3 on mismatch".  It is its own code
# because it is the one failure a caller can act on mechanically -- pin a
# different SDK, or change hw_rev -- rather than a generic invalid-project 1
# (#1019).
EXIT_SDK_REVISION_UNSUPPORTED = 3

# A DIFFERENT failure from the above: the requested hw_rev isn't a key in
# its resolved hw_revisions table at all, so there's no range to have been
# outside of.  Its own code (not 3) for the same mechanical-action reason:
# a customer hitting this needs "pick a revision that exists", not "pin a
# different SDK" (#1025, the existence-only half).
EXIT_SDK_REVISION_UNKNOWN = 4

# A THIRD failure, distinct from both above: the requested hw_rev IS a key
# in its resolved hw_revisions table, but its declared `status:` refuses a
# build (`reserved`, `tbd`, or no `status` key at all -- #1025's broad
# reading).  Its own code for the same mechanical-action reason: a customer
# hitting this needs "pick a revision whose status is buildable", not
# "pick a revision that exists" (already true here) or "pin a different
# SDK" (there is no range mismatch).
EXIT_SDK_REVISION_NOT_BUILDABLE = 5


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
    collector = validate_board_yaml(args.input, metadata_root=args.metadata_root)
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
        except SdkRevisionUnknown as exc:
            print(f"FAIL sdk-compat: {exc}", file=sys.stderr)
            return EXIT_SDK_REVISION_UNKNOWN
        except SdkRevisionNotBuildable as exc:
            print(f"FAIL sdk-compat: {exc}", file=sys.stderr)
            return EXIT_SDK_REVISION_NOT_BUILDABLE
        except SdkRevisionUnsupported as exc:
            print(f"FAIL sdk-compat: {exc}", file=sys.stderr)
            return EXIT_SDK_REVISION_UNSUPPORTED
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

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Customer-side validator for `board.yaml`.

Runs the three checks an alp-sdk app author cares about *before*
the build kicks off:

  1. The file is valid YAML.
  2. It conforms to metadata/schemas/board-config-v1.schema.json.
  3. The referenced SoM SKU has a preset at
     `metadata/e1m_modules/<SKU>/som.yaml` -- and the referenced
     carrier has a preset at `metadata/carriers/<name>/board.yaml`,
     or is treated as a customer-supplied custom carrier when
     `populated` is provided inline.

Customer usage (from the app root):

    python3 $ALP_SDK/scripts/validate_board_yaml.py
    python3 $ALP_SDK/scripts/validate_board_yaml.py --input board.yaml

CI usage (smoke-checks every shipped example):

    python3 scripts/validate_board_yaml.py \\
        --input metadata/templates/board.yaml.example

Exit codes:
  0  clean
  1  YAML parse or schema violation
  2  missing SoM SKU preset or missing carrier preset without
     inline `populated`
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("validate_board_yaml: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("validate_board_yaml: jsonschema is required.  Install via `pip install jsonschema`.")


REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT_DEFAULT = REPO / "metadata"
SCHEMA = METADATA_ROOT_DEFAULT / "schemas" / "board-config-v1.schema.json"


def _load_yaml(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        print(f"FAIL {path}: file not found", file=sys.stderr)
        return None
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        print(f"FAIL {path}: invalid YAML ({e})", file=sys.stderr)
        return None
    if not isinstance(data, dict):
        print(f"FAIL {path}: top-level value is not a mapping", file=sys.stderr)
        return None
    return data


def _check_schema(project: dict[str, Any], path: Path) -> bool:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(project), key=lambda e: list(e.absolute_path))
    if not errors:
        print(f"OK   schema: {path}")
        return True
    print(f"FAIL schema: {path}", file=sys.stderr)
    for e in errors:
        loc = "/".join(str(p) for p in e.absolute_path) or "<root>"
        print(f"  · {loc}: {e.message}", file=sys.stderr)
    return False


def _check_som_preset(project: dict[str, Any], metadata_root: Path) -> int:
    """Return 0 on OK, 1 on warning (partial HW config), 2 on missing."""
    sku = project["som"]["sku"]
    preset = metadata_root / "e1m_modules" / sku / "som.yaml"
    if not preset.is_file():
        print(f"FAIL som preset: no preset for {sku} at {preset.relative_to(REPO) if preset.is_relative_to(REPO) else preset}",
              file=sys.stderr)
        return 2

    try:
        body = yaml.safe_load(preset.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        print(f"FAIL som preset: {preset} failed to parse ({e})", file=sys.stderr)
        return 2

    partial = isinstance(body, dict) and (body.get("status") or {}).get("partial_hw_config", False)
    if partial:
        print(f"WARN som preset: {sku} preset is marked partial_hw_config=true "
              f"(memory / pinout fields still TBD)")
        return 1
    print(f"OK   som preset: {sku}")
    return 0


def _check_carrier_preset(project: dict[str, Any], metadata_root: Path) -> int:
    """Return 0 on OK, 2 on missing-without-inline-populated."""
    carrier_block = project.get("carrier")
    if not carrier_block:
        # No carrier -- headless / inference-only build.  Schema permits.
        print("OK   carrier: <none declared>")
        return 0
    name = carrier_block.get("name", "")
    preset = metadata_root / "carriers" / name / "board.yaml"
    if preset.is_file():
        print(f"OK   carrier preset: {name}")
        return 0

    # No preset -- accept iff the user's board.yaml supplies a
    # `populated` block (custom carrier path).
    populated = carrier_block.get("populated") or {}
    if populated:
        print(f"OK   carrier: '{name}' is a custom carrier ({len(populated)} populated entries)")
        return 0
    print(f"FAIL carrier: '{name}' has no preset and no inline `populated` block",
          file=sys.stderr)
    print(f"     expected preset at {preset.relative_to(REPO) if preset.is_relative_to(REPO) else preset}",
          file=sys.stderr)
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a board.yaml against the SDK's schema + preset library.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT_DEFAULT,
                        help="Override the metadata search root.")
    parser.add_argument("--no-presets", action="store_true",
                        help="Skip the SoM SKU + carrier preset checks (schema-only mode).")
    args = parser.parse_args()

    project = _load_yaml(args.input)
    if project is None:
        return 1

    if not _check_schema(project, args.input):
        return 1

    if args.no_presets:
        print(f"\n{args.input}: schema OK (preset checks skipped via --no-presets)")
        return 0

    som_rv = _check_som_preset(project, args.metadata_root)
    carrier_rv = _check_carrier_preset(project, args.metadata_root)

    # Missing preset is exit 2; partial_hw_config is a warning (exit 0).
    if som_rv == 2 or carrier_rv == 2:
        print(f"\n{args.input}: missing-preset failures", file=sys.stderr)
        return 2

    if som_rv == 1:
        print(f"\n{args.input}: clean (with warnings)")
    else:
        print(f"\n{args.input}: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

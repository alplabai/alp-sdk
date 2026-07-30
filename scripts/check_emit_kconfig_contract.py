#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI schema/smoke contract for `--emit kconfig` (#893).

THE LIVE WORKSPACE SMOKE IS GONE, NOT NARROWED IN PLACE.

`--emit kconfig` was the SDK's only workspace-dependent emit mode: it needed
a bootstrapped `ZEPHYR_BASE` (v4.4.0) to run the real Kconfig solver, so this
gate used to spawn `python -m alp_orchestrate --input <board.yaml> --emit
kconfig --core <id>` against a real board and assert shape on the LIVE
output. `scripts/alp_orchestrate/kconfig_symbols.py` -- the emitter -- has
moved to tan; alp-sdk no longer has a `--emit kconfig` to invoke, live or
otherwise. Unlike `check_emit_snapshots.py`, which repoints its own
subprocess list at `tan.planner_cli` for the modes tan still renders on
request, there is nothing to repoint here: nothing in this repo implements
or runs the mode any more, so the spawn is deleted outright.

What alp-sdk CAN still see, hermetically and with no `ZEPHYR_BASE`, is the
CONTRACT the emitter's output has to conform to:

  * the contract shape itself (`_ENVELOPE_SCHEMA` below) is a valid Draft
    2020-12 JSON Schema (`jsonschema.Draft202012Validator.check_schema`);
  * the committed cross-repo golden
    (`tests/fixtures/kconfig-contract/emit-kconfig.golden.json`) validates
    against it;
  * that golden is non-empty and structurally sane -- at least one symbol,
    every symbol carrying `name` + `type` (`type` one of the Kconfig
    types) plus `prompt`/`depends`/`default`/`help`.

No `metadata/schemas/*.schema.json` file backs this mode today --
`metadata/emit-registry-v1.json`'s `kconfig` entry already records
`output.schema_id: null` for exactly that reason -- so the schema lives
here, inline, rather than as a new file this single-file gate migration
would then also have to wire into that registry (`schema_id` plus whatever
else references it). Promoting `_ENVELOPE_SCHEMA` to a real
`metadata/schemas/emit-kconfig-v1.schema.json`, and flipping the registry's
`output.schema_id` to match in the same commit, is still open work.

WHAT WAS LOST, deliberately: the live `--emit kconfig` smoke against a real
bootstrapped Zephyr workspace no longer runs from this gate, or from
anywhere in alp-sdk. tan covers the dumper's own render invariants in
`python/tests/core/test_kconfig_symbols.py`; the real `west build`
round-trip -- proof that a LIVE envelope, not just this committed golden,
still conforms to the shared contract -- is now uncovered by alp-sdk and
must be re-established in tan's own CI. No `--emit-output <path>`-style
"validate a live document" flag was added here to soften that: this script
never had one, and inventing one now would just be alp-sdk speculatively
building tan's half of the fix. If tan's CI wants this exact
schema/key-set check reused rather than re-implemented against its own copy
of the golden, that reuse is tan's call to make, not alp-sdk's to
pre-empt.

Run locally (no ZEPHYR_BASE, no west, no Zephyr workspace required):

    python scripts/check_emit_kconfig_contract.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent

_GOLDEN = REPO / "tests" / "fixtures" / "kconfig-contract" / "emit-kconfig.golden.json"

_ALLOWED_TYPES = ["bool", "hex", "int", "string", "tristate"]

# Inline Draft 2020-12 schema for the `--emit kconfig` envelope shape
# (what `alp_orchestrate.kconfig_symbols._envelope()` used to return, now
# tan's to produce) -- see the module docstring for why this is inline
# rather than a metadata/schemas/*.schema.json file.
_ENVELOPE_SCHEMA: dict = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://github.com/alplabai/alp-sdk/scripts/check_emit_kconfig_contract.py",
    "title": "`--emit kconfig` envelope contract",
    "description": (
        "The shape of tests/fixtures/kconfig-contract/emit-kconfig.golden.json "
        "-- the canonical cross-repo contract fixture tan-cli's `parse_kconfig` "
        "and alp-sdk-vscode's `kconfigSymbolsFromEnvelope` both test their own "
        "parsers against."
    ),
    "type": "object",
    "additionalProperties": False,
    "required": ["schemaVersion", "board", "core", "symbols"],
    "properties": {
        "schemaVersion": {"const": 1},
        "board": {"type": "string", "minLength": 1},
        "core": {"type": "string", "minLength": 1},
        "symbols": {
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["name", "type", "prompt", "depends", "default", "help"],
                "properties": {
                    "name":    {"type": "string", "minLength": 1},
                    "type":    {"type": "string", "enum": _ALLOWED_TYPES},
                    "prompt":  {"type": "string"},
                    "depends": {"type": "string"},
                    "default": {"type": ["string", "null"]},
                    "help":    {"type": "string"},
                },
            },
        },
    },
}


def _make_validator(schema: dict) -> jsonschema.Draft202012Validator:
    """The schema is itself checked for Draft 2020-12 validity before it is
    used to validate anything -- an invalid schema would otherwise silently
    validate nothing, which is worse than no gate at all."""
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())


def _check_golden(golden_path: Path,
                   validator: jsonschema.Draft202012Validator) -> list[str]:
    """Every problem is prefixed with the golden's own repo-relative path,
    so a failure names the file as well as the violation."""
    label = golden_path.relative_to(REPO).as_posix()
    try:
        doc = json.loads(golden_path.read_text(encoding="utf-8"))
    except Exception as e:
        return [f"{label}: parse error ({e})"]

    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    problems = []
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        problems.append(f"{label}: {loc}: {err.message}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--golden", type=Path, default=_GOLDEN,
                     help="Path to the golden envelope to check (default: "
                          "the committed cross-repo contract fixture).")
    args = ap.parse_args()

    validator = _make_validator(_ENVELOPE_SCHEMA)
    problems = _check_golden(args.golden, validator)

    label = args.golden.relative_to(REPO).as_posix() if args.golden.is_relative_to(REPO) \
        else str(args.golden)

    if problems:
        print(f"FAIL {label}")
        for p in problems:
            print(f"  · {p}")
        return 1

    print(f"OK   {label}  (schema is valid Draft 2020-12; golden conforms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

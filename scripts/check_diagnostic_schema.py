#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate the versioned machine diagnostics contract
(metadata/schemas/diagnostic-v1.schema.json) two ways:

  1. Schema self-validity (Draft 2020-12).
  2. A REAL document, produced by actually running the emitter
     (alp_cli.diagnostic_format.machine_json_for_board_yaml) against a
     known-bad board.yaml fixture (never a hand-written sample),
     validates against the schema. This is the same drift-can't-happen
     posture as scripts/check_emit_registry.py: the gate exercises the
     real emitter, not a copy of its output.

The gate calls the emitter IN-PROCESS. It used to shell
`python -m alp_cli.main validate --format json`, which pinned two pure
CLI wrappers (alp_cli/main.py, alp_cli/validate.py) in place for a
metadata gate even though ADR 0020 retires alp-sdk's whole user command
surface. The document under test is unchanged: `--format json` prints
exactly `machine_json_for_board_yaml(path)`.

What the subprocess form additionally covered -- that the CLI prints ONLY
the JSON document to stdout, with no human prose interleaved -- was NOT a
property of this schema, and was locked by
tests/scripts/test_diagnostic_format.py::test_cli_format_json_emits_conformant_document_on_stdout
(plus its sarif and default-format siblings) until `alp_cli/main.py` and
`alp_cli/validate.py` themselves retired (alp-sdk#1368); `tan validate
--format json|diagnostic-v1|sarif` is the live command now, with its own
tan-cli test coverage of the same stdout-only property.

Run locally:

    python3 scripts/check_diagnostic_schema.py
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from alp_cli.diagnostic_format import machine_json_for_board_yaml  # noqa: E402

SCHEMA = REPO / "metadata" / "schemas" / "diagnostic-v1.schema.json"
FIXTURE = REPO / "tests" / "fixtures" / "board_yaml_bad" / "ALP-B005-bad-sku.yaml"


def _make_validator(schema_path: Path) -> jsonschema.Draft202012Validator:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    ap.add_argument("--fixture", type=Path, default=FIXTURE)
    args = ap.parse_args()

    # Click used to do this for us (`click.Path(exists=True, dir_okay=False)`
    # on `alp validate PATH`), and a bad --fixture came back as a clean gate
    # error rather than a traceback. The in-process form has no argv layer, so
    # the gate owns the check.
    if not args.fixture.is_file():
        print(f"check_diagnostic_schema: --fixture {args.fixture} is not a file; "
              f"point it at a known-bad board.yaml under "
              f"tests/fixtures/board_yaml_bad/.")
        return 1

    validator = _make_validator(args.schema)
    try:
        doc = machine_json_for_board_yaml(args.fixture)
    except Exception as exc:  # noqa: BLE001 -- deliberately broad, framed below
        # Same posture as the --fixture-is-a-file guard above: the old
        # subprocess form (`alp validate`) framed a crash out of the
        # validator as a clean CLI error; the in-process form has no argv/
        # command layer to do that for free, so the gate frames it itself
        # instead of letting a bare traceback (e.g. UnicodeDecodeError on a
        # non-UTF-8 fixture) stand in for the gate's verdict.
        print(f"check_diagnostic_schema: --fixture {args.fixture} could not be "
              f"validated: {exc}")
        return 1

    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    if errors:
        print(f"FAIL {args.schema.relative_to(REPO).as_posix()}")
        for err in errors:
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            print(f"  · {loc}: {err.message}")
        return 1

    if not doc.get("diagnostics"):
        print("check_diagnostic_schema: fixture produced zero diagnostics -- "
              "the gate needs at least one to exercise the schema.")
        return 1

    print(f"OK   {args.schema.relative_to(REPO).as_posix()}  "
          f"(self-valid, and a real machine_json_for_board_yaml() document "
          f"conforms, {len(doc['diagnostics'])} diagnostic(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())

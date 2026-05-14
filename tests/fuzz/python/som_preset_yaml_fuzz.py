#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Atheris fuzz harness for the SoM-preset YAML loader.

Each per-SKU SoM preset (`metadata/e1m_modules/E1M-<MPN>.yaml`)
is loaded by `scripts/alp_project.py` at build time to resolve
which silicon + on-module chips + memory layout the firmware is
targeting.

Customers typically don't author these (the SDK ships them per
released MPN), BUT custom-SKU customers can drop a preset in
their own metadata-root + point `validate_board_yaml.py
--metadata-root` at it.  That makes the loader a parser of
customer-supplied untrusted YAML.

The threat model (`docs/threat-model.md`) covers this surface
under §3.5 supply-chain adversary; this fuzzer is the runtime
guarantee.

Install: `pip install atheris pyyaml jsonschema`.

Run:

    python3 tests/fuzz/python/som_preset_yaml_fuzz.py \\
        -max_total_time=30 tests/fuzz/python/corpus/som_preset/
"""

import json
import sys
from pathlib import Path

try:
    import atheris
except ImportError:
    print("atheris not installed.  Skipping.", file=sys.stderr)
    sys.exit(0)

import yaml
import jsonschema

REPO        = Path(__file__).resolve().parent.parent.parent.parent
SCHEMA_PATH = REPO / "metadata" / "schemas" / "som-preset-v1.schema.json"
SCHEMA      = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


@atheris.instrument_func
def TestOneInput(data: bytes) -> None:
    if len(data) > 4096:
        data = data[:4096]
    try:
        text = data.decode("utf-8", errors="replace")
    except Exception:
        return
    try:
        cfg = yaml.safe_load(text)
        if cfg is not None:
            jsonschema.validate(cfg, SCHEMA)
    except (yaml.YAMLError, jsonschema.ValidationError, KeyError,
            TypeError, ValueError, AttributeError):
        return


def main() -> int:
    atheris.Setup(sys.argv, TestOneInput, enable_python_coverage=True)
    atheris.Fuzz()
    return 0


if __name__ == "__main__":
    sys.exit(main())

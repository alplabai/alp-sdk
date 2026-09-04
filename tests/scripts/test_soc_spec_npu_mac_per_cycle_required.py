# SPDX-License-Identifier: Apache-2.0
"""#1849: `soc-spec-v1.schema.json` must require `mac_per_cycle` on every
`ethos-u*` `npus[]` entry, matching what the code already assumes.

Two consumers read `npus[].mac_per_cycle` for an ethos-u* NPU without a
schema backstop:

  * `scripts/alp_model/targets.py::_soc_targets` does `npu['mac_per_cycle']`
    -- a bare dict subscript that raises `KeyError` if the field is absent.
  * `scripts/alp_orchestrate/kconfig.py` filters `npus[]` to entries with a
    truthy `mac_per_cycle` before sizing the Ethos-U accelerator Kconfig; if
    an ethos-u* NPU is the SoC's only instance of its variant and omits the
    field, it drops out of that filter and the accelerator size SILENTLY
    defaults to 256 MAC (`mac = macs[0] if macs else 256`) instead of
    failing loudly.

Neither consumer defaults safely, so the fix is at the metadata source: the
schema's `npu` $def now carries an `if type ~= ^ethos-u` / `then required:
[mac_per_cycle]` conditional. Validated against the isolated `npu` subschema
(not a full SoC doc), matching test_board_models_schema.py's isolation
pattern.
"""
import json
from pathlib import Path

import jsonschema
import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMA = json.loads((_ROOT / "metadata/schemas/soc-spec-v1.schema.json").read_text(encoding="utf-8"))
_NPU_SCHEMA = _SCHEMA["$defs"]["npu"]


def test_ethos_u_npu_without_mac_per_cycle_is_rejected():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate({"type": "ethos-u55", "gops": 128.0}, _NPU_SCHEMA)


def test_ethos_u_npu_with_mac_per_cycle_passes():
    jsonschema.validate(
        {"type": "ethos-u55", "gops": 128.0, "mac_per_cycle": 256}, _NPU_SCHEMA
    )


def test_ethos_u85_without_mac_per_cycle_is_rejected():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate({"type": "ethos-u85", "tops": 4.0}, _NPU_SCHEMA)


def test_non_ethos_u_npu_does_not_require_mac_per_cycle():
    # DRP-AI3 / DX-M1 have no mac_per_cycle concept -- the requirement is
    # scoped to the ethos-u* family only.
    jsonschema.validate({"type": "drp-ai3", "tops": 8.0}, _NPU_SCHEMA)

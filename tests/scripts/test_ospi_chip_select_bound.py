# SPDX-License-Identifier: Apache-2.0
"""`ospi_memory.chip_select` / `hyperram.chip_select`
(`metadata/schemas/som-preset-v1.schema.json`) bound the chip-select index
to alp-sdk#1944's hardware fact: every AEN preset's OSPI0 octal bus wires
exactly two chip-select lines (OSPI0_SS0/OSPI0_SS1 per the shared
E1M-AEN-2626-R2 netlist -- see the `chip_select:` comments in
`metadata/e1m_modules/E1M-AEN301.yaml` and siblings). Before this fix the
field was `{"type": "integer", "minimum": 0}` with no upper bound, so a typo
like `chip_select: 9` validated cleanly and every gate stayed green.

Run locally:

    python -m pytest tests/scripts/test_ospi_chip_select_bound.py -v
"""
import json
from pathlib import Path

import jsonschema
import pytest

_SCHEMA = json.loads(
    (Path(__file__).resolve().parents[2]
     / "metadata" / "schemas" / "som-preset-v1.schema.json").read_text(encoding="utf-8")
)

_OSPI_MEMORY = _SCHEMA["$defs"]["ospi_memory"]
_HYPERRAM = _SCHEMA["$defs"]["hyperram"]

_VALID_OSPI = {"chip": "MX25UM25645GXDI00", "role": "app_storage"}
_VALID_HYPERRAM = {"chip": "W958D8NBYA5I", "capacity_mbit": 256}


@pytest.mark.parametrize("chip_select", [0, 1])
def test_ospi_memory_chip_select_in_range_passes(chip_select):
    doc = {**_VALID_OSPI, "chip_select": chip_select}
    jsonschema.Draft202012Validator(_OSPI_MEMORY).validate(doc)


@pytest.mark.parametrize("chip_select", [2, 9])
def test_ospi_memory_chip_select_out_of_range_rejected(chip_select):
    doc = {**_VALID_OSPI, "chip_select": chip_select}
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.Draft202012Validator(_OSPI_MEMORY).validate(doc)


@pytest.mark.parametrize("chip_select", [0, 1])
def test_hyperram_chip_select_in_range_passes(chip_select):
    doc = {**_VALID_HYPERRAM, "chip_select": chip_select}
    jsonschema.Draft202012Validator(_HYPERRAM).validate(doc)


@pytest.mark.parametrize("chip_select", [2, 9])
def test_hyperram_chip_select_out_of_range_rejected(chip_select):
    doc = {**_VALID_HYPERRAM, "chip_select": chip_select}
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.Draft202012Validator(_HYPERRAM).validate(doc)

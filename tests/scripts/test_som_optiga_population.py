# SPDX-License-Identifier: Apache-2.0
"""An on-module I2C chip marked DNP (`assembled: false`) must not be
advertised as a capability, and SoMs sharing one PCB must agree on whether
OPTIGA Trust M is fitted (#2958, #2990)."""
from __future__ import annotations

from pathlib import Path

import pytest
import yaml

MODULES = Path(__file__).resolve().parents[2] / "metadata" / "e1m_modules"
PRESETS = sorted(MODULES.glob("E1M-*.yaml"))


def _load(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _entries(doc: dict, chip: str) -> list[dict]:
    buses = (doc.get("on_module") or {}).get("i2c_devices") or {}
    return [d for b in buses.values() for d in (b.get("devices") or []) if d.get("chip") == chip]


@pytest.mark.parametrize("path", PRESETS, ids=lambda p: p.stem)
def test_capability_not_true_for_unassembled_chip(path: Path) -> None:
    doc = _load(path)
    for chip, val in (doc.get("capabilities") or {}).items():
        entries = _entries(doc, chip)
        if val is True and entries:
            assert not all(e.get("assembled") is False for e in entries), (
                f"{path.stem}: capabilities.{chip} is true but every "
                f"i2c_devices entry is assembled: false"
            )


def test_aen_family_agrees_on_optiga_population() -> None:
    """Every AEN SKU is one shared PCB; IC1 is DNP=1 on this batch."""
    paths = sorted(MODULES.glob("E1M-AEN*.yaml"))
    assert len(paths) == 7
    for path in paths:
        entries = _entries(_load(path), "optiga_trust_m")
        assert entries, path.stem
        assert all(e.get("assembled") is False for e in entries), path.stem

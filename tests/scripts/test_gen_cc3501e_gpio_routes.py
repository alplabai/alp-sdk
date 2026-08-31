# SPDX-License-Identifier: Apache-2.0
"""Tests for `scripts/gen_cc3501e_gpio_routes.py` (issue #1859).

Covers:
- `_proxy_rows()` drops a composed route whose dispatch_pin is
  bridge-reserved (the generation-time guard replacing the old
  hand-picked per-instance omission that missed IO16 -> GPIO_17);
- `_discover_targets()` finds exactly the AEN examples that enable
  CONFIG_ALP_SDK_GPIO_CC3501E_PROXY, not every cc3501e example;
- the real committed route tables never carry a reserved pad (this
  assertion was hand-verified to fail against the pre-#1859 hand-written
  table -- `{ ALP_E1M_GPIO_IO16, 17u }` -- per
  [[feedback-validate-regression-tests-against-the-broken-build]]; it is
  not re-checked against git history here so the test stays valid under a
  shallow/single-commit CI checkout);
- idempotency of the real generator run.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_cc3501e_gpio_routes.py"

EXAMPLE_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_gpio_routes.c",
)


@pytest.fixture(scope="module")
def gen_module():
    spec = importlib.util.spec_from_file_location("gen_cc3501e_gpio_routes", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gen_cc3501e_gpio_routes"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_proxy_rows_drops_bridge_reserved_pads(gen_module):
    composed = {
        "routes": [
            {"e1m": "E1M_GPIO_IO16", "dispatch": "cc3501e", "dispatch_pin": 17,
             "som_doc": "bridge READY line -- reserved"},
            {"e1m": "E1M_GPIO_IO17", "dispatch": "cc3501e", "dispatch_pin": 16,
             "som_doc": "bridge SPI0 CS -- reserved"},
            {"e1m": "E1M_GPIO_IO9", "dispatch": "cc3501e", "dispatch_pin": 12,
             "som_doc": "ordinary proxied pad"},
            {"e1m": "E1M_GPIO_IO2", "dispatch": "direct"},
        ],
    }
    rows = gen_module._proxy_rows(composed)
    assert [e1m for e1m, _pin, _doc in rows] == ["E1M_GPIO_IO9"]


def test_discover_targets_matches_proxy_kconfig_examples(gen_module):
    targets = {p.parent.name for p in gen_module._discover_targets()}
    assert targets == {
        "aen-cc3501e-bringup",
        "aen-cc3501e-companion-tour",
        "aen-cc3501e-gpio",
    }
    # aen-cc3501e-ble-gatt / aen-cc3501e-gatt-register also open gpio on the
    # same SoM but never set CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y -- they
    # must not get a generated table.
    assert "aen-cc3501e-ble-gatt" not in targets
    assert "aen-cc3501e-gatt-register" not in targets


_ROUTE_ENTRY_RE = re.compile(r"\{\s*ALP_(E1M_GPIO_IO\d+),\s*(\d+)u\s*\}")


def _route_pins(text: str) -> dict[str, int]:
    return {e1m: int(pin) for e1m, pin in _ROUTE_ENTRY_RE.findall(text)}


@pytest.mark.parametrize("path", EXAMPLE_ROUTE_TABLES)
def test_real_tables_never_target_a_reserved_pad(gen_module, path):
    pins = _route_pins(path.read_text(encoding="utf-8"))
    reserved_hits = {e1m: pin for e1m, pin in pins.items()
                     if pin in gen_module.RESERVED_CC3501E_PADS}
    assert not reserved_hits, (
        f"{path} routes a bridge-reserved CC3501E pad: {reserved_hits} -- "
        f"gpio_pad_reserved() would refuse this at runtime"
    )


def test_real_generation_is_idempotent(gen_module):
    before = {p: p.read_bytes() for p in EXAMPLE_ROUTE_TABLES}
    rc = gen_module.main()
    assert rc == 0
    after = {p: p.read_bytes() for p in EXAMPLE_ROUTE_TABLES}
    assert before == after, "regenerating the committed tables changed them"

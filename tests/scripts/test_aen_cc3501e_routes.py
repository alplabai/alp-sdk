# SPDX-License-Identifier: Apache-2.0
"""Regression tests for AEN CC3501E GPIO routes and bridge helpers."""
from __future__ import annotations

import csv
import re
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
METADATA = REPO / "metadata"

AEN_SKUS = (
    "E1M-AEN301",
    "E1M-AEN401",
    "E1M-AEN501",
    "E1M-AEN601",
    "E1M-AEN701",
    "E1M-AEN801",
)

EXAMPLE_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_gpio_routes.c",
)

EXAMPLE_BRIDGE_HELPERS = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-usb-firstlight" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-usb-firstlight" / "src" / "cc3501e_bridge.h",
)


def _tsv_gpio_routes() -> dict[str, int]:
    """Return E1M GPIO pad -> raw CC3501E GPIO from the AEN TSV source."""
    path = METADATA / "e1m_modules" / "aen" / "from-cc3501e.tsv"
    routes: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as f:
        rows = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            e1m_match = re.fullmatch(r"IO(\d+)", row["e1m_function"])
            gpio_match = re.fullmatch(r"GPIO_?(\d+)", row["cc3501e_function"])
            if e1m_match and gpio_match:
                routes[f"E1M_GPIO_IO{e1m_match.group(1)}"] = int(gpio_match.group(1))
    return routes


def _sku_gpio_routes(sku: str) -> dict[str, int]:
    """Return CC3501E GPIO pad_routes from one AEN SoM preset."""
    path = METADATA / "e1m_modules" / f"{sku}.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    routes: dict[str, int] = {}
    for row in doc["pad_routes"]:
        e1m = row["e1m"]
        if row.get("dispatch") == "cc3501e" and e1m.startswith("E1M_GPIO_IO"):
            routes[e1m] = int(row["dispatch_pin"])
    return routes


def _example_gpio_routes(path: Path) -> dict[str, int]:
    """Return the strong cc3501e_gpio_routes[] entries from an example."""
    text = path.read_text(encoding="utf-8")
    routes: dict[str, int] = {}
    for e1m, pin in re.findall(r"\{\s*ALP_(E1M_GPIO_IO\d+),\s*(\d+)u\s*\}", text):
        routes[e1m] = int(pin)
    return routes


def test_tsv_captures_io9_and_io16_io17_crossing():
    routes = _tsv_gpio_routes()
    assert routes["E1M_GPIO_IO9"] == 12
    assert routes["E1M_GPIO_IO16"] == 17
    assert routes["E1M_GPIO_IO17"] == 16


@pytest.mark.parametrize("sku", AEN_SKUS)
def test_aen_som_gpio_pad_routes_match_tsv_source(sku):
    assert _sku_gpio_routes(sku) == _tsv_gpio_routes()


@pytest.mark.parametrize("path", EXAMPLE_ROUTE_TABLES)
def test_example_route_tables_match_som_metadata_subset(path):
    expected = _tsv_gpio_routes()
    expected.pop("E1M_GPIO_IO17")

    assert _example_gpio_routes(path) == expected


@pytest.mark.parametrize("suffix", ("cc3501e_bridge.c", "cc3501e_bridge.h"))
def test_example_bridge_helpers_stay_in_sync(suffix):
    reference = (
        REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / suffix
    ).read_text(encoding="utf-8")
    for path in EXAMPLE_BRIDGE_HELPERS:
        if path.name == suffix:
            assert path.read_text(encoding="utf-8") == reference, f"{path} drifted"

# SPDX-License-Identifier: Apache-2.0
"""Regression tests for AEN CC3501E GPIO routes and bridge helpers."""
from __future__ import annotations

import csv
import json
import re
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
METADATA = REPO / "metadata"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"

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
            if not e1m_match:
                continue
            # A raw-GPIO row is 3 columns and carries the pad in
            # cc3501e_function; a row whose pad is claimed by a named
            # peripheral is 4 columns, with the claim in cc3501e_function and
            # the pad in cc3501e_pad.  IO16 and IO17 took the second shape when
            # the bridge's READY and SPI CSN claims were recorded (#1808), so
            # read the pad from whichever column actually holds one instead of
            # assuming the 3-column layout.
            for cell in (row["cc3501e_function"], row["cc3501e_pad"]):
                gpio_match = re.fullmatch(r"GPIO_?(\d+)", cell or "")
                if gpio_match:
                    routes[f"E1M_GPIO_IO{e1m_match.group(1)}"] = int(
                        gpio_match.group(1))
                    break
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


def _tsv_reserved_pads() -> set[int]:
    """CC3501E GPIO indices reserved from the host GPIO proxy: a TSV row
    whose e1m_function is an IOxx pad and whose cc3501e_function is a
    NAMED claim (BRIDGE_READY, BRIDGE_SPI_CSN -- #1808's peripheral-column
    convention) rather than a bare GPIOxx pad name.  Independently coded
    from scripts/gen_cc3501e_gpio_routes.py's own
    _reserved_pads_from_tsv() (both read the same TSV, but via separate
    parsing) so this test is a real cross-check, not a tautology against
    the generator's own logic."""
    path = METADATA / "e1m_modules" / "aen" / "from-cc3501e.tsv"
    reserved: set[int] = set()
    with path.open(newline="", encoding="utf-8") as f:
        rows = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            if not re.fullmatch(r"IO\d+", row["e1m_function"]):
                continue
            if re.fullmatch(r"GPIO_?\d+", row["cc3501e_function"] or ""):
                continue  # unclaimed -- an ordinary proxyable pad
            pad_match = re.fullmatch(r"GPIO_?(\d+)", row["cc3501e_pad"] or "")
            if pad_match:
                reserved.add(int(pad_match.group(1)))
    return reserved


def _composed_cc3501e_gpio_routes(board_yaml: Path) -> dict[str, int]:
    """Resolve <board_yaml>'s OWN composed-route-table (hw_rev-aware,
    scripts/alp_project.py --emit composed-route-table) and return its
    CC3501E-dispatched, non-reserved GPIO pads -- what
    scripts/gen_cc3501e_gpio_routes.py should have written for exactly
    this board.yaml.  Unlike a plain from-cc3501e.tsv read, this
    correctly differs for a board.yaml that sets som.hw_rev (#1859 PR
    review: a `som.hw_rev: r1` board must NOT be compared against r2's
    TSV-only expectation)."""
    proc = subprocess.run(
        [sys.executable, str(ALP_PROJECT), "--input", str(board_yaml),
         "--emit", "composed-route-table"],
        capture_output=True, text=True, check=True,
    )
    composed = json.loads(proc.stdout)
    reserved = _tsv_reserved_pads()
    routes: dict[str, int] = {}
    for row in composed["routes"]:
        if row.get("dispatch") != "cc3501e":
            continue
        e1m = row.get("e1m", "")
        if not re.fullmatch(r"E1M_GPIO_IO\d+", e1m):
            continue
        pin = row.get("dispatch_pin")
        if pin is None or int(pin) in reserved:
            continue
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
    # Built from THIS example's own board.yaml, hw_rev included -- not
    # from-cc3501e.tsv directly, which is production-rev (r2) only and
    # would wrongly redden this test the moment any example declares
    # `som.hw_rev: r1` (#1859 PR review: this previously hardcoded the
    # TSV's r2 set minus IO16/IO17, so it could never validate the PR's
    # own headline feature -- per-example revision-awareness).
    board_yaml = path.parent.parent / "board.yaml"
    expected = _composed_cc3501e_gpio_routes(board_yaml)

    assert _example_gpio_routes(path) == expected


@pytest.mark.parametrize("suffix", ("cc3501e_bridge.c", "cc3501e_bridge.h"))
def test_example_bridge_helpers_stay_in_sync(suffix):
    reference = (
        REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / suffix
    ).read_text(encoding="utf-8")
    for path in EXAMPLE_BRIDGE_HELPERS:
        if path.name == suffix:
            assert path.read_text(encoding="utf-8") == reference, f"{path} drifted"

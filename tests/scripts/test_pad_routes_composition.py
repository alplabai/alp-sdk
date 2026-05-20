# SPDX-License-Identifier: Apache-2.0
"""Tests for `_resolve_pad_routes` + `_compose_route` in alp_project.py.

Validates the SoM-swappability promise: given a fixed board
e1m_routes entry (e.g. EVK_PIN_BMI323_INT1 on E1M_GPIO_IO15), the
composed route changes only the dispatch path based on the active
SoM preset's pad_routes.
"""
import importlib.util
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "alp_project.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("alp_project", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["alp_project"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_resolve_pad_routes_returns_empty_dict_for_missing_block():
    mod = _load_module()
    assert mod._resolve_pad_routes({}) == {}
    assert mod._resolve_pad_routes({"pad_routes": []}) == {}
    assert mod._resolve_pad_routes({"pad_routes": None}) == {}


def test_resolve_pad_routes_indexes_by_e1m_identifier():
    mod = _load_module()
    sku_preset = {
        "pad_routes": [
            {"e1m": "E1M_GPIO_IO15", "dispatch": "cc3501e",
             "dispatch_pin": 14},
            {"e1m": "E1M_SPI1", "dispatch": "cc3501e"},
        ],
    }
    idx = mod._resolve_pad_routes(sku_preset)
    assert idx["E1M_GPIO_IO15"]["dispatch"] == "cc3501e"
    assert idx["E1M_GPIO_IO15"]["dispatch_pin"] == 14
    assert idx["E1M_SPI1"]["dispatch"] == "cc3501e"


def test_compose_route_pad_listed_in_both_layers():
    """Board role + SoM dispatch combine into one composed route."""
    mod = _load_module()
    board_route = {
        "e1m": "E1M_GPIO_IO15",
        "role": "bmi323_int1",
        "macro": "EVK_PIN_BMI323_INT1",
        "doc": "BMI323 INT1.",
    }
    pad_routes = {
        "E1M_GPIO_IO15": {
            "e1m": "E1M_GPIO_IO15", "dispatch": "cc3501e",
            "dispatch_pin": 14, "doc": "CC3501E GPIO_14.",
        },
    }
    composed = mod._compose_route("E1M_GPIO_IO15", board_route, pad_routes)
    assert composed["dispatch"] == "cc3501e"
    assert composed["dispatch_pin"] == 14
    assert composed["board_role"] == "bmi323_int1"
    assert composed["board_macro"] == "EVK_PIN_BMI323_INT1"


def test_compose_route_pad_only_in_board_falls_back_to_direct():
    """When the SoM declares no proxy for the pad, dispatch is direct."""
    mod = _load_module()
    board_route = {
        "e1m": "E1M_GPIO_IO5",
        "role": "cam_rst",
        "macro": "EVK_PIN_CAM_RST",
    }
    composed = mod._compose_route("E1M_GPIO_IO5", board_route, {})
    assert composed["dispatch"] == "direct"
    assert composed["board_role"] == "cam_rst"


def test_compose_route_som_swap_changes_dispatch_path():
    """The SoM-swappability promise in one assertion: a fixed board
    role on the same E1M pad yields different dispatch when the SoM's
    pad_routes differ.  This is the principle-3 acid test."""
    mod = _load_module()
    board_route = {
        "e1m": "E1M_GPIO_IO15",
        "role": "bmi323_int1",
        "macro": "EVK_PIN_BMI323_INT1",
    }
    aen_pad_routes = {
        "E1M_GPIO_IO15": {
            "e1m": "E1M_GPIO_IO15", "dispatch": "cc3501e",
            "dispatch_pin": 14,
        },
    }
    n93_pad_routes: dict = {}  # empty == direct
    aen_composed = mod._compose_route("E1M_GPIO_IO15", board_route, aen_pad_routes)
    n93_composed = mod._compose_route("E1M_GPIO_IO15", board_route, n93_pad_routes)
    assert aen_composed["dispatch"] == "cc3501e"
    assert n93_composed["dispatch"] == "direct"
    # Board role identical in both -- this is the "swap modules without
    # changing the board" property.
    assert aen_composed["board_macro"] == n93_composed["board_macro"]


def test_compose_route_pad_in_neither_layer():
    mod = _load_module()
    composed = mod._compose_route("E1M_GPIO_IO99", None, {})
    assert composed["dispatch"] == "direct"
    assert composed.get("board_role") is None

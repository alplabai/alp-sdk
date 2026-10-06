# SPDX-License-Identifier: Apache-2.0
"""The host header's bridge-bit / min-minor macros must match SoM pad_routes metadata."""

import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
HDR = (ROOT / "include/alp/chips/gd32g553.h").read_text(encoding="utf-8")
SOMS = sorted((ROOT / "metadata/e1m_modules").glob("E1M-V2[NM]10[123].yaml"))


def _macro(name: str) -> int:
    return int(re.search(rf"#define {name}\s+(\d+)u", HDR).group(1))


def test_bridge_bit_routes_match_header():
    assert SOMS
    expect = {
        "E1M_X_GPIO_IO15": (_macro("GD32G553_GPIO_LINE_E1M_IO15"),
                            _macro("GD32G553_IO15_IO26_MIN_PROTOCOL_MINOR")),
        "E1M_X_GPIO_IO26": (_macro("GD32G553_GPIO_LINE_E1M_IO26"),
                            _macro("GD32G553_IO15_IO26_MIN_PROTOCOL_MINOR")),
    }
    for som in SOMS:
        routes = {r["e1m"]: r for r in yaml.safe_load(som.read_text(encoding="utf-8"))["pad_routes"]}
        for pad, (bit, minor) in expect.items():
            assert (routes[pad]["bridge_bit"], routes[pad]["min_bridge_protocol"]) == (bit, minor), (som.name, pad)

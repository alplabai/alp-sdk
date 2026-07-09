# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_e1m_route_capability as route_capability  # noqa: E402


def test_e1m_x_board_refs_are_collected() -> None:
    refs = set(route_capability._board_refs("e1m-x-evk"))

    assert "E1M_X_I2C0" in refs
    assert "E1M_X_GPIO_IO9" in refs
    assert "E1M_X_PWM5" in refs


def test_e1m_x_expected_functions_use_x_namespace_names() -> None:
    assert route_capability._expected_functions("E1M_X_ADC0") == ["ADC0"]
    assert route_capability._expected_functions("E1M_X_I2C3") == [
        "I2C3.SCL",
        "I2C3.SDA",
    ]
    assert route_capability._expected_functions("E1M_ADC0") == ["ANA_S0"]


def test_v2n_direct_bus_aliases_back_x_evk_routes() -> None:
    funcs = route_capability._table_functions("v2n")

    for expected in (
        "I2C0.SCL",
        "I2C0.SDA",
        "SPI1_MISO",
        "SPI1_CS1",
        "UART0_RX",
        "UART1_TX",
        "I2S0_SCLK",
        "I2S0_SDI",
        "CAN0_RX",
        "CAN1_TX",
    ):
        assert expected in funcs


def test_v2n_allowlist_is_limited_to_currently_unbacked_x_evk_routes() -> None:
    funcs = route_capability._table_functions("v2n")
    allow = route_capability.KNOWN_UNBACKED[("e1m-x-evk", "v2n")]

    assert "E1M_X_GPIO_IO0" in allow
    ok, missing = route_capability._resolves("E1M_X_GPIO_IO0", funcs)
    assert not ok
    assert missing == ["IO0"]

    assert "E1M_X_GPIO_IO9" not in allow
    ok, missing = route_capability._resolves("E1M_X_GPIO_IO9", funcs)
    assert ok
    assert missing == []


def test_live_route_capability_gate_passes() -> None:
    proc = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "check_e1m_route_capability.py")],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )

    assert proc.returncode == 0, proc.stderr
    assert "e1m-x-evk <-> v2n" in proc.stdout

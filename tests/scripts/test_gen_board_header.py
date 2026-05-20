# SPDX-License-Identifier: Apache-2.0
"""Tests for `scripts/gen_board_header.py`.

Covers:
- `emit_board()` shape for a hand-built sample YAML dict;
- the `active_low` flag appearing in the doc comment;
- empty `e1m_routes` block returning None;
- idempotency of the real `main()` against the committed
  `metadata/boards/E1M-EVK/board.yaml`;
- macro-coverage smoke check (the generated EVK header must define
  every macro that hand-written firmware already uses).
"""

from __future__ import annotations

import hashlib
import importlib.util
import sys
from pathlib import Path
from typing import Any

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_board_header.py"
EVK_OUT = REPO / "include" / "alp" / "boards" / "alp_e1m_evk_routes.h"


@pytest.fixture(scope="module")
def gen_module():
    spec = importlib.util.spec_from_file_location(
        "gen_board_header", SCRIPT
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gen_board_header"] = mod
    spec.loader.exec_module(mod)
    return mod


def _sample_doc() -> dict[str, Any]:
    return {
        "name": "TEST-CARRIER",
        "e1m_routes": {
            "gpio": [
                {"e1m": "E1M_GPIO_IO0", "macro": "TST_PIN_BUTTON",
                 "doc": "User button."},
            ],
            "buses": [
                {"e1m": "E1M_I2C0", "macro": "TST_I2C_BUS_MAIN",
                 "doc": "Sensor bus."},
            ],
            "pwm": [
                {"e1m": "E1M_PWM0", "macro": "TST_PWM_LED",
                 "doc": "Status LED."},
            ],
        },
    }


def test_emit_board_produces_header_for_well_formed_yaml(gen_module):
    out = gen_module.emit_board("TEST-CARRIER", _sample_doc())
    assert out is not None
    assert "ALP_BOARDS_TEST_CARRIER_ROUTES_H" in out
    assert "#define TST_PIN_BUTTON" in out
    assert "E1M_GPIO_IO0" in out
    assert "#define TST_I2C_BUS_MAIN" in out
    assert "#define TST_PWM_LED" in out
    assert "DO NOT EDIT BY HAND" in out


def test_emit_board_returns_none_without_e1m_routes(gen_module):
    assert gen_module.emit_board("EMPTY", {"name": "EMPTY"}) is None
    assert gen_module.emit_board("EMPTY", {"name": "EMPTY", "e1m_routes": {}}) is None


def test_active_low_flag_renders_in_doxygen(gen_module):
    doc = {
        "name": "T",
        "e1m_routes": {
            "gpio": [
                {"e1m": "E1M_GPIO_IO0", "macro": "TST_PIN_FOO",
                 "doc": "Foo.", "active_low": True},
            ],
        },
    }
    out = gen_module.emit_board("T", doc)
    assert out is not None
    assert "Active-low." in out


def test_real_evk_header_is_idempotent(gen_module):
    """Generate twice against the committed E1M-EVK YAML; output must
    be byte-identical (idempotency is non-negotiable for code-gen)."""
    rc = gen_module.main()
    assert rc == 0
    first = EVK_OUT.read_bytes()
    rc = gen_module.main()
    assert rc == 0
    second = EVK_OUT.read_bytes()
    assert hashlib.sha256(first).hexdigest() == hashlib.sha256(second).hexdigest()


def test_real_evk_header_covers_known_macros(gen_module):
    """Smoke check: every macro that hand-written firmware already
    relies on must be present in the generated EVK header.  If a
    macro is dropped from the YAML, this test breaks the build --
    intentional: deletion needs an explicit `pytest --update-snapshots`
    -equivalent gesture, not a silent regression."""
    rc = gen_module.main()
    assert rc == 0
    out = EVK_OUT.read_text(encoding="utf-8")
    must_define = [
        # GPIO section
        "EVK_PIN_CAM_MUX_SEL",
        "EVK_PIN_ENCODER_SW",
        "EVK_PIN_CAM_RST",
        "EVK_PIN_PCIE_IOEXP_INT",
        "EVK_PIN_I2S_MUX_EN",
        "EVK_PIN_PCIE_IOEXP_RST",
        "EVK_PIN_PCIE0_I2C_EN",
        "EVK_PIN_USB2_MUX_SEL",
        "EVK_PIN_I2S_MUX_SEL",
        "EVK_PIN_BMI323_INT1",
        "EVK_PIN_W_DISABLE1",
        "EVK_PIN_W_DISABLE2",
        "EVK_PIN_M2E_SDIO_WAKE",
        "EVK_PIN_M2E_UART_WAKE",
        "EVK_PIN_SDIO_MUX_EN",
        "EVK_PIN_SDIO_MUX_SEL",
        "EVK_PIN_PCIE_MUX_PD",
        "EVK_PIN_PCIE_MUX_SEL",
        # Bus section
        "EVK_I2C_BUS_SENSORS",
        "EVK_I2C_BUS_DSI_CSI",
        "EVK_I2C_BUS_ARDUINO",
        "EVK_SPI_BUS_ARDUINO",
        "EVK_UART_PORT_DEBUG",
        "EVK_UART_PORT_ARDUINO",
        # PWM section
        "EVK_PWM_LED_RED",
        "EVK_PWM_LED_GREEN",
        "EVK_PWM_LED_BLUE",
        "EVK_ARD_PWM1",
        "EVK_ARD_PWM2",
        "EVK_ARD_PWM3",
        "EVK_ARD_PWM4",
        "EVK_MB_PWM",
    ]
    for macro in must_define:
        assert f"#define {macro}" in out, f"{macro} missing from generated header"


def test_no_clash_with_existing_alp_e1m_evk_h(gen_module):
    """The generated routes header MUST NOT also re-define macros
    that live in the surviving hand-authored sections of
    `alp_e1m_evk.h` (overlay-pad indices, mux enums, I2C addresses,
    INA236 tuning constants, on-board sensor addresses).  This
    guards against accidental over-lift in future slices."""
    rc = gen_module.main()
    assert rc == 0
    out = EVK_OUT.read_text(encoding="utf-8")
    must_not_appear = [
        # Overlay-pad indices (slice deferred)
        "EVK_PIN_OVERLAY_BASE",
        "EVK_PIN_IO_EXP_INT",
        "EVK_PIN_IO_EXP_RST",
        "EVK_PIN_AMP_FAULT",
        "EVK_PIN_AMP_ENABLE",
        "EVK_PIN_CTP_INT",
        "EVK_PIN_MB_INT",
        # On-board I2C addresses (slice deferred)
        "EVK_I2C_ADDR_ICM42670",
        "EVK_I2C_ADDR_BMI323",
        "EVK_I2C_ADDR_BMP581",
        "EVK_I2C_ADDR_TCAL9538",
        # INA236 tuning constants (slice deferred)
        "EVK_INA236_SHUNT_3V3_OHMS",
        "EVK_INA236_MAX_3V3_A",
        # ADC channel macros (slice deferred -- ADC not in gpio/buses/pwm scope)
        "EVK_ARD_A0",
        "EVK_MB_ANA",
    ]
    for macro in must_not_appear:
        assert macro not in out, (
            f"{macro} unexpectedly appears in generated header -- "
            f"slice scope is gpio/buses/pwm only"
        )

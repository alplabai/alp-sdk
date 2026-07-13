# SPDX-License-Identifier: Apache-2.0
"""Tests for `scripts/gen_board_header.py`.

Covers:
- `emit_board()` shape for a hand-built sample YAML dict;
- the `active_low` flag appearing in the doc comment;
- empty `e1m_routes` block returning None;
- idempotency of the real `main()` against the committed
  `metadata/boards/e1m-evk.yaml`;
- macro-coverage smoke check (the generated EVK header must define
  every macro that hand-written firmware already uses);
- issue #515: `i2c_devices:` (on-board I2C addresses + INA236
  calibration) reproduces the previously hand-authored values
  exactly, and the generator-level fixture proves the mechanism.
"""

from __future__ import annotations

import hashlib
import importlib.util
import re
import sys
from pathlib import Path
from typing import Any

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_board_header.py"
EVK_OUT = REPO / "include" / "alp" / "boards" / "alp_e1m_evk_routes.h"
XEVK_OUT = REPO / "include" / "alp" / "boards" / "alp_e1m_x_evk_routes.h"


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
    assert "ALP_E1M_GPIO_IO0" in out
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
    `alp_e1m_evk.h` (overlay-pad indices, mux enums).  On-board I2C
    device addresses + INA236 calibration constants were lifted into
    the `i2c_devices:` metadata block (issue #515) and are generated
    now -- see `test_real_evk_header_covers_i2c_device_macros` below.
    This guards against accidental over-lift of what's still
    hand-authored in future slices."""
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
        # ADC spellings not generated (the shipped ADC routes are
        # EVK_ADC_ARDUINO_A1..A5 / EVK_ADC_MB_AN / EVK_ADC_VBAT_SENSE)
        "EVK_ARD_A0",
        "EVK_MB_ANA",
    ]
    for macro in must_not_appear:
        assert macro not in out, (
            f"{macro} unexpectedly appears in generated header -- "
            f"slice scope is gpio/buses/pwm/i2c_devices only"
        )


def test_real_evk_header_covers_i2c_device_macros(gen_module):
    """Issue #515: on-board I2C device addresses + INA236 calibration
    are now single-sourced from `metadata/boards/e1m-evk.yaml`'s
    `i2c_devices:` block and generated -- assert every macro
    hand-written firmware relies on is still defined, with the
    bench-confirmed values preserved verbatim."""
    rc = gen_module.main()
    assert rc == 0
    out = EVK_OUT.read_text(encoding="utf-8")
    must_define = {
        "EVK_I2C_ADDR_ICM42670": "0x69u",
        "EVK_I2C_ADDR_BMI323": "0x68u",
        "EVK_I2C_ADDR_BMP581": "0x47u",
        "EVK_I2C_ADDR_TCAL9538_MAIN": "0x72u",
        "EVK_I2C_ADDR_TCAL9538_PCIE": "0x71u",
        "EVK_I2C_ADDR_TCA6408A_MAIN": "0x20u",
        "EVK_I2C_ADDR_TAS2563_LOW": "0x4Du",
        "EVK_I2C_ADDR_TAS2563_HIGH": "0x4Eu",
        "EVK_I2C_ADDR_INA236_3V3": "0x40u",
        "EVK_I2C_ADDR_INA236_1V8": "0x41u",
        "EVK_I2C_ADDR_INA236_VIO": "0x42u",
        "EVK_I2C_ADDR_INA236_VCAM0": "0x4Bu",
        "EVK_I2C_ADDR_INA236_VCAM1": "0x49u",
        "EVK_I2C_ADDR_INA236_5V": "0x4Au",
        # legacy convenience alias -- must resolve to the same instance
        "EVK_I2C_ADDR_TCAL9538": "EVK_I2C_ADDR_TCAL9538_MAIN",
        "EVK_INA236_SHUNT_3V3_OHMS": "0.020f",
        "EVK_INA236_MAX_3V3_A": "4.0f",
        "EVK_INA236_SHUNT_1V8_OHMS": "0.020f",
        "EVK_INA236_MAX_1V8_A": "4.0f",
        "EVK_INA236_SHUNT_VIO_OHMS": "0.050f",
        "EVK_INA236_MAX_VIO_A": "1.6f",
        "EVK_INA236_SHUNT_VCAM0_OHMS": "0.050f",
        "EVK_INA236_MAX_VCAM0_A": "1.6f",
        "EVK_INA236_SHUNT_VCAM1_OHMS": "0.050f",
        "EVK_INA236_MAX_VCAM1_A": "1.6f",
        "EVK_INA236_SHUNT_5V_OHMS": "0.020f",
        "EVK_INA236_MAX_5V_A": "4.0f",
    }
    # `#define <macro>   <value>` -- allow the generator's column
    # alignment (arbitrary run of spaces) between the two.
    defined = dict(
        re.findall(r"#define\s+(\S+)\s+(\S+)", out)
    )
    for macro, value in must_define.items():
        assert macro in defined, f"{macro} missing from generated header"
        assert defined[macro] == value, (
            f"{macro} = {defined[macro]!r}, expected {value!r} -- value drifted "
            f"from the hand-authored original"
        )


def test_i2c_devices_reproduce_metadata_values(gen_module):
    """Generator unit test (issue #515): feed a hand-built YAML dict
    with an `i2c_devices:` block (address + alias + calibration) and
    assert the emitted macros carry exactly the metadata's values --
    not just presence.  Complements the real-YAML coverage check
    above with a minimal, generator-only fixture."""
    doc = {
        "name": "TEST-I2C",
        "e1m_routes": {},
        "i2c_devices": [
            {
                "macro": "EVK_I2C_ADDR_FOO",
                "address": "0x42",
                "alias": "EVK_I2C_ADDR_FOO_LEGACY",
                "doc": "Test device.",
                "calibration": {
                    "shunt_macro": "EVK_INA236_SHUNT_FOO_OHMS",
                    "shunt_ohms": "0.030",
                    "max_macro": "EVK_INA236_MAX_FOO_A",
                    "max_current_a": "2.5",
                },
            },
        ],
    }
    out = gen_module.emit_board("TEST-I2C", doc)
    assert out is not None
    defined = dict(re.findall(r"#define\s+(\S+)\s+(\S+)", out))
    assert defined["EVK_I2C_ADDR_FOO"] == "0x42u"
    assert defined["EVK_I2C_ADDR_FOO_LEGACY"] == "EVK_I2C_ADDR_FOO"
    assert defined["EVK_INA236_SHUNT_FOO_OHMS"] == "0.030f"
    assert defined["EVK_INA236_MAX_FOO_A"] == "2.5f"


def test_emit_board_selects_e1m_x_pinout_for_x_routes(gen_module):
    """A board whose routes reference the E1M_X_* namespace must pull
    `<alp/e1m_x_pinout.h>`, not the 35x35 `<alp/e1m_pinout.h>`.  The
    generator derives this from the route values themselves, so no
    redundant per-board "which namespace" field is needed."""
    x_doc = {
        "name": "X-CARRIER",
        "e1m_routes": {
            "buses": [
                {"e1m": "E1M_X_I2C0", "macro": "XC_I2C_MAIN",
                 "doc": "Sensor bus."},
            ],
        },
    }
    out = gen_module.emit_board("X-CARRIER", x_doc)
    assert out is not None
    assert '#include "alp/e1m_x_pinout.h"' in out
    assert '#include "alp/e1m_pinout.h"' not in out
    assert "#define XC_I2C_MAIN" in out


def test_real_xevk_header_uses_x_pinout_and_covers_macros(gen_module):
    """The committed E1M-X-EVK YAML must generate a routes header that
    pulls the E1M-X pinout namespace and defines the XEVK_* macros
    hand-written X-EVK firmware relies on (mirrors the EVK coverage
    smoke check, one form factor over)."""
    rc = gen_module.main()
    assert rc == 0
    assert XEVK_OUT.exists(), "alp_e1m_x_evk_routes.h was not generated"
    out = XEVK_OUT.read_text(encoding="utf-8")
    assert '#include "alp/e1m_x_pinout.h"' in out
    must_define = [
        "XEVK_I2C_BUS_SENSORS",   # buses
        "XEVK_UART_PORT_DEBUG",
        "XEVK_PIN_LED_RED",       # GPIO (PWM pad as digital GPIO)
        "XEVK_PIN_BMI323_INT1",
        "XEVK_PWM_LED_RED",       # pwm
        "XEVK_ADC_MIKROBUS_AN",   # adc (renamed from ARDUINO_A0 2026-06-05:
                                  # the V2 netlist routes only mikroBUS AN
                                  # to ANA_S0)
        "XEVK_CAN_BUS0",          # can
        "XEVK_ENC_ROTARY",        # qenc
    ]
    for macro in must_define:
        assert f"#define {macro}" in out, f"{macro} missing from X-EVK header"
    # Routes must resolve to the ALP_E1M_X_* namespace.
    assert "ALP_E1M_X_I2C0" in out and "ALP_E1M_X_GPIO_PWM5" in out

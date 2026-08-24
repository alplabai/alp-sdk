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


@pytest.fixture()
def real_headers(gen_module, tmp_path, monkeypatch):
    """Run the real generator against the committed board YAMLs, redirected
    into tmp_path.  gen_module.main() would otherwise write every generated
    header (including EVK_OUT / XEVK_OUT) straight into the real
    include/alp/boards/ tree as a side effect of running this test (#973)."""
    out_dir = tmp_path / "boards"
    monkeypatch.setattr(gen_module, "OUT_DIR", out_dir)
    # main() logs each written path via out_path.relative_to(REPO); REPO is
    # looked up fresh from the module namespace at call time, so patching
    # REPO doesn't move BOARDS_DIR (it was already bound at import time);
    # OUT_DIR is redirected explicitly above.
    monkeypatch.setattr(gen_module, "REPO", tmp_path)
    rc = gen_module.main()
    assert rc == 0
    return out_dir / EVK_OUT.name, out_dir / XEVK_OUT.name


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


def test_real_evk_header_is_idempotent(gen_module, real_headers):
    """Generate twice against the committed E1M-EVK YAML; output must
    be byte-identical (idempotency is non-negotiable for code-gen)."""
    evk_out, _xevk_out = real_headers
    first = evk_out.read_bytes()
    rc = gen_module.main()
    assert rc == 0
    second = evk_out.read_bytes()
    assert hashlib.sha256(first).hexdigest() == hashlib.sha256(second).hexdigest()


def test_real_evk_header_covers_known_macros(real_headers):
    """Smoke check: every macro that hand-written firmware already
    relies on must be present in the generated EVK header.  If a
    macro is dropped from the YAML, this test breaks the build --
    intentional: deletion needs an explicit `pytest --update-snapshots`
    -equivalent gesture, not a silent regression."""
    evk_out, _xevk_out = real_headers
    out = evk_out.read_text(encoding="utf-8")
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


def test_no_clash_with_existing_alp_e1m_evk_h(real_headers):
    """The generated routes header MUST NOT also re-define macros
    that live in the surviving hand-authored sections of
    `alp_e1m_evk.h` (mux enums).  On-board I2C device addresses +
    INA236 calibration constants (#515) and the overlay-pad indices
    (#1636) were lifted into metadata and are generated now -- see
    `test_real_evk_header_covers_i2c_device_macros` and
    `test_real_evk_header_covers_overlay_pin_macros` below.  This
    guards against accidental over-lift of what's still hand-authored
    in future slices."""
    evk_out, _xevk_out = real_headers
    out = evk_out.read_text(encoding="utf-8")
    must_not_appear = [
        # ADC spellings not generated (the shipped ADC routes are
        # EVK_ADC_ARDUINO_A1..A5 / EVK_ADC_MB_AN / EVK_ADC_VBAT_SENSE)
        "EVK_ARD_A0",
        "EVK_MB_ANA",
    ]
    for macro in must_not_appear:
        assert macro not in out, (
            f"{macro} unexpectedly appears in generated header -- "
            f"slice scope is gpio/buses/pwm/i2c_devices/overlay_pins only"
        )


def test_real_evk_header_covers_overlay_pin_macros(real_headers):
    """Issue #1636: the eleven `EVK_PIN_OVERLAY_BASE + N` overlay-pad
    indices are ordinal positions in an `alp,pin-array` overlay list,
    not independent hardware facts -- N must come from the metadata
    list's order, computed by the generator, not hand-typed. Assert
    every macro is present and its N matches its position (0-based)
    in `metadata/boards/e1m-evk.yaml`'s `overlay_pins:` list, in the
    same order the hand-authored header previously hard-coded."""
    evk_out, _xevk_out = real_headers
    out = evk_out.read_text(encoding="utf-8")
    assert "#define EVK_PIN_OVERLAY_BASE ALP_E1M_GPIO_COUNT" in out
    ordered_macros = [
        "EVK_PIN_IO_EXP_INT",
        "EVK_PIN_IO_EXP_RST",
        "EVK_PIN_AMP_FAULT",
        "EVK_PIN_AMP_ENABLE",
        "EVK_PIN_MB_INT",
        "EVK_PIN_CK_DIO4",
        "EVK_PIN_CK_DIO3",
        "EVK_PIN_CK_DIO2",
        "EVK_PIN_CK_DIO1",
        "EVK_PIN_CK_RST",
        "EVK_PIN_CTP_INT",
    ]
    defined = dict(re.findall(r"#define\s+(\S+)\s+\(EVK_PIN_OVERLAY_BASE \+ (\d+)u\)", out))
    for idx, macro in enumerate(ordered_macros):
        assert macro in defined, f"{macro} missing from generated header"
        assert defined[macro] == str(idx), (
            f"{macro} = BASE + {defined[macro]}, expected BASE + {idx} -- "
            f"index drifted from list order"
        )


def test_real_evk_header_covers_i2c_device_macros(real_headers):
    """Issue #515: on-board I2C device addresses + INA236 calibration
    are now single-sourced from `metadata/boards/e1m-evk.yaml`'s
    `i2c_devices:` block and generated -- assert every macro
    hand-written firmware relies on is still defined, with the
    bench-confirmed values preserved verbatim."""
    evk_out, _xevk_out = real_headers
    out = evk_out.read_text(encoding="utf-8")
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


def test_real_xevk_header_uses_x_pinout_and_covers_macros(real_headers):
    """The committed E1M-X-EVK YAML must generate a routes header that
    pulls the E1M-X pinout namespace and defines the XEVK_* macros
    hand-written X-EVK firmware relies on (mirrors the EVK coverage
    smoke check, one form factor over)."""
    _evk_out, xevk_out = real_headers
    assert xevk_out.exists(), "alp_e1m_x_evk_routes.h was not generated"
    out = xevk_out.read_text(encoding="utf-8")
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


def test_real_xevk_header_covers_i2c_device_macros(real_headers):
    """Issue #1636: the X-EVK's five INA236 addresses + shunt/max-current
    calibration must come from `metadata/boards/e1m-x-evk.yaml`'s
    `i2c_devices:` block and be generated -- not hand-defined in
    `include/alp/boards/alp_e1m_x_evk.h`, which is the E1M-EVK's #515
    fix never having been applied to its sibling carrier. Values must
    be preserved verbatim from the previously hand-authored header."""
    _evk_out, xevk_out = real_headers
    out = xevk_out.read_text(encoding="utf-8")
    must_define = {
        "XEVK_I2C_ADDR_INA236_3V3": "0x40u",
        "XEVK_I2C_ADDR_INA236_1V8": "0x41u",
        "XEVK_I2C_ADDR_INA236_VCAM2": "0x48u",
        "XEVK_I2C_ADDR_INA236_VCAM3": "0x49u",
        "XEVK_I2C_ADDR_INA236_5V": "0x4Au",
        "XEVK_INA236_SHUNT_3V3_OHMS": "0.020f",
        "XEVK_INA236_MAX_3V3_A": "4.0f",
        "XEVK_INA236_SHUNT_1V8_OHMS": "0.020f",
        "XEVK_INA236_MAX_1V8_A": "4.0f",
        "XEVK_INA236_SHUNT_VCAM2_OHMS": "0.050f",
        "XEVK_INA236_MAX_VCAM2_A": "1.6f",
        "XEVK_INA236_SHUNT_VCAM3_OHMS": "0.050f",
        "XEVK_INA236_MAX_VCAM3_A": "1.6f",
        "XEVK_INA236_SHUNT_5V_OHMS": "0.020f",
        "XEVK_INA236_MAX_5V_A": "4.0f",
    }
    defined = dict(re.findall(r"#define\s+(\S+)\s+(\S+)", out))
    for macro, value in must_define.items():
        assert macro in defined, f"{macro} missing from generated X-EVK header"
        assert defined[macro] == value, (
            f"{macro} = {defined[macro]!r}, expected {value!r} -- value drifted "
            f"from the hand-authored original"
        )


def test_main_removes_orphaned_generated_header(gen_module, tmp_path, monkeypatch):
    """#1128(b): a renamed/deleted board YAML must take its generated
    header with it -- a stale `alp_<slug>_routes.h` left behind would
    stay committed and green forever."""
    boards_dir = tmp_path / "boards"
    boards_dir.mkdir()
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    (boards_dir / "solo.yaml").write_text(
        "name: SOLO\n"
        "e1m_routes:\n"
        "  gpio:\n"
        "    - {e1m: E1M_GPIO_IO0, macro: SOLO_PIN, doc: t}\n"
    )
    monkeypatch.setattr(gen_module, "BOARDS_DIR", boards_dir)
    monkeypatch.setattr(gen_module, "OUT_DIR", out_dir)
    monkeypatch.setattr(gen_module, "REPO", tmp_path)

    stale = out_dir / "alp_deleted_board_routes.h"
    stale.write_text("/* stale: source YAML was renamed/deleted */\n")

    rc = gen_module.main()
    assert rc == 0
    assert not stale.exists(), "orphaned header was not reconciled away"
    assert (out_dir / "alp_solo_routes.h").exists()


def test_main_rejects_slug_collision(gen_module, tmp_path, monkeypatch):
    """#1128(c): two schema-legal but distinct board names that
    `_board_slug()` folds onto the same output path must fail the
    generator, not silently let the second write clobber the first."""
    boards_dir = tmp_path / "boards"
    boards_dir.mkdir()
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    (boards_dir / "a.yaml").write_text(
        "name: FOO-BAR\n"
        "e1m_routes:\n"
        "  gpio:\n"
        "    - {e1m: E1M_GPIO_IO0, macro: A_PIN, doc: t}\n"
    )
    (boards_dir / "b.yaml").write_text(
        "name: foo_bar\n"
        "e1m_routes:\n"
        "  gpio:\n"
        "    - {e1m: E1M_GPIO_IO1, macro: B_PIN, doc: t}\n"
    )
    monkeypatch.setattr(gen_module, "BOARDS_DIR", boards_dir)
    monkeypatch.setattr(gen_module, "OUT_DIR", out_dir)
    monkeypatch.setattr(gen_module, "REPO", tmp_path)

    rc = gen_module.main()
    assert rc == 1


def test_main_rejects_identical_board_names(gen_module, tmp_path, monkeypatch):
    """#1128(c) follow-up: the ORIGINAL guard only fired when the two
    slug owners' `name:` strings differed (`slug_owners[slug] != name`),
    so the more probable instance of the clobber it names -- a preset
    YAML copy-pasted to a new file and the `name:` field never edited --
    stayed silent: both boards' `name:` is byte-identical, so the
    inequality check never tripped and the second write clobbered the
    first with rc=0. The guard must key on the slug alone."""
    boards_dir = tmp_path / "boards"
    boards_dir.mkdir()
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    (boards_dir / "a.yaml").write_text(
        "name: SAME-NAME\n"
        "e1m_routes:\n"
        "  gpio:\n"
        "    - {e1m: E1M_GPIO_IO0, macro: A_PIN, doc: t}\n"
    )
    (boards_dir / "b.yaml").write_text(
        "name: SAME-NAME\n"
        "e1m_routes:\n"
        "  gpio:\n"
        "    - {e1m: E1M_GPIO_IO1, macro: B_PIN, doc: t}\n"
    )
    monkeypatch.setattr(gen_module, "BOARDS_DIR", boards_dir)
    monkeypatch.setattr(gen_module, "OUT_DIR", out_dir)
    monkeypatch.setattr(gen_module, "REPO", tmp_path)

    rc = gen_module.main()
    assert rc == 1

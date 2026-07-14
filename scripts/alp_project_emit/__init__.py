#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board.yaml emitters -- Zephyr Kconfig / DTS overlay / native_sim overlay /
west.yml / hw-info-h / carrier route + netlist rendering for
`scripts/alp_project.py`.

Split out of the former ~2500-line `alp_project.py` monolith (issue #459)
into a single flat `alp_project_emit.py` module, then split again by
output contract into this subpackage (issue #673 Phase 1):

  - `dts.py`         -- `--emit dts-overlay` + the board-macro parsing /
                        carrier peripheral DT-wiring catalog it needs.
  - `native_sim.py`  -- `--emit native-sim-overlay`.
  - `west_libs.py`   -- `--emit west-libraries` + the per-library
                        HW-accelerator Kconfig binding loader.
  - `hw_info.py`     -- `--emit hw-info-h`.
  - `bom_netlist.py` -- `--emit composed-route-table` + `--emit
                        carrier-netlist` (shares the route-row helper so
                        the two views cannot drift on hw_rev overrides).

This file is the package root: it holds the handful of facts genuinely
shared across more than one leaf module (the E1M / E1M-X canonical GPIO
pad-name order, consumed by both `dts.py` and `native_sim.py`, and the
chip/peripheral/library Kconfig-mapping tables consumed only by
`alp_project.py` and its callers) and re-exports every name a caller
outside this package reaches for, so `from alp_project_emit import
<name>` keeps working unchanged for both `scripts/alp_project.py` and
the test suite.  The loader (`alp_project_loader.py`) resolves the
SoM/board/capability facts these emitters render; `alp_project.py`
stays the CLI entry point that wires `--emit <mode>` to the right
function.  Structural split only, no behaviour change -- see
`tests/fixtures/emit-snapshots/` and `check_emit_snapshots.py` for the
byte-identical-output pin.
"""

from __future__ import annotations

from alp_registries import peripheral_kconfig


# ---------------------------------------------------------------------
# Shared resolved model: canonical E1M / E1M-X GPIO pad order
# ---------------------------------------------------------------------
#
# Both `dts.py` (`--emit dts-overlay`) and `native_sim.py` (`--emit
# native-sim-overlay`) render the `alp,pin-array` positional map and
# must agree byte-for-byte on the pad order -- this is the "Devicetree /
# overlay invariant" documented in e1m_pinout.h / e1m_x_pinout.h.  Kept
# here (not duplicated in either leaf) so the two overlays cannot drift.


def _e1m_gpio_canonical() -> list[str]:
    """Return the 52 ALP_E1M_GPIO_<suffix> names in canonical index order."""
    names: list[str] = [f"IO{n}" for n in range(26)]      # 0..25
    names += [f"PWM{n}" for n in range(8)]                 # 26..33
    for e in range(4):                                     # 34..41
        names += [f"ENC{e}_X", f"ENC{e}_Y"]
    names += [f"ADC{n}" for n in range(8)]                 # 42..49
    names += ["DAC0", "DAC1"]                              # 50..51
    return names


def _e1m_x_gpio_canonical() -> list[str]:
    """Return the 99 ALP_E1M_X_GPIO_<suffix> names in canonical index order."""
    names: list[str] = [f"IO{n}" for n in range(36)]       # 0..35
    names += [f"PWM{n}" for n in range(8)]                 # 36..43
    for e in range(4):                                     # 44..51
        names += [f"ENC{e}_X", f"ENC{e}_Y"]
    names += [f"ADC{n}" for n in range(8)]                 # 52..59
    names += ["DAC0", "DAC1"]                              # 60..61
    names += ["I2C2_SDA", "I2C2_SCL", "I2C3_SDA", "I2C3_SCL"]
    names += ["SPI2_MISO", "SPI2_MOSI", "SPI2_SCLK", "SPI2_CS0", "SPI2_CS1"]
    names += ["CAN1_H", "CAN1_L"]
    names += [f"LCD_B{n}" for n in range(24)]
    names += ["LCD_HSYNC", "LCD_VSYNC"]                    # 97..98
    return names


# Chip name -> Zephyr subsystem CONFIG_* keys the chip driver
# depends on.  Mirrors the `depends on ...` line in each
# `config ALP_SDK_CHIP_<NAME>` entry in zephyr/Kconfig: enabling
# a chip driver doesn't auto-select its subsystem, so the loader
# emits the matching `CONFIG_<SUBSYS>=y` here.  Not consumed inside
# this package -- re-exported for `alp_project.py` / `alp_orchestrate`.
_CHIP_SUBSYSTEMS: dict[str, tuple[str, ...]] = {
    # GPIO-only
    "button_led":         ("GPIO",),
    "cam_mux_pi3wvr626":  ("GPIO",),
    # SPI + GPIO
    "ssd1331":            ("SPI", "GPIO"),
    "cc3501e":            ("SPI", "GPIO"),
    # I2C + GPIO
    "tas2563":            ("I2C", "GPIO"),
    # I2C-only
    "lsm6dso":            ("I2C",),
    "ssd1306":            ("I2C",),
    "bme280":             ("I2C",),
    "lis2dw12":           ("I2C",),
    "ov5640":             ("I2C",),
    "icm42670":           ("I2C",),
    "bmi323":             ("I2C",),
    "bmp581":             ("I2C",),
    "tmp112":             ("I2C",),
    "rv3028c7":           ("I2C",),
    "optiga_trust_m":     ("I2C",),
    "eeprom_24c128":      ("I2C",),
    "tcal9538":           ("I2C",),
    "ina236":             ("I2C",),
    # pdm_mic helper has no subsystem dep declared in Kconfig
    # (uses <alp/i2s.h> when enabled at v0.2+).
    # v0.5 §D.AI batch -- 18 vision / display / accelerator chips.
    "ov2640":             ("I2C",),
    "ov5645":             ("I2C",),
    "ov7670":             ("I2C",),
    "ov9281":             ("I2C",),
    "ar0234":             ("I2C",),
    "imx219":             ("I2C",),
    "imx477":             ("I2C",),
    "gc2145":             ("I2C",),
    "ti_ds90ub953_954":   ("I2C",),
    "maxim_max9295_9296": ("I2C",),
    "st7789":             ("SPI", "GPIO"),
    "ili9341":            ("SPI", "GPIO"),
    "ili9488":            ("SPI", "GPIO"),
    "ra8875":             ("SPI",),
    "sh1106":             ("I2C",),
    "il3820":             ("SPI", "GPIO"),
    "gdew0154t8":         ("SPI", "GPIO"),
    "hailo_8l":           ("GPIO",),
    # v0.5 §D.industrial batch -- 18 industrial sensing / control chips.
    "bmp390":             ("I2C",),
    "ms5611":             ("I2C",),
    "lps22hb":            ("I2C",),
    "vl53l1x":            ("I2C",),
    "vl53l5cx":           ("I2C",),
    "a02yyuw":            ("SERIAL",),
    "drv8833":            ("PWM",),
    "drv8825":            ("PWM", "GPIO"),
    "tmc2209":            ("SERIAL",),
    "a4988":              ("PWM", "GPIO"),
    "as5048a_b":          ("I2C",),
    "mt6701":             ("I2C",),
    "hx711":              ("GPIO",),
    "max31855":           ("SPI",),
    "max31865":           ("SPI",),
    "tsl2591":            ("I2C",),
    "qmc5883l":           ("I2C",),
    "veml7700":           ("I2C",),
    # v0.5 §D.iot batch -- 9 IoT / connectivity chips.
    "quectel_bg95":       ("SERIAL",),
    "quectel_bg77":       ("SERIAL",),
    "ublox_sara_r5":      ("SERIAL",),
    "semtech_sx1262":     ("SPI", "GPIO"),
    "semtech_sx1276":     ("SPI", "GPIO"),
    "ublox_neo_m9n":      ("SERIAL",),
    "ublox_max_m10s":     ("SERIAL",),
    "atgm336h":           ("SERIAL",),
    "atecc608b":          ("I2C",),
    # v0.5 §D.audio batch -- 6 audio chips.
    "ics_43434":          (),                 # no Zephyr subsystem dep; sample flow via <alp/i2s.h>
    "inmp441":            (),
    "wm8960":             ("I2C",),
    "tlv320aic3204":      ("I2C",),
    "max98357a":          ("GPIO",),
    "es8388":             ("I2C",),
}


# Peripheral name (from board.yaml's `peripherals:` array) -> Zephyr Kconfig
# symbols.  Single-sourced in metadata/registries/peripheral-kconfig.json and
# shared with alp_orchestrate/slugs.py.
_PERIPHERAL_KCONFIG: dict[str, tuple[str, ...]] = peripheral_kconfig()


# ---------------------------------------------------------------------
# Dispatcher: re-export every leaf module's public emit surface
# ---------------------------------------------------------------------
#
# Every name below is a compat re-export so `from alp_project_emit import
# <name>` -- as used by `scripts/alp_project.py` and the test suite --
# keeps working unchanged after the split.

from .dts import (  # noqa: E402,F401  (compat re-export)
    _board_header_path,
    _catalog_generic_alias_indices,
    _catalog_owned_alias_indices,
    _emit_aen_adc_wiring,
    _emit_dts_overlay,
    _parse_board_macros,
    _project_pin_indices,
    _read_board_header_with_includes,
    _route_indices_for_catalog,
)
from .native_sim import _emit_native_sim_overlay  # noqa: E402,F401
from .west_libs import (  # noqa: E402,F401  (compat re-export)
    _SOC_FAMILY_TOKEN,
    _emit_library_hw_backends,
    _emit_west_libraries,
    _load_curated_library_manifest,
)
from .hw_info import _emit_hw_info_h, _pick_primary_core_os  # noqa: E402,F401
from .bom_netlist import (  # noqa: E402,F401  (compat re-export)
    _block_bom_row,
    _carrier_bom_rows,
    _chip_bom_row,
    _composed_route_rows,
    _emit_carrier_netlist,
    _emit_composed_route_table,
    _load_optional_manifest,
    _manifest_path,
    _passive_rows,
    _route_to_net,
)

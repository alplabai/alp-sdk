#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
board.yaml emitters -- Zephyr Kconfig / DTS overlay / native_sim overlay /
west.yml / hw-info-h / carrier route + netlist rendering for
`scripts/alp_project.py`.

Split out of the former ~2500-line `alp_project.py` monolith (issue #459):
this module owns every `--emit` surface's rendering logic; the loader
(`alp_project_loader.py`) resolves the SoM/board/capability facts these
emitters render, and `alp_project.py` stays the CLI entry point that wires
`--emit <mode>` to the right function.  Structural split only, no
behaviour change -- see `tests/fixtures/emit-snapshots/` and
`check_emit_snapshots.py` for the byte-identical-output pin.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: PyYAML is required.  Install via `pip install pyyaml`.")

from alp_registries import peripheral_kconfig

from alp_project_loader import (
    METADATA_ROOT,
    REPO,
    _compose_route,
    _hwrev_pad_route_overrides,
    _load_yaml,
    _resolve_pad_routes,
    _resolve_silicon_variant,
    _sku_family,
    _sku_form_factor,
    resolve_capabilities,
)


# ---------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------


# §D.lib.loader: map _sku_family() return values to the soc_family
# tokens used in metadata/library-profiles/<name>/hw-backends.yaml.
_SOC_FAMILY_TOKEN: dict[str, str] = {
    "aen":    "alif_ensemble",
    "v2n":    "renesas_rzv2n",
    "v2n-m1": "renesas_rzv2n",     # DEEPX add-on; HW-acc tokens still resolve via host family.
    "imx93":  "nxp_imx9",
}


def _emit_library_hw_backends(libs: list[str], sku: str) -> list[str]:
    """Per-library HW-accelerator binding loader.

    For each enabled library that ships a
    `metadata/library-profiles/<name>/hw-backends.yaml`, pick the
    highest-priority implemented matching backend per accelerator class
    given the active SoM SKU and emit the matching `CONFIG_*=y` line.

    Match rules (per priority entry; checked in order, all specified
    keys must match):
      - `silicon:` key, if present, must equal the SKU's `silicon:`
        ref exactly (e.g. `alif:ensemble:e4`).  Lets us pin a backend
        to specific SoCs in a family -- Ethos-U85 on E4/E6/E8, but
        not on E3/E5/E7 which carry only U55.
      - `soc_family:` key, if present, must equal the SKU family
        token (`alif_ensemble`, `renesas_rzv2n`, ...).  Selects every
        SKU in that family.
      - `requires_cap:` key, if present, must name a capability flag
        in the SKU's `metadata/e1m_modules/<sku>.yaml` `capabilities:`
        block that resolves to a truthy value (`true` or a non-zero
        count).  Cleanest matcher when an accelerator is shared
        across families (e.g. `optiga_trust_m` is populated on AEN
        + V2N).
      - All three omitted = universal entry (e.g. plain FPU, generic
        DMA), matches any SKU.
      - `status: planned` / `status: stub` entries are retained as
        metadata for the roadmap but are not emitted as active build
        claims.
    """
    import re
    from pathlib import Path

    family       = _sku_family(sku)
    soc_token    = _SOC_FAMILY_TOKEN.get(family)
    if soc_token is None:
        return []

    out: list[str] = []
    repo_root = Path(__file__).resolve().parent.parent

    # Resolve the SKU's `silicon:` ref and merged capabilities (SoC JSON
    # defaults + SoM-level overrides) via resolve_capabilities().  This
    # replaces the former inline YAML text-parser so that silicon-determined
    # capabilities removed from SoM YAMLs (Task 3, slice 3b) continue to
    # resolve from the SoC JSON that sibling Agent D populated.
    silicon_ref: str | None = None
    sku_path = repo_root / "metadata" / "e1m_modules" / f"{sku}.yaml"
    sku_preset: dict[str, Any] = {}
    if sku_path.exists():
        sku_preset = _load_yaml(sku_path) or {}
        silicon_ref = sku_preset.get("silicon")

    merged_caps: dict[str, Any] = resolve_capabilities(sku_preset, repo_root / "metadata")

    def _cap_truthy(name: str) -> bool:
        v = merged_caps.get(name)
        if v is None:
            return False
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return v > 0
        # String value from a YAML-loaded dict (should not occur after
        # resolve_capabilities, but guard for safety).
        sv = str(v).lower()
        if sv in ("true", "yes"):
            return True
        if sv in ("false", "no", "null", "none", "0"):
            return False
        try:
            return int(sv) > 0
        except ValueError:
            return False

    for lib in libs:
        prof = repo_root / "metadata" / "library-profiles" / lib / "hw-backends.yaml"
        if not prof.exists():
            continue
        try:
            text = prof.read_text(encoding="utf-8")
        except OSError:
            continue

        # Cheap line-driven parse: every `      - { ... kconfig: CONFIG_X=y }`
        # entry sits inside a `priority:` block; we walk top-down to keep
        # the per-class first-match.  No yaml dependency on the loader.
        per_class_emitted: set[str] = set()
        current_class: str | None   = None
        for raw in text.splitlines():
            cls_match = re.match(r"^\s*-\s*class:\s*(\S+)", raw)
            if cls_match:
                current_class = cls_match.group(1)
                continue
            if current_class is None or current_class in per_class_emitted:
                continue
            entry = re.match(r"^\s*-\s*\{\s*(.+)\s*\}\s*$", raw)
            if not entry:
                continue
            kv: dict[str, str] = {}
            for tok in entry.group(1).split(","):
                tok = tok.strip()
                if not tok or ":" not in tok:
                    continue
                k, v = tok.split(":", 1)
                kv[k.strip()] = v.strip()
            sili = kv.get("silicon")
            sf   = kv.get("soc_family")
            cap  = kv.get("requires_cap")
            kcv  = kv.get("kconfig")
            status = kv.get("status", "implemented").strip().lower()
            if not kcv:
                continue
            if status in {"planned", "stub"}:
                continue
            # All specified matchers must succeed.
            if sili is not None and sili != silicon_ref:
                continue
            if sf is not None and sf != soc_token:
                continue
            if cap is not None and not _cap_truthy(cap):
                continue
            out.append(f"{kcv}  # {lib} / {current_class}")
            per_class_emitted.add(current_class)

    return out


# Chip name -> Zephyr subsystem CONFIG_* keys the chip driver
# depends on.  Mirrors the `depends on ...` line in each
# `config ALP_SDK_CHIP_<NAME>` entry in zephyr/Kconfig: enabling
# a chip driver doesn't auto-select its subsystem, so the loader
# emits the matching `CONFIG_<SUBSYS>=y` here.
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


# Library-name -> Kconfig flag(s) to set when the library appears
# in board.yaml's `libraries:` array.  Only USER-facing libraries are
# listed here -- SDK-internal libs (LwRB for audio DMA staging,
# nanopb for mproc IPC framing) are pulled in unconditionally by
# their consumer code, no enable flag needed.
_LIBRARY_KCONFIG: dict[str, tuple[str, ...]] = {
    # User-facing C++ libs (Tier 1) -- header-only, no Kconfig
    # in Zephyr; the loader just adds the profile dir to the
    # include path via a v0.4 CMake hook.  The TODO comment
    # surfaces in the emitted alp.conf so consumers can see
    # what's pending.
    "etl":           ("# etl: include path + etl_profile.h via the v0.4 loader hook",),
    "fmt":           ("# fmt: include path + fmt_config.h via the v0.4 loader hook",),
    "nlohmann_json": ("# nlohmann_json: include path + json_config.h via the v0.4 loader hook",),
    "doctest":       ("# doctest: include path + doctest_config.h via the v0.4 loader hook",),
    # Zephyr-native libs (Tier 3) -- the SDK forwards the
    # consumer's intent to Zephyr's own Kconfig + adds the profile
    # header to the include path.
    # Baseline Tier 3 libs: emit the upstream Zephyr Kconfig + the
    # matching ALP-side SW-fallback knob.  The SW-fallback line is
    # redundant with Kconfig.alp-libraries' `default y`, but emitting
    # it explicitly in alp.conf documents the fallback choice next
    # to the library-enable line.
    "lvgl":          ("CONFIG_LVGL=y",
                      "CONFIG_ALP_LVGL_SW_BLIT=y"),
    "mbedtls":       ("CONFIG_MBEDTLS=y", "CONFIG_MBEDTLS_BUILTIN=y",
                      "CONFIG_ALP_MBEDTLS_PURE_C=y"),
    "cmsis_dsp":     ("CONFIG_CMSIS_DSP=y",
                      # CMSIS-DSP's per-component switches are off by default in
                      # the upstream Zephyr module -- enabling CMSIS_DSP alone
                      # only pulls in BASICMATH.  We turn on every component
                      # consumers might reach so kernels like arm_rfft_fast_*
                      # (TRANSFORM), arm_biquad_cascade_* (FILTERING),
                      # arm_correlate_* (STATISTICS), etc. link cleanly.  Cost
                      # is minimal -- LD's --gc-sections drops unused symbols.
                      "CONFIG_CMSIS_DSP_BASICMATH=y",
                      "CONFIG_CMSIS_DSP_COMPLEXMATH=y",
                      "CONFIG_CMSIS_DSP_CONTROLLER=y",
                      "CONFIG_CMSIS_DSP_FASTMATH=y",
                      "CONFIG_CMSIS_DSP_FILTERING=y",
                      "CONFIG_CMSIS_DSP_INTERPOLATION=y",
                      "CONFIG_CMSIS_DSP_MATRIX=y",
                      "CONFIG_CMSIS_DSP_STATISTICS=y",
                      "CONFIG_CMSIS_DSP_SUPPORT=y",
                      "CONFIG_CMSIS_DSP_TRANSFORM=y",
                      "CONFIG_ALP_CMSIS_DSP_SCALAR=y"),
    "littlefs":      ("CONFIG_FILE_SYSTEM_LITTLEFS=y", "CONFIG_FILE_SYSTEM=y",
                      "CONFIG_ALP_LITTLEFS_SYNC_IO=y"),

    # v0.5 §D.lib batch -- 17 new libraries.  Per-library hardware-
    # accelerator binding is declared in
    # metadata/library-profiles/<name>/hw-backends.yaml and emitted
    # by the §D.lib.loader hook (next commit).  The entries here just
    # surface the per-library include-path + base-Kconfig hook so the
    # consumer's `libraries: [...]` enumeration works end-to-end.
    # SW-fallback CONFIG_* knobs are emitted unconditionally; the
    # HW-backend CONFIG_* knobs come from the cross-reference loader.

    # §D.lib.ai
    "tflite_micro":   ("CONFIG_ALP_TFLM_REF_KERNELS=y",
                       "# tflite_micro: include path + tflm_config.h via v0.4 loader hook",),
    "u8g2":           ("CONFIG_ALP_U8G2_SW_BLIT=y",
                       # Compiles vendors/u8g2/csrc/ into the build (see
                       # zephyr/CMakeLists.txt) -- unlike ALP_U8G2_SW_BLIT
                       # above (a fallback-capability marker, `default y`
                       # for every build), this gates a real compiled
                       # source addition and must stay selection-scoped.
                       "CONFIG_ALP_SDK_U8G2_VENDORED_CORE=y",),
    "gfx_compat":     ("CONFIG_ALP_GFX_COMPAT_SW=y",
                       "# gfx_compat: maintainer-shipped thin shim; no external dep",),

    # §D.lib.industrial
    "madgwick_ahrs":  ("CONFIG_ALP_SDK_AHRS=y", "CONFIG_ALP_MADGWICK_LIBM=y"),
    "pid":            ("CONFIG_ALP_SDK_PID=y", "CONFIG_ALP_PID_INT_MATH=y"),
    "modbus":         ("CONFIG_ALP_MODBUS_SYNC_IO=y",),

    # §D.lib.iot
    "coremqtt_sn":    ("CONFIG_ALP_MQTTSN_NO_TLS=y",),
    "libcoap":        ("CONFIG_ALP_COAP_NO_TLS=y",),
    "tinygsm":        ("CONFIG_ALP_TINYGSM_SYNC_IO=y",),
    "nanopb":         ("CONFIG_ALP_NANOPB_SW=y",),
    "libwebsockets":  ("CONFIG_ALP_LWS_NO_TLS=y",),
    "jsmn":           ("CONFIG_ALP_JSMN_SW=y",),
    "bearssl":        ("CONFIG_ALP_BEARSSL_PURE_C=y",),

    # §D.lib.audio
    "minimp3":        ("CONFIG_ALP_MINIMP3_PURE_C=y",),
    "opus":           ("CONFIG_ALP_OPUS_PURE_C=y",),
    "libhelix":       ("CONFIG_ALP_LIBHELIX_PURE_C=y",),

    # §D.lib.test
    "catch2":         ("CONFIG_ALP_CATCH2_SW=y",
                       # Compiles vendors/catch2/src/catch_amalgamated.cpp
                       # into the build (see zephyr/CMakeLists.txt) --
                       # unlike ALP_CATCH2_SW above (a fallback-capability
                       # marker, `default y` for every build), this gates
                       # a real C++ compiled source addition and must stay
                       # selection-scoped so a C-only app never pulls in a
                       # C++ TU it has no libstdc++ linked for.
                       "CONFIG_ALP_SDK_CATCH2_VENDORED=y",),
}


# ---------------------------------------------------------------------
# DTS overlay emission (v0.3: i2c / spi / uart / pwm / gpio aliases)
# ---------------------------------------------------------------------
#
# Per the project memory note "pending exact hardware configurations
# -- mark unknowns TBD, never invent values", the loader translates
# the macros in include/alp/boards/<board>.h verbatim; it does not
# invent gpio bank numbers or per-pad GPIO_ACTIVE_* flags.  The
# emitted .overlay declares the board's bus aliases and a stub
# alp,pin-array with one entry per EVK_PIN_* macro, each annotated
# with a comment naming the macro and the ALP_E1M_GPIO_IO<N> it
# resolves to.  Customers fill the gpio bank / index columns with
# their SoM's actual DT controller phandles once the upstream board
# files land in alplabai/alp-zephyr-modules.
#
# Bus phandle naming convention matches the manually-written EVK
# overlays at tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay:
# &i2c<N>, &spi<N>, &uart<N>, &pwm<N>.  Per-SoC vendor DT may use
# alternate names (e.g. &lpi2c0 on some Alif boards); the customer
# fixes the phandle if their board file diverges -- the loader's
# job is to surface every alias the board wants, not to second-
# guess vendor DT naming.

# Match `#define <NAME> ALP_E1M_<CLASS><N>` (with optional trailing
# token).  Class is one of the bus / pwm / gpio / analog-converter
# names we care about.  ADC + DAC join the set so the portable
# <alp/adc.h> / <alp/dac.h> backends -- which resolve their channels
# via the `alp-adcN` / `alp-dacN` DT aliases -- get a generated alias
# scaffold from the board's `e1m_routes.adc` / `.dac` entries.
_DEFINE_E1M_RE = re.compile(
    r"^\s*#\s*define\s+(\w+)\s+ALP_E1M_(I2C|SPI|UART|PWM|ADC|DAC|GPIO_IO)(\d+)\b",
    re.MULTILINE,
)

# Bus-alias buckets the loader emits.  Each entry maps the e1m_pinout
# class name -> (alias prefix, Zephyr DT phandle prefix).
#
# The phandle prefix is the convention-default node-label (&i2c0,
# &adc0, ...); vendor DT may use a different label (e.g. the Alif
# Ensemble ADCs are node-labelled `adc12_0` and the EEPROM bus is
# `i2c2`), in which case the per-app board overlay repoints the alias
# (`aliases { alp-adc0 = &adc12_0; };`) -- the loader's job is to
# surface every alias the board wires, not to second-guess vendor DT
# node-label naming.
_BUS_BUCKETS: tuple[tuple[str, str, str], ...] = (
    ("I2C",  "alp-i2c",  "i2c"),
    ("SPI",  "alp-spi",  "spi"),
    ("UART", "alp-uart", "uart"),
    ("PWM",  "alp-pwm",  "pwm"),
    ("ADC",  "alp-adc",  "adc"),
    ("DAC",  "alp-dac",  "dac"),
)


# Canonical E1M GPIO index order -- e1m_pinout.h "Devicetree / overlay
# invariant".  The alp,pin-array `gpios` property MUST list these 52
# entries in this exact order so the GPIO backend's positional resolve
# (alp_z_gpio_resolve -> alp_pins[pin_id]) lands on the right pad,
# including secondary-function pads opened as GPIO via ALP_E1M_GPIO_<class><N>
# (PWM -> 26..33, ENC -> 34..41, ADC -> 42..49, DAC -> 50..51).
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


def _strip_c_comments(text: str) -> str:
    """Strip /* ... */ and // ... comments from C source text."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _collapse_line_continuations(text: str) -> str:
    """Join `\\<newline>` continuations into single logical lines so a
    multi-line `#define NAME \\\n    VALUE` shows up as one line."""
    return re.sub(r"\\\s*\n\s*", " ", text)


def _board_header_path(board_name: str, repo_root: Path) -> Path:
    """Resolve include/alp/boards/alp_<board>.h for a board name.

    Example: 'E1M-EVK' -> include/alp/boards/alp_e1m_evk.h.
    """
    fname = "alp_" + board_name.lower().replace("-", "_") + ".h"
    return repo_root / "include" / "alp" / "boards" / fname


_INCLUDE_LOCAL_RE = re.compile(
    r'^\s*#\s*include\s+"(alp/boards/[^"]+\.h)"', re.MULTILINE,
)


def _read_board_header_with_includes(header_path: Path) -> str:
    """Read `header_path` and inline any `#include "alp/boards/<file>.h"`
    that exists under include/.  Used so the loader picks up the
    generated routes header (alp_e1m_evk_routes.h) which holds the
    EVK_* -> ALP_E1M_* macro bindings since slice 1c.

    Single-level inlining is sufficient -- the generated routes header
    only `#include`s `alp/e1m_pinout.h`, which carries no EVK_* macros.
    """
    text = header_path.read_text(encoding="utf-8")
    include_root = header_path.parent.parent.parent  # .../include/
    pieces: list[str] = [text]
    for m in _INCLUDE_LOCAL_RE.finditer(text):
        inc_rel = m.group(1)
        inc_path = include_root / inc_rel
        if inc_path.is_file() and inc_path.resolve() != header_path.resolve():
            pieces.append(inc_path.read_text(encoding="utf-8"))
    return "\n".join(pieces)


def _parse_board_macros(
    header_path: Path,
) -> dict[str, list[tuple[str, int]]]:
    """Return {class_name: [(macro_name, channel_index), ...]} for
    each ALP_E1M_<CLASS><N> reference in the board header."""
    raw = _read_board_header_with_includes(header_path)
    text = _strip_c_comments(_collapse_line_continuations(raw))
    out: dict[str, list[tuple[str, int]]] = {
        "I2C": [], "SPI": [], "UART": [], "PWM": [], "GPIO_IO": [],
    }
    for m in _DEFINE_E1M_RE.finditer(text):
        macro_name = m.group(1)
        cls = m.group(2)
        idx = int(m.group(3))
        out.setdefault(cls, []).append((macro_name, idx))
    return out


def _project_pin_indices(project: dict[str, Any], class_name: str) -> set[int]:
    """Return E1M route indices for one class named in board.yaml `pins:`."""
    pattern = re.compile(rf"^E1M_{re.escape(class_name)}(\d+)$")
    indices: set[int] = set()
    for pin in project.get("pins", []) or []:
        if isinstance(pin, str):
            e1m = pin
        elif isinstance(pin, dict):
            e1m = pin.get("e1m")
        else:
            continue
        if not isinstance(e1m, str):
            continue
        m = pattern.match(e1m)
        if m:
            indices.add(int(m.group(1)))
    return indices


def _route_indices_for_catalog(
    project: dict[str, Any],
    macros: dict[str, list[tuple[str, int]]],
    class_name: str,
    default_indices: set[int],
) -> set[int]:
    """Route indices a SoM-family catalog entry should own.

    Apps that name concrete `pins:` get exactly those aliases.  Older or
    chip-bound examples without `pins:` keep the catalog's historical default
    instance, e.g. AEN portable I2C0 -> SoC i2c2.
    """
    requested = _project_pin_indices(project, class_name)
    if requested:
        return requested
    board_indices = {idx for _macro, idx in macros.get(class_name, [])}
    return board_indices & default_indices


def _emit_aen_adc_wiring(indices: set[int]) -> str:
    """Emit AEN ADC consumer nodes for the requested portable ADC ids."""
    ordered = sorted(indices)
    lines: list[str] = ["/ {"]
    for idx in ordered:
        lines.append(f"\talp_adc_in{idx}: alp-adc-in{idx} {{")
        lines.append('\t\tcompatible = "alp,adc-input";')
        lines.append(f"\t\tio-channels = <&adc12_0 {idx}>;")
        lines.append("\t};")
    lines.append("\taliases {")
    for idx in ordered:
        lines.append(f"\t\talp-adc{idx} = &alp_adc_in{idx};")
    lines.append("\t};")
    lines.append("};")
    lines.append("&adc12_0 {")
    lines.append('\tstatus = "okay";')
    for idx in ordered:
        lines.append(f"\tchannel@{idx} {{")
        lines.append(f"\t\treg = <{idx}>;")
        lines.append('\t\tzephyr,gain = "ADC_GAIN_1";')
        lines.append('\t\tzephyr,reference = "ADC_REF_INTERNAL";')
        lines.append("\t\tzephyr,vref-mv = <1800>;")
        lines.append("\t\tzephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;")
        lines.append("\t\tzephyr,resolution = <12>;")
        lines.append("\t};")
    lines.append("};")
    return "\n".join(lines) + "\n"


def _catalog_owned_alias_indices(
    fam: str,
    phandle_prefix: str,
    project: dict[str, Any],
    macros: dict[str, list[tuple[str, int]]],
) -> set[int]:
    """Return portable alias indices emitted by SoM-family special wiring."""
    if fam == "aen" and phandle_prefix == "i2c":
        return _route_indices_for_catalog(project, macros, "I2C", {0}) & {0}
    if fam == "aen" and phandle_prefix == "adc":
        return _route_indices_for_catalog(project, macros, "ADC", {0})
    return set()


def _catalog_generic_alias_indices(
    fam: str,
    phandle_prefix: str,
    project: dict[str, Any],
    macros: dict[str, list[tuple[str, int]]],
    class_name: str,
) -> set[int]:
    """Return generic alias indices to emit for a catalog-owned class."""
    if fam == "aen" and phandle_prefix == "i2c":
        return _route_indices_for_catalog(project, macros, class_name, {0})
    if fam == "aen" and phandle_prefix == "adc":
        return _route_indices_for_catalog(project, macros, class_name, {0})
    return {idx for _macro, idx in macros.get(class_name, [])}


# ---------------------------------------------------------------------
# Carrier peripheral DT-wiring catalog (single source of truth)
# ---------------------------------------------------------------------
#
# Declaring a peripheral in board.yaml (`cores.<id>.peripherals`) sets the
# subsystem CONFIG via the conf emit -- but that alone does NOT bind hardware:
# the controller node sits `disabled` in the SoC dtsi, so e.g. ADC_ALIF never
# selects and `alp adc read` returns -ENOENT.  Examples used to paper over this
# with a hand-written boards/*.overlay enabling the node + the alp-<x>N alias /
# io-channels consumer the portable backends resolve.
#
# This catalog moves that wiring into the codegen: `_emit_dts_overlay` renders
# the fragment for each declared peripheral, so `peripherals: [adc]` ALONE
# yields a working `alp adc read` with NO per-example overlay.  Keyed by SoM
# family (from `_sku_family`); each entry carries the dt-bindings `#include`s it
# needs + a self-contained DTS fragment (DT permits repeated `/{}` and
# `&label{}` sections).  To add or fix a peripheral's wiring, edit ONE entry
# here -- never per example.  A per-example boards/*.overlay still layers last
# and wins (override tier), so this is opt-in-complete, not a straitjacket.
_PERIPH_DT_WIRING: dict[str, dict[str, dict[str, Any]]] = {
    "aen": {
        "i2c": {
            "include": ["zephyr/dt-bindings/i2c/i2c.h",
                        "zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h"],
            "dts": (
                "&pinctrl {\n"
                "\tpinctrl_i2c2: pinctrl_i2c2 {\n"
                "\t\tgroup0 {\n"
                "\t\t\tpinmux = <PIN_P5_6__I2C2_SCL_C>, <PIN_P5_7__I2C2_SDA_C>;\n"
                "\t\t\tinput-enable;\n"
                "\t\t\tbias-pull-down;\n"
                "\t\t};\n"
                "\t};\n"
                "};\n"
                "&i2c2 {\n"
                "\tstatus = \"okay\";\n"
                "\tpinctrl-0 = <&pinctrl_i2c2>;\n"
                "\tpinctrl-names = \"default\";\n"
                "\tclock-frequency = <I2C_BITRATE_STANDARD>;\n"
                "};\n"
                "/ {\n"
                "\taliases {\n"
                "\t\talp-i2c0 = &i2c2;\n"
                "\t};\n"
                "};\n"
            ),
        },
        "gpio": {
            "include": [],
            "dts": "&gpio8 {\n\tstatus = \"okay\";\n};\n",
        },
        "adc": {
            "include": ["zephyr/dt-bindings/adc/adc.h"],
        },
    },
}


def _emit_dts_overlay(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_peripherals: list[str] | None = None,
    v2_core_id: str | None = None,
    v2_core_os: str | None = None,
) -> str:
    """Emit a Zephyr DTS overlay describing the board wiring.

    v1 path (`v2_peripherals is None`): reads project-level
    `peripherals:` implicitly via the board header macros.

    v2 path: the project's peripherals live under `cores.<id>.peripherals`.
    Callers compute the union across Zephyr/baremetal cores (or pick one
    when `--core <id>` is supplied) and pass it in via `v2_peripherals`.
    The list is currently informational -- the bus aliases + `alp,pin-array`
    binding root node are derived from the board header, which describes
    the SoM mounting, not the project.  When `v2_core_os` is set to a
    non-Zephyr runtime (`yocto`, `off`, ...), the emitter returns a stub
    overlay with just the header comment.
    """
    lines: list[str] = []
    lines.append("/*")
    lines.append(" * Auto-generated by scripts/alp_project.py "
                 "-- do not edit by hand.")
    lines.append(" * Regenerate after changes to board.yaml or "
                 "include/alp/boards/<board>.h.")
    lines.append(" *")
    lines.append(" * Per-pad GPIO bank/index values are TBD pending the upstream")
    lines.append(" * alp_<board>_<som>.dts board file (alplabai/alp-zephyr-modules).")
    lines.append(" * The alp,pin-array below is the full 52-entry positional map in")
    lines.append(" * e1m_pinout.h canonical order; fill the <&gpioX Y FLAGS> columns")
    lines.append(" * in place without renumbering (the positional index is the ABI).")
    lines.append(" */")
    lines.append("")

    # v2 short-circuit: a non-Zephyr core has no Zephyr overlay to emit.
    # Customer-passed `--core <id>` may target a yocto / off slice -- the
    # emitter returns a stub so the caller's pipeline doesn't fail.
    if v2_core_os is not None and v2_core_os not in ("zephyr", "baremetal"):
        lines.append(f"// --core {v2_core_id} has os: {v2_core_os}; no Zephyr DTS overlay applies.")
        return "\n".join(lines) + "\n"

    lines.append("#include <zephyr/dt-bindings/gpio/gpio.h>")
    lines.append("")

    sku = project["som"]["sku"]
    # Carrier peripheral DT-wiring catalog for this SoM family.  Computed
    # ONCE here and reused both to skip the generic bus aliases the catalog
    # owns (below) and to emit the catalog fragments at the tail.
    fam = _sku_family(sku)
    wiring = _PERIPH_DT_WIRING.get(fam, {})
    board_name = (board_preset or {}).get("name", "")
    if not board_name:
        lines.append("// No board declared in board.yaml; nothing to emit.")
        return "\n".join(lines) + "\n"

    header_path = _board_header_path(board_name, REPO)
    if not header_path.is_file():
        sys.exit(f"alp_project: no board header at "
                 f"{header_path.relative_to(REPO)} for board '{board_name}' "
                 f"-- DTS overlay emission requires one.")

    macros = _parse_board_macros(header_path)

    lines.append(f"/ {{")
    lines.append(f"    /* Board: {board_name} (SoM SKU {sku}) */")
    lines.append(f"    /* Source: include/alp/boards/{header_path.name} */")
    if v2_peripherals is not None:
        # Surface which Zephyr peripherals the v2 union resolved to so
        # consumers can correlate the alias list back to their cores.
        if v2_core_id is not None:
            lines.append(
                f"    /* v2 scope: --core {v2_core_id} peripherals: "
                f"{', '.join(v2_peripherals) if v2_peripherals else '<none>'} */"
            )
        else:
            lines.append(
                f"    /* v2 scope: union of Zephyr/baremetal cores' peripherals: "
                f"{', '.join(v2_peripherals) if v2_peripherals else '<none>'} */"
            )
    lines.append("")

    # Bus aliases -- one per unique channel the board wires.
    lines.append("    aliases {")
    for class_name, alp_prefix, phandle_prefix in _BUS_BUCKETS:
        # Catalog-owned classes (AEN i2c/adc) are instance-specific: the
        # catalog owns only the aliases it remaps, while requested sibling
        # instances can still receive the generic scaffold.
        if phandle_prefix in wiring and phandle_prefix not in set(v2_peripherals or []):
            continue
        if phandle_prefix in wiring:
            entries = sorted(
                _catalog_generic_alias_indices(fam, phandle_prefix, project, macros, class_name)
                - _catalog_owned_alias_indices(fam, phandle_prefix, project, macros)
            )
        else:
            entries = sorted(set(idx for _macro, idx in macros.get(class_name, [])))
        if not entries:
            continue
        lines.append(f"        /* {class_name} */")
        for idx in entries:
            # Comment lists every board macro that references this channel.
            referencing = [m for (m, i) in macros[class_name] if i == idx]
            comment = ", ".join(referencing)
            lines.append(
                f"        {alp_prefix}{idx} = &{phandle_prefix}{idx};"
                f"  /* {comment} */"
            )
    lines.append("    };")
    lines.append("")

    # alp,pin-array -- the 52-entry positional GPIO map.  Order is fixed
    # by e1m_pinout.h's "Devicetree / overlay invariant" so the GPIO
    # backend's positional resolve (alp_pins[pin_id]) lands on the right
    # pad, including secondary-function pads opened as GPIO via
    # ALP_E1M_GPIO_<class><N>.  Every slot is present even when the board
    # doesn't route it; <&gpioX Y FLAGS> triplets are TBD pending the
    # upstream SoM board file.
    io_by_idx = {idx: m for (m, idx) in macros.get("GPIO_IO", [])}
    pwm_by_idx = {idx: m for (m, idx) in macros.get("PWM", [])}
    canonical = _e1m_gpio_canonical()
    lines.append("    alp_pins: alp-pins {")
    lines.append('        compatible = "alp,pin-array";')
    lines.append("        /* 52 entries in E1M canonical order (e1m_pinout.h).  Indices:")
    lines.append("         *   0..25  IO0..IO25       26..33 PWM0..PWM7")
    lines.append("         *   34..41 ENC0_X..ENC3_Y  42..49 ADC0..ADC7   50..51 DAC0..DAC1")
    lines.append("         * Each <&gpioX Y FLAGS> triplet is TBD pending the upstream SoM")
    lines.append("         * board file; unrouted pads keep their slot so indices stay")
    lines.append("         * stable (alp_gpio_open of an unrouted pad returns NULL).      */")
    lines.append("        gpios =")
    for i, suffix in enumerate(canonical):
        terminator = ";" if i == len(canonical) - 1 else ","
        # Annotate IO / PWM slots with the board macro routed to that pad
        # (parsed from the board header); other classes carry the bare
        # ALP_E1M_GPIO_<suffix> so the customer knows which pad the slot is.
        routed = ""
        if suffix.startswith("IO"):
            n = int(suffix[2:])
            if n in io_by_idx:
                routed = f"  routed: {io_by_idx[n]}"
        elif suffix.startswith("PWM"):
            n = int(suffix[3:])
            if n in pwm_by_idx:
                routed = f"  default fn: {pwm_by_idx[n]}"
        lines.append(
            f"            <&gpio0 0 GPIO_ACTIVE_HIGH>{terminator}"
            f"  /* [{i:2d}] ALP_E1M_GPIO_{suffix}{routed} */"
        )
    lines.append("    };")
    lines.append("")

    lines.append("};")

    # ── carrier peripheral wiring (catalog-driven) ──────────────────────
    # For each declared peripheral carrying a _PERIPH_DT_WIRING entry, append
    # its controller node-enable + the alp-<x>N alias / io-channels consumer
    # the portable backends resolve -- so `peripherals: [adc]` ALONE binds the
    # hardware (no hand-written boards/*.overlay).  The v1 path (v2_peripherals
    # is None) emits nothing here.  A per-example overlay still layers last.
    # `fam`/`wiring` were computed once near the top of the function (and also
    # gate the generic alias skip above), so they are reused here verbatim.
    emitted: list[tuple[str, dict[str, Any], str]] = []
    for p in sorted(set(v2_peripherals or [])):
        if p not in wiring:
            continue
        entry = wiring[p]
        if fam == "aen" and p == "adc":
            indices = _route_indices_for_catalog(project, macros, "ADC", {0})
            if not indices:
                continue
            emitted.append((p, entry, _emit_aen_adc_wiring(indices)))
            continue
        if fam == "aen" and p == "i2c":
            indices = _route_indices_for_catalog(project, macros, "I2C", {0})
            if 0 not in indices:
                continue
        emitted.append((p, entry, entry.get("dts", "")))

    if emitted:
        incs: list[str] = []
        for _p, entry, _dts in emitted:
            for inc in entry.get("include", []):
                if inc not in incs:
                    incs.append(inc)
        lines.append("")
        lines.append("/* ---- carrier peripheral wiring "
                     "(auto, from board.yaml `peripherals:`) ---- */")
        for inc in incs:
            lines.append(f"#include <{inc}>")
        if incs:
            lines.append("")
        for p, _entry, dts in emitted:
            lines.append(f"/* peripheral: {p} */")
            lines.append(dts.rstrip("\n"))
            lines.append("")

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# native_sim overlay emission (canonical alp,pin-array on gpio-emul)
# ---------------------------------------------------------------------
#
# Studio + `alp init` want a native_sim board overlay so a scaffolded GPIO
# app links + runs on `native_sim/native/64` (host emulation, emulated CI)
# with no silicon.  Unlike `--emit dts-overlay` -- which stubs every
# pin-array triplet at <&gpio0 0> pending the upstream SoM board file --
# gpio-emul IS the backing controller under native_sim, so this overlay is
# complete and directly buildable.  Keeping it here (not hand-rolled in a
# downstream scaffolder) means the E1M/E1M-X pad ABI lives in exactly one
# place: the canonical helpers above.

# zephyr,gpio-emul controllers cap at 32 pins.
_GPIO_EMUL_MAX_PINS = 32


def _emit_native_sim_overlay(project: dict[str, Any]) -> str:
    """Emit a native_sim board overlay: the canonical alp,pin-array mapped
    onto zephyr,gpio-emul controllers.

    The pin-array is the full positional map in the form-factor pinout header's
    canonical order, so alp_z_gpio_resolve(pin_id) is a direct index for
    any pad (the GPIO backend derives ALP_PIN_COUNT from this node's gpios
    length).  gpio-emul caps at 32 pins, so the pads span as many
    controllers as the active form factor needs.  Every pad defaults to
    GPIO_ACTIVE_HIGH; an app that needs a
    different polarity (e.g. an active-low button) overrides its own pad.

    GPIO only: non-GPIO peripherals (display, sensors) have no universal
    native_sim emulation, so an app that needs them adds its own emulated
    nodes (e.g. a zephyr,dummy-dc display), exactly as the curated examples
    do.
    """
    sku = project["som"]["sku"]
    if _sku_form_factor(sku) == "e1m-x":
        canonical = _e1m_x_gpio_canonical()
        form_factor = "E1M-X"
        pinout_header = "e1m_x_pinout.h"
        gpio_macro_prefix = "ALP_E1M_X_GPIO"
        index_summary = [
            "         *   0..35  IO0..IO35       36..43 PWM0..PWM7",
            "         *   44..51 ENC0_X..ENC3_Y  52..59 ADC0..ADC7   60..61 DAC0..DAC1",
            "         *   62..72 I2C2/3 + SPI2 + CAN1",
            "         *   73..98 LCD_B0..LCD_B23 + LCD_HSYNC/LCD_VSYNC */",
        ]
    else:
        canonical = _e1m_gpio_canonical()
        form_factor = "E1M"
        pinout_header = "e1m_pinout.h"
        gpio_macro_prefix = "ALP_E1M_GPIO"
        index_summary = [
            "         *   0..25  IO0..IO25       26..33 PWM0..PWM7",
            "         *   34..41 ENC0_X..ENC3_Y  42..49 ADC0..ADC7   50..51 DAC0..DAC1 */",
        ]
    n = len(canonical)
    n_ctrl = (n + _GPIO_EMUL_MAX_PINS - 1) // _GPIO_EMUL_MAX_PINS

    lines: list[str] = []
    lines.append("/*")
    lines.append(" * Auto-generated by scripts/alp_project.py "
                 "--emit native-sim-overlay -- do not edit by hand.")
    lines.append(f" * Regenerate after changes to the {pinout_header} GPIO order.")
    lines.append(" *")
    lines.append(f" * native_sim GPIO emulation for SoM SKU {sku}: the canonical")
    lines.append(" * alp,pin-array mapped onto zephyr,gpio-emul so alp_gpio_open(pin_id)")
    lines.append(f" * resolves under `west build -b native_sim/native/64`.  All {n} {form_factor} pads")
    lines.append(" * are present so any pin_id resolves; pads default to GPIO_ACTIVE_HIGH")
    lines.append(" * -- override per pad in your app for a different polarity.  GPIO only:")
    lines.append(" * add your own emulated nodes for non-GPIO peripherals (display, etc.).")
    lines.append(" */")
    lines.append("")
    lines.append("#include <zephyr/dt-bindings/gpio/gpio.h>")
    lines.append("")
    lines.append("/ {")

    # gpio-emul controllers -- one per 32-pad span.
    for c in range(n_ctrl):
        base = c * _GPIO_EMUL_MAX_PINS
        count = min(_GPIO_EMUL_MAX_PINS, n - base)
        lines.append(f"    gpio_emul{c}: gpio_emul{c} {{")
        lines.append('        compatible = "zephyr,gpio-emul";')
        lines.append('        status = "okay";')
        lines.append("        gpio-controller;")
        lines.append("        rising-edge;")
        lines.append("        falling-edge;")
        lines.append("        #gpio-cells = <2>;")
        lines.append(f"        ngpios = <{count}>;")
        lines.append("    };")
        lines.append("")

    # alp,pin-array -- the full positional map spanning the controllers.
    lines.append("    alp_pins: alp-pins {")
    lines.append('        compatible = "alp,pin-array";')
    lines.append(f"        /* {n} entries in {form_factor} canonical order ({pinout_header}).  Indices:")
    for line in index_summary:
        lines.append(line)
    lines.append("        gpios =")
    for i, suffix in enumerate(canonical):
        ctrl = i // _GPIO_EMUL_MAX_PINS
        local = i % _GPIO_EMUL_MAX_PINS
        terminator = ";" if i == n - 1 else ","
        lines.append(
            f"            <&gpio_emul{ctrl} {local:2d} GPIO_ACTIVE_HIGH>{terminator}"
            f"  /* [{i:2d}] {gpio_macro_prefix}_{suffix} */"
        )
    lines.append("    };")
    lines.append("};")

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# west.yml fragment emission (libraries -> Zephyr-module name-allowlist)
# ---------------------------------------------------------------------
#
# Closes the second v0.4 gap docs/board-config.md flagged: customers
# whose board.yaml declares `libraries: [lvgl, mbedtls]` should not
# also have to hand-add those modules to their app's west.yml
# `name-allowlist:`.  The emitter produces a paste-ready fragment
# they import via a self-referencing `import:` block.


# Library name -> Zephyr module name the workspace's west.yml must
# import.  Mirrors zephyr/modules.git's published modules; LittleFS
# ships as `fs/littlefs` while the rest match their library names 1:1.
_LIBRARY_WEST_MODULES: dict[str, str] = {
    "lvgl":          "lvgl",
    "mbedtls":       "mbedtls",
    "cmsis_dsp":     "cmsis-dsp",
    "littlefs":      "fs/littlefs",
    # The four header-only C++ libraries (etl / fmt / nlohmann_json /
    # doctest) are not Zephyr modules today -- they land in v0.4 via
    # the per-library profile + include-path hook in the loader, not
    # via west.yml.  Listing them here would emit an entry that
    # `west update` rejects.
}


# OTA provider -> Zephyr module name the workspace's west.yml must
# import.  Hawkbit and MCUmgr ship in Zephyr upstream so no entry --
# only out-of-tree clients need a west.yml line.  See ADR 0009.
_OTA_PROVIDER_WEST_MODULES: dict[str, str] = {
    "mender":  "mender-mcu-client",
    # hawkbit -- in Zephyr upstream
    # mcumgr  -- in Zephyr upstream
}


def _load_curated_library_manifest(lib: str) -> dict[str, Any] | None:
    """Load a top-level ADR 0018 library manifest if one exists."""
    path = METADATA_ROOT / "libraries" / f"{lib}.yaml"
    if not path.is_file():
        return None
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    return doc if isinstance(doc, dict) else None


def _emit_west_libraries(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_libraries: list[str] | None = None,
    v2_project_libraries: list[str] | None = None,
) -> str:
    """Emit a west.yml fragment that the customer's manifest can
    import to pin the Zephyr modules board.yaml's `libraries:` array
    requires.  Idempotent: emitting an empty `libraries:` array gives
    an empty (but well-formed) name-allowlist.

    v1 path (`v2_libraries is None`): reads project-level `libraries:`.
    v2 path: callers compute the union across the Zephyr-runtime cores
    (or pick one when `--core <id>` is supplied) and pass it in via
    `v2_libraries`.  `v2_project_libraries` carries the top-level ADR 0018
    curated library manifests; these may either import a Zephyr-owned module
    by name or emit a standalone west project pin from the manifest's
    `integration.zephyr.west` block.
    """
    del sku_preset, board_preset  # unused -- libraries are SoM-agnostic
    if v2_libraries is not None:
        libs = list(v2_libraries)
    else:
        libs = project.get("libraries") or []
    project_libs = list(v2_project_libraries
                        if v2_project_libraries is not None
                        else [])
    modules: list[tuple[str, str]] = []   # (library, Zephyr-owned west module)
    west_projects: list[tuple[str, dict[str, Any]]] = []
    unsupported: list[str] = []
    seen_modules: set[str] = set()
    seen_projects: set[str] = set()

    def add_module(lib: str, mod: str) -> None:
        if mod not in seen_modules:
            modules.append((lib, mod))
            seen_modules.add(mod)

    def add_west_project(lib: str, west: dict[str, Any]) -> None:
        name = str(west.get("name") or "")
        if name and name not in seen_projects:
            west_projects.append((lib, west))
            seen_projects.add(name)

    for lib in libs:
        mod = _LIBRARY_WEST_MODULES.get(lib)
        if mod is None:
            unsupported.append(lib)
        else:
            add_module(lib, mod)

    for lib in project_libs:
        manifest = _load_curated_library_manifest(lib)
        zephyr = ((manifest or {}).get("integration") or {}).get("zephyr") or {}
        if not zephyr:
            continue
        west = zephyr.get("west")
        if isinstance(west, dict):
            add_west_project(lib, west)
            continue
        mod = zephyr.get("module")
        if isinstance(mod, str) and mod:
            add_module(lib, mod)
        else:
            unsupported.append(lib)

    # OTA provider-driven dispatch (ADR 0009 follow-up): out-of-tree
    # Zephyr OTA clients need their own west.yml entry.  Mender-MCU-client
    # is the only one today; hawkbit and mcumgr ship in Zephyr upstream.
    ota = project.get("ota") or {}
    if isinstance(ota, dict):
        ota_provider = (ota.get("provider") or "").lower()
        ota_mod = _OTA_PROVIDER_WEST_MODULES.get(ota_provider)
        if ota_mod is not None:
            modules.append((f"ota:{ota_provider}", ota_mod))

    lines: list[str] = []
    lines.append("# SPDX-License-Identifier: Apache-2.0")
    lines.append("#")
    lines.append("# Auto-generated by scripts/alp_project.py -- "
                 "do not edit by hand.")
    lines.append("# Regenerate after changes to board.yaml's `libraries:` array.")
    lines.append("#")
    lines.append("# Import into your application's west.yml so `west update`")
    lines.append("# pulls only the Zephyr modules the libraries you enabled")
    lines.append("# actually need.  Drop alongside your west.yml and reference")
    lines.append("# from the `import:` field of the alp-sdk project entry.")
    lines.append("")
    lines.append("manifest:")
    lines.append("  projects:")
    lines.append("    - name: zephyr")
    lines.append("      import:")
    lines.append("        name-allowlist:")
    if modules:
        for lib, mod in modules:
            lines.append(f"          - {mod}        # board.yaml libraries: '{lib}'")
    else:
        lines.append("          # no selected Zephyr-owned modules -- nothing to allowlist.")
        lines.append("          []")

    if west_projects:
        lines.append("")
        lines.append("    # ADR 0018 libraries not imported by Zephyr's own west.yml.")
        for lib, west in west_projects:
            lines.append(f"    - name: {west['name']}")
            lines.append(f"      url: {west['url']}")
            lines.append(f"      revision: {west['revision']}")
            lines.append(f"      path: {west['path']}        # board.yaml libraries: '{lib}'")

    if unsupported:
        lines.append("")
        lines.append("# The following libraries have no Zephyr west project entry today")
        lines.append("# (header-only/profile libraries ride the loader's include path;")
        lines.append("# Yocto-only or in-tree Zephyr subsystems do not need a project pin):")
        for lib in unsupported:
            lines.append(f"#   - {lib}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# C header emission (build-time identifiers for <alp/hw_info.h>)
# ---------------------------------------------------------------------
#
# Produces the auto-generated `<alp_hw_info_build.h>` companion to
# `<alp/hw_info.h>` -- a small header that bakes the customer's
# board.yaml identifiers in as `ALP_HW_BUILD_*` string macros so the
# runtime check has something to compare the EEPROM read against:
#
#     #include "alp/hw_info.h"
#     #include "alp_hw_info_build.h"   // generated
#
#     alp_hw_info_t info;
#     alp_hw_info_read(&info);
#     alp_hw_info_assert_matches_build(&info,
#                                      ALP_HW_BUILD_SOM_SKU,
#                                      ALP_HW_BUILD_SOM_HW_REV);
#
# The CMakeLists.txt example pattern (mirroring the zephyr-conf
# emission) writes the header to `${CMAKE_BINARY_DIR}/generated/`
# and adds that path to the include search.


def _pick_primary_core_os(cores: dict[str, str]) -> tuple[str, str]:
    """Pick the "primary" core for the `ALP_HW_BUILD_OS` macro.

    `cores` maps core id -> os string ("zephyr" / "yocto" / "baremetal" /
    "off").  The selection rule:

      1. First M-class core (alphabetical by id), with os != off, if any.
      2. Else first A-class core (alphabetical by id), with os != off, if any.
      3. Else first non-off core (alphabetical by id), if any.
      4. Else returns ("", "off").

    Returns (core_id, os).
    """
    active = {cid: os_ for cid, os_ in cores.items() if os_ != "off"}
    if not active:
        return ("", "off")
    m_class = sorted(cid for cid in active if cid.startswith("m"))
    if m_class:
        cid = m_class[0]
        return (cid, active[cid])
    a_class = sorted(cid for cid in active if cid.startswith("a"))
    if a_class:
        cid = a_class[0]
        return (cid, active[cid])
    cid = sorted(active.keys())[0]
    return (cid, active[cid])


def _emit_hw_info_h(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_cores: dict[str, str] | None = None,
    v2_selected_core: str | None = None,
) -> str:
    """Emit <alp_hw_info_build.h> -- build-time identifier companion to
    <alp/hw_info.h>.

    v1 path (`v2_cores is None`): the `ALP_HW_BUILD_OS` macro comes from
    `project.os` (the v1 schema's single top-level OS).

    v2 path: derive `ALP_HW_BUILD_OS` from the cores: block.  If
    `v2_selected_core` is set (i.e. the caller passed `--core <id>`),
    use that core's OS.  Else pick a "primary" core via
    `_pick_primary_core_os`: first M-class core alphabetically, falling
    back to first A-class core, falling back to any non-off core.

    The v2 path also emits `ALP_HW_BUILD_CORES` (comma-separated list of
    every non-off core id) and one `ALP_HW_BUILD_HAS_<id>` macro per
    non-off core so consumers can `#ifdef` on the topology.
    """
    sku = project["som"]["sku"]
    som_hw_rev = (project["som"].get("hw_rev")
                  or sku_preset.get("default_hw_rev")
                  or "unknown")
    family = _sku_family(sku)

    board_block = project.get("board") or {}
    board_name = board_block.get("name") or ""
    board_hw_rev = ""
    if board_name and board_preset is not None:
        board_hw_rev = (board_block.get("hw_rev")
                          or board_preset.get("default_hw_rev")
                          or "")

    # Resolve the OS string.
    primary_core_id = ""
    primary_core_os = ""
    if v2_cores is not None:
        if v2_selected_core is not None and v2_selected_core in v2_cores:
            primary_core_id = v2_selected_core
            primary_core_os = v2_cores[v2_selected_core]
        else:
            primary_core_id, primary_core_os = _pick_primary_core_os(v2_cores)
        os_choice = primary_core_os
    else:
        os_choice = project.get("os") or ""

    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_project.py -- do not edit by hand.",
        " * Regenerate after changes to board.yaml.",
        " *",
        " * Build-time identifier companion to <alp/hw_info.h>.  Apps include",
        " * this header alongside <alp/hw_info.h> and pass the ALP_HW_BUILD_*",
        " * string macros to alp_hw_info_assert_matches_build() so the runtime",
        " * EEPROM read can be checked against what the firmware was built for.",
        " */",
        "",
        "#ifndef ALP_HW_INFO_BUILD_H",
        "#define ALP_HW_INFO_BUILD_H",
        "",
        f'#define ALP_HW_BUILD_SOM_SKU         "{sku}"',
        f'#define ALP_HW_BUILD_SOM_FAMILY      "{family}"',
        f'#define ALP_HW_BUILD_SOM_HW_REV      "{som_hw_rev}"',
    ]
    if board_name:
        lines.append(f'#define ALP_HW_BUILD_BOARD_NAME      "{board_name}"')
        if board_hw_rev:
            lines.append(f'#define ALP_HW_BUILD_BOARD_HW_REV    "{board_hw_rev}"')
    if os_choice:
        lines.append(f'#define ALP_HW_BUILD_OS              "{os_choice}"')
    if v2_cores is not None:
        # Per-core topology surface for `#ifdef ALP_HW_BUILD_HAS_<id>`
        # conditional compilation.  Primary-core selection rule:
        #   1. First M-class core (alphabetical by id), if any non-off.
        #   2. Else first A-class core (alphabetical by id), if any non-off.
        #   3. Else first non-off core (alphabetical by id).
        active = sorted(cid for cid, os_ in v2_cores.items() if os_ != "off")
        if active:
            lines.append("")
            lines.append(
                f'#define ALP_HW_BUILD_CORES           "{",".join(active)}"'
            )
            if primary_core_id:
                lines.append(
                    f'#define ALP_HW_BUILD_PRIMARY_CORE    "{primary_core_id}"'
                )
            lines.append("")
            lines.append("/* Per-core presence flags -- `#ifdef "
                         "ALP_HW_BUILD_HAS_<id>` to compile slice-")
            lines.append(" * specific code.  Each macro's value is the "
                         "slice's OS string, useful for")
            lines.append(" * `#if defined(...)`-style runtime selection. */")
            for cid in active:
                lines.append(
                    f'#define ALP_HW_BUILD_HAS_{cid.upper():<12} "{v2_cores[cid]}"'
                )
    lines += [
        "",
        "#endif /* ALP_HW_INFO_BUILD_H */",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------
# Carrier route / netlist emitters
# ---------------------------------------------------------------------
#
# `composed-route-table` stays as the original debug surface.  The
# route-row helper below is shared by the production `carrier-netlist`
# contract so the two views cannot drift on hw_rev pad-route overrides.


def _composed_route_rows(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> tuple[list[dict[str, Any]], str | None, str | None]:
    """Return composed route rows plus the selected hw_rev / variant.

    Rows cover every E1M pad named by the board and every SoM-only pad
    that has a dispatch route.  Board-defined rows preserve YAML order;
    SoM-only rows are sorted by E1M ID for deterministic output.
    """
    pad_routes = _resolve_pad_routes(sku_preset)

    # Apply the selected board revision's pad-route overrides on top of the
    # base (production-rev) pad_routes, so the composed table -- and thus
    # `--emit composed-route-table` -- differs by hw_rev.  The rev comes from
    # the board's `som.hw_rev`, falling back to the SoM's `default_hw_rev`.
    hw_rev = ((project.get("som") or {}).get("hw_rev")
              or sku_preset.get("default_hw_rev"))
    for ov in _hwrev_pad_route_overrides(project["som"]["sku"], hw_rev,
                                         metadata_root):
        pad_routes[ov["e1m"]] = ov

    # Resolve silicon variant order_code for the top-level summary field.
    variant = _resolve_silicon_variant(sku_preset, metadata_root)
    silicon_variant_str = variant["order_code"] if variant else None

    # Collect board-side entries, preserving the sub-category name.
    # Build a mapping: e1m_id -> (category, entry_dict).
    # When the same E1M pad appears multiple times (e.g. E1M_PWM1 maps to
    # both EVK_PWM_LED_BLUE and EVK_ARD_PWM1 in the EVK YAML) we emit one
    # row per board entry so no information is lost.
    board_entries: list[tuple[str, dict[str, Any]]] = []
    seen_from_board: set[str] = set()
    if board_preset is not None:
        e1m_routes = board_preset.get("e1m_routes") or {}
        for category, entries in e1m_routes.items():
            if not isinstance(entries, list):
                continue
            for entry in entries:
                if not isinstance(entry, dict):
                    continue
                e1m = entry.get("e1m")
                if not isinstance(e1m, str):
                    continue
                board_entries.append((category, entry))
                seen_from_board.add(e1m)

    # Also include SoM-only pads (in pad_routes but not in board).
    som_only_pads = sorted(set(pad_routes.keys()) - seen_from_board)

    routes: list[dict[str, Any]] = []

    # Board-defined entries first (preserves YAML order).
    for category, c_entry in board_entries:
        e1m = c_entry["e1m"]
        composed = _compose_route(e1m, c_entry, pad_routes)
        row: dict[str, Any] = {"e1m": e1m, "board_category": category}
        row["board_macro"] = composed.get("board_macro")
        row["board_role"] = composed.get("board_role")
        if "board_doc" in composed:
            row["board_doc"] = composed["board_doc"]
        # active_low is a board-side flag, not surfaced by _compose_route;
        # read it directly from the board entry.
        active_low = c_entry.get("active_low")
        if active_low is not None:
            row["active_low"] = bool(active_low)
        row["dispatch"] = composed.get("dispatch", "direct")
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    # SoM-only pads (not assigned a board role in this board YAML).
    for e1m in som_only_pads:
        composed = _compose_route(e1m, None, pad_routes)
        row = {
            "e1m": e1m,
            "board_category": None,
            "board_macro": None,
            "board_role": None,
            "dispatch": composed.get("dispatch", "direct"),
        }
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    return routes, hw_rev, silicon_variant_str


def _emit_composed_route_table(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit a JSON summary of the fully-composed pad route table for
    the current (board x SoM) pair.

    The table is derived by calling _resolve_pad_routes() (SoM side) and
    _compose_route() (join with board side) for every E1M pad that
    appears in either the board's e1m_routes: block or the SoM's
    pad_routes: block.

    Pads that only appear in the SoM's pad_routes: block (i.e. no
    board-side role assigned) are included with null board_* fields
    so the table is complete for the SoM-standalone scenario.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "routes": routes,
    }
    return json.dumps(result, indent=2) + "\n"


def _manifest_path(kind: str, item_id: str, metadata_root: Path) -> Path:
    return metadata_root / kind / f"{item_id}.yaml"


def _load_optional_manifest(kind: str, item_id: str,
                            metadata_root: Path) -> dict[str, Any] | None:
    path = _manifest_path(kind, item_id, metadata_root)
    if not path.is_file():
        return None
    return _load_yaml(path)


def _passive_rows(passives: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for passive in passives or []:
        if not isinstance(passive, dict):
            continue
        row: dict[str, Any] = {
            "role": passive.get("role"),
            "value": passive.get("value"),
            "net": passive.get("net"),
            "refdes_prefix": passive.get("refdes_prefix"),
        }
        rows.append({k: v for k, v in row.items() if v is not None})
    return rows


def _chip_bom_row(item_id: str, manifest: dict[str, Any],
                  manifest_relpath: str) -> dict[str, Any]:
    physical = manifest.get("physical") or {}
    caveats: list[str] = []
    if not physical:
        caveats.append("missing_physical")
    elif physical.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if len(manifest.get("mpn_population") or []) > 1:
        caveats.append("mpn_population_candidates")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "chip",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "vendor": manifest.get("vendor"),
        "mpn_population": manifest.get("mpn_population") or [],
        "bus": manifest.get("bus"),
        "quantity": None,
        "physical": {
            "refdes_prefix": physical.get("refdes_prefix"),
            "package": physical.get("package"),
            "footprint": physical.get("footprint"),
            "visibility": physical.get("visibility"),
            "provenance": physical.get("provenance"),
        },
        "passives": _passive_rows(physical.get("passives")),
    }
    row["physical"] = {k: v for k, v in row["physical"].items()
                       if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _block_bom_row(item_id: str, manifest: dict[str, Any],
                   manifest_relpath: str) -> dict[str, Any]:
    realizations = [
        r for r in manifest.get("realizations") or []
        if isinstance(r, dict)
    ]
    realization = realizations[0] if realizations else {}
    caveats: list[str] = []
    if not realizations:
        caveats.append("missing_realization")
    elif len(realizations) > 1:
        caveats.append("multiple_realizations")
    if realization.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if not realization.get("parts"):
        caveats.append("no_concrete_parts")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "block",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "quantity": None,
        "realization": {
            "id": realization.get("id"),
            "physical_form": realization.get("physical_form"),
            "visibility": realization.get("visibility"),
        },
        "parts": realization.get("parts") or [],
        "passives": _passive_rows(realization.get("passives")),
    }
    row["realization"] = {k: v for k, v in row["realization"].items()
                          if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _carrier_bom_rows(
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> list[dict[str, Any]]:
    """Build carrier BOM rows from board `populated: true`.

    `populated:` is a logical population map, not a line-item BOM with
    refdes or count, so rows deliberately leave `quantity` null unless a
    future metadata field makes it authoritative.
    """
    rows: list[dict[str, Any]] = []
    if board_preset is None:
        return rows

    populated = board_preset.get("populated") or {}
    for item_id in sorted(k for k, v in populated.items() if v is True):
        chip = _load_optional_manifest("chips", item_id, metadata_root)
        if chip is not None:
            rows.append(_chip_bom_row(
                item_id, chip, f"metadata/chips/{item_id}.yaml"))
            continue

        block = _load_optional_manifest("blocks", item_id, metadata_root)
        if block is not None:
            rows.append(_block_bom_row(
                item_id, block, f"metadata/blocks/{item_id}.yaml"))
            continue

        rows.append({
            "item_id": item_id,
            "kind": "unknown",
            "scope": "carrier",
            "source": None,
            "quantity": None,
            "caveats": ["missing_manifest"],
        })
    return rows


def _route_to_net(row: dict[str, Any]) -> dict[str, Any]:
    net_id = row.get("board_macro") or row["e1m"]
    endpoints: list[dict[str, Any]] = [
        {"kind": "e1m", "ref": row["e1m"]},
    ]
    if row.get("board_macro"):
        endpoints.append({"kind": "board-macro", "ref": row["board_macro"]})
    if row.get("dispatch") and row["dispatch"] != "direct":
        endpoint: dict[str, Any] = {
            "kind": "som-dispatch",
            "ref": row["dispatch"],
        }
        if "dispatch_pin" in row:
            endpoint["pin"] = row["dispatch_pin"]
        endpoints.append(endpoint)

    net: dict[str, Any] = {
        "net_id": net_id,
        "e1m": row["e1m"],
        "board_category": row.get("board_category"),
        "board_macro": row.get("board_macro"),
        "board_role": row.get("board_role"),
        "dispatch": row.get("dispatch", "direct"),
        "endpoints": endpoints,
    }
    for key in ("board_doc", "active_low", "dispatch_pin", "som_doc"):
        if key in row:
            net[key] = row[key]
    caveats = []
    if row.get("board_macro") is None:
        caveats.append("som_only_no_carrier_role")
    if caveats:
        net["caveats"] = caveats
    return net


def _emit_carrier_netlist(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit the Studio-facing carrier netlist + BOM handoff contract.

    This is intentionally not a KiCad, Gerber, or layout artifact.  It
    exposes only public carrier-facing facts derivable from board.yaml,
    board presets, chip/block manifests, and SoM pad dispatch metadata.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "schema_version": 1,
        "kind": "alp.carrier_netlist",
        "generated_by": "scripts/alp_project.py --emit carrier-netlist",
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "nets": [_route_to_net(row) for row in routes],
        "bom": {
            "carrier": _carrier_bom_rows(board_preset, metadata_root),
        },
        "caveats": [
            "carrier_handoff_not_pcb_layout",
            "no_kicad_or_gerber_output",
            "som_internals_excluded",
            "quantity_null_when_board_populated_has_no_count",
        ],
    }
    return json.dumps(result, indent=2) + "\n"

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Load + validate a board.yaml project config and emit the per-backend
native config it compiles down to.

Usage:

    # Emit a Zephyr Kconfig fragment from ./board.yaml to stdout:
    python3 scripts/alp_project.py

    # Same, explicit:
    python3 scripts/alp_project.py --input board.yaml --emit zephyr-conf

    # Plain-CMake -D args:
    python3 scripts/alp_project.py --emit cmake-args

    # Yocto local.conf snippet:
    python3 scripts/alp_project.py --emit yocto-conf

    # Write to a file (typical Zephyr usage: included by prj.conf):
    python3 scripts/alp_project.py --emit zephyr-conf \\
        --output build/generated/alp.conf

The loader resolves:
  - The SoM SKU preset under metadata/e1m_modules/<SKU>.yaml
  - The carrier preset under metadata/carriers/<name>/board.yaml
    (if `carrier` block present)
  - Per-block overrides in the user's board.yaml

Then emits the appropriate native config.  For Zephyr this is a
.conf file the build appends to prj.conf; for plain CMake a
sequence of `-D` args; for Yocto a local.conf snippet.

Errors are reported with a one-line summary + the underlying
schema / file path so failures are debuggable.

Dependencies (standard CPython 3.10+ stdlib + two well-established
pip packages):
  - PyYAML  (yaml parser)
  - jsonschema  (already used by validate_metadata.py)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: jsonschema is required.  Install via `pip install jsonschema`.")


REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT = REPO / "metadata"
SCHEMA = METADATA_ROOT / "schemas" / "board-config-v1.schema.json"
SDK_VERSION_FILE = METADATA_ROOT / "sdk_version.yaml"


# ---------------------------------------------------------------------
# SKU + silicon → Kconfig mapping
# ---------------------------------------------------------------------
#
# The mapping is small (5 families × a handful of SKUs each) so we
# bake it inline rather than reading another metadata file.  When a
# new SoM family lands, add the entry here + update the schema's
# `som.sku` pattern.

_SKU_FAMILY = re.compile(r"^E1M-(AEN|V2N|V2M|NX9)")


def _sku_family(sku: str) -> str:
    """Return the SoM family directory name for a SKU string."""
    m = _SKU_FAMILY.match(sku)
    if m is None:
        raise ValueError(f"unrecognised SoM SKU pattern: {sku}")
    return {"AEN": "aen", "V2N": "v2n", "V2M": "v2n-m1", "NX9": "imx93"}[m.group(1)]


# Map silicon refs to the Zephyr Kconfig that selects them.
_SILICON_TO_KCONFIG: dict[str, str] = {
    "alif:ensemble:e3": "ALP_SOC_ALIF_ENSEMBLE_E3",
    "alif:ensemble:e4": "ALP_SOC_ALIF_ENSEMBLE_E4",
    "alif:ensemble:e5": "ALP_SOC_ALIF_ENSEMBLE_E5",
    "alif:ensemble:e6": "ALP_SOC_ALIF_ENSEMBLE_E6",
    "alif:ensemble:e7": "ALP_SOC_ALIF_ENSEMBLE_E7",
    "alif:ensemble:e8": "ALP_SOC_ALIF_ENSEMBLE_E8",
    "renesas:rzv2n:n44": "ALP_SOC_RENESAS_RZV2N_N44",
    "nxp:imx9:imx93": "ALP_SOC_NXP_IMX9_IMX93",
}


# ---------------------------------------------------------------------
# Load + validate
# ---------------------------------------------------------------------


def _load_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        sys.exit(f"alp_project: file not found: {path}")
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        sys.exit(f"alp_project: failed to parse {path}: {e}")
    if not isinstance(data, dict):
        sys.exit(f"alp_project: {path} did not parse to a top-level mapping")
    return data


def _validate(project: dict[str, Any]) -> None:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(project), key=lambda e: list(e.path))
    if errors:
        for e in errors:
            loc = "/".join(str(p) for p in e.path) or "<root>"
            print(f"alp_project: schema violation at {loc}: {e.message}", file=sys.stderr)
        sys.exit(1)


def _resolve_sku(sku: str, metadata_root: Path) -> dict[str, Any]:
    # Per-SKU preset lives at metadata/e1m_modules/<SKU>.yaml.
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        sys.exit(
            f"alp_project: no preset for SKU {sku} at {preset_path.relative_to(REPO)} "
            f"(remaining SKUs land alongside the user-supplied HW config writeup)"
        )
    return _load_yaml(preset_path)


def _resolve_carrier(name: str, metadata_root: Path) -> dict[str, Any]:
    # Per-carrier preset lives at metadata/carriers/<name>/board.yaml.
    preset_path = metadata_root / "carriers" / name / "board.yaml"
    if not preset_path.is_file():
        # Custom carrier -- no preset; the board.yaml's `carrier.populated`
        # block is authoritative.  Return an empty preset.
        return {"name": name, "populated": {}}
    return _load_yaml(preset_path)


# ---------------------------------------------------------------------
# Hardware-revision compatibility (board.yaml hw_rev vs SDK version)
# ---------------------------------------------------------------------


def _read_sdk_version(metadata_root: Path) -> str | None:
    """Read metadata/sdk_version.yaml.  Returns the version string,
    or None if the file is absent (allows running the loader on
    older checkouts that pre-date the hw_rev work)."""
    p = metadata_root / "sdk_version.yaml"
    if not p.is_file():
        return None
    data = yaml.safe_load(p.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        return None
    v = data.get("version")
    return str(v) if v else None


def _parse_semver(s: str) -> tuple[int, ...]:
    """Lenient '0.3.0' -> (0, 3, 0) split.  Non-numeric trailing
    pre-release suffixes like '0.3.0-rc1' are dropped."""
    head = s.split("-", 1)[0].split("+", 1)[0]
    parts = []
    for chunk in head.split("."):
        try:
            parts.append(int(chunk))
        except ValueError:
            parts.append(0)
    return tuple(parts)


def _load_family_hw_revisions(
    family: str, metadata_root: Path,
) -> dict[str, Any] | None:
    """metadata/e1m_modules/<family>/hw-revisions.yaml -- returns
    the file's `hw_revisions` block (or None if the file is missing)."""
    p = metadata_root / "e1m_modules" / family / "hw-revisions.yaml"
    if not p.is_file():
        return None
    doc = yaml.safe_load(p.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        return None
    revs = doc.get("hw_revisions")
    return revs if isinstance(revs, dict) else None


def _resolve_hw_rev(
    declared: str | None,
    preset_default: str | None,
    label: str,
) -> str | None:
    """Pick the effective hw_rev -- the explicit board.yaml field
    wins; otherwise fall back to the preset's default_hw_rev.  Returns
    None when both are absent (in which case the check is skipped
    with a comment, not a hard error)."""
    rev = declared or preset_default
    if rev is None:
        print(f"alp_project: no hw_rev declared for {label} -- check skipped",
              file=sys.stderr)
    return rev


def _check_hw_rev_vs_sdk(
    rev: str,
    hw_revisions: dict[str, Any],
    sdk_version: str,
    label: str,
) -> None:
    """Fail-fast: validate that the chosen hw_rev's
    [min_sdk_version, max_sdk_version] window covers the SDK
    version.  Exits with the loader's standard error path on
    mismatch."""
    if rev not in hw_revisions:
        sys.exit(
            f"alp_project: {label} hw_rev '{rev}' is not declared in "
            f"hw_revisions; known: {sorted(hw_revisions.keys())}")
    entry = hw_revisions[rev] or {}
    min_s = entry.get("min_sdk_version")
    max_s = entry.get("max_sdk_version")
    sdk_t = _parse_semver(sdk_version)
    if min_s is not None and sdk_t < _parse_semver(str(min_s)):
        sys.exit(
            f"alp_project: SDK {sdk_version} is older than {label} hw_rev "
            f"'{rev}' minimum {min_s}.  Upgrade the SDK or pick an "
            f"older hw_rev.")
    # max_sdk_version may be null/None -- treated as "unlimited".
    if max_s is not None and str(max_s).strip() not in ("", "~"):
        if sdk_t > _parse_semver(str(max_s)):
            sys.exit(
                f"alp_project: SDK {sdk_version} is newer than {label} "
                f"hw_rev '{rev}' maximum {max_s}.  Downgrade the SDK or "
                f"pick a newer hw_rev.")


# ---------------------------------------------------------------------
# Override merge
# ---------------------------------------------------------------------


def _merge_populated(base: dict[str, bool], overrides: dict[str, bool] | None) -> dict[str, bool]:
    out = dict(base or {})
    for k, v in (overrides or {}).items():
        out[k] = bool(v)
    return out


# ---------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------


def _emit_zephyr(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_project.py -- do not edit by hand.")
    lines.append("# Regenerate after changes to board.yaml.")
    lines.append("")

    # 0. ALP SDK + Zephyr baseline.  Always required when emitting
    # zephyr-conf so the customer's prj.conf can be a single rsource
    # of the generated fragment (board.yaml is the single source of
    # truth -- prj.conf no longer carries CONFIG_ALP_SDK / CONFIG_LOG
    # / CONFIG_PRINTK / TLS by hand).
    diagnostics = project.get("diagnostics") or {}
    lines.append("# ALP SDK + Zephyr baseline")
    lines.append("CONFIG_ALP_SDK=y")
    lines.append("CONFIG_LOG=y")
    lines.append("CONFIG_PRINTK=y")
    if diagnostics.get("last_error", True):
        # Thread-local alp_last_error() slot requires TLS.
        lines.append("CONFIG_THREAD_LOCAL_STORAGE=y")
    log_level = diagnostics.get("log_level")
    if log_level is not None:
        # Zephyr's CONFIG_LOG_DEFAULT_LEVEL is 0..4 (off, err, wrn,
        # inf, dbg).  Map our `trace` to dbg since Zephyr has no
        # finer-grained slot.
        log_level_kc = {"error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4}
        if log_level in log_level_kc:
            lines.append(f"CONFIG_LOG_DEFAULT_LEVEL={log_level_kc[log_level]}")
    lines.append("")

    # 1. Silicon selection
    silicon = sku_preset.get("silicon")
    kconfig = _SILICON_TO_KCONFIG.get(silicon, None) if silicon else None
    if kconfig is not None:
        lines.append(f"# SoM silicon ({silicon} via {project['som']['sku']})")
        lines.append(f"CONFIG_{kconfig}=y")
        lines.append("")
    elif silicon is not None:
        lines.append(f"# silicon '{silicon}' has no Kconfig mapping yet -- update _SILICON_TO_KCONFIG")
        lines.append("")

    # 2. Carrier-populated chip drivers
    final: dict[str, bool] = {}
    if carrier_preset is not None:
        base = carrier_preset.get("populated", {}) or {}
        user_overrides = (project.get("carrier", {}) or {}).get("populated", {}) or {}
        final = _merge_populated(base, user_overrides)
        if final:
            lines.append(f"# Carrier chip drivers ({carrier_preset.get('name', '?')})")
            for chip, on in sorted(final.items()):
                lines.append(f"CONFIG_ALP_SDK_CHIP_{chip.upper()}={'y' if on else 'n'}")
            lines.append("")

    # 2b. Zephyr subsystems required by the enabled chip drivers.
    # Each chip driver's Kconfig has a `depends on <SUBSYS>` line;
    # Kconfig won't auto-select GPIO / I2C / SPI / PWM when the
    # chip is enabled, so we map populated chips to the subsystems
    # they need and emit the matching CONFIG_<SUBSYS>=y.
    subsystems: set[str] = set()
    for chip, on in final.items():
        if not on:
            continue
        for s in _CHIP_SUBSYSTEMS.get(chip, ()):
            subsystems.add(s)

    # 2c. Subsystems the app declares directly via the `peripherals:`
    # block.  Covers apps that hit <alp/<class>.h> without going
    # through any chip driver -- the per-peripheral examples are the
    # canonical case (adc-voltmeter / i2c-scanner / etc.).
    for periph in project.get("peripherals") or []:
        kc = _PERIPHERAL_KCONFIG.get(periph)
        if kc:
            subsystems.add(kc)
        else:
            lines.append(f"# TODO: peripheral '{periph}' has no Kconfig mapping yet")
    if subsystems:
        lines.append("# Zephyr subsystems pulled in by enabled chip drivers + peripherals")
        for s in sorted(subsystems):
            lines.append(f"CONFIG_{s}=y")
        lines.append("")

    # 3. Inference backend
    inference = project.get("inference") or {}
    backend = inference.get("backend") or sku_preset.get("inference", {}).get("preferred_backend") \
        or "auto"
    lines.append(f"# Inference backend ({backend})")
    if backend in ("cpu", "ethos_u"):
        lines.append("CONFIG_ALP_SDK_INFERENCE_TFLM=y")
        if backend == "ethos_u":
            lines.append("CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y")
            if silicon == "nxp:imx9:imx93":
                lines.append("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y")
    elif backend == "drpai":
        lines.append("CONFIG_ALP_SDK_INFERENCE_DRPAI=y")
    elif backend == "deepx_dx":
        lines.append("# Note: DEEPX backend is Yocto-only today; the Zephyr build")
        lines.append("# falls back to NOSUPPORT.  Use --emit cmake-args for V2N-M1.")
    elif backend == "auto":
        lines.append("# auto -- the dispatcher's resolve_auto() picks based on compiled backends")
    lines.append("")

    # 4. IoT features
    iot = project.get("iot") or {}
    if iot:
        lines.append("# IoT features")
        if iot.get("wifi"):
            lines.append("CONFIG_ALP_SDK_IOT_WIFI=y")
        if iot.get("mqtt"):
            lines.append("CONFIG_ALP_SDK_IOT_MQTT=y")
        if iot.get("ble"):
            lines.append("CONFIG_ALP_SDK_BLE=y")
        if iot.get("tls"):
            lines.append("CONFIG_ALP_SDK_SECURITY=y")
        lines.append("")

    # 5. Optional libraries
    libs = project.get("libraries") or []
    if libs:
        lines.append("# Optional libraries (apps use the upstream native API; the SDK")
        lines.append("# adds the matching compile-time profile from metadata/library-profiles/)")
        for lib in libs:
            if lib in _LIBRARY_KCONFIG:
                for kc in _LIBRARY_KCONFIG[lib]:
                    lines.append(f"{kc}")
            else:
                lines.append(f"# TODO: wire library '{lib}' once the v0.4 enable lands")
        lines.append("")

    return "\n".join(lines) + "\n"


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


# Peripheral name (from board.yaml's `peripherals:` array) ->
# Zephyr Kconfig symbol the loader sets.  Mirrors the per-class
# wrapper enables in zephyr/Kconfig (ALP_SDK_PERIPH_*) which all
# `default y if <SUBSYS>` -- so enabling the Zephyr subsystem
# here lights up both the subsystem driver AND the alp wrapper.
_PERIPHERAL_KCONFIG: dict[str, str] = {
    "adc":      "ADC",
    "can":      "CAN",
    "counter":  "COUNTER",
    "gpio":     "GPIO",
    "i2c":      "I2C",
    "i2s":      "I2S",
    "pwm":      "PWM",
    "rtc":      "RTC",
    "sensor":   "SENSOR",    # underlying class for the qenc helper
    "spi":      "SPI",
    "uart":     "SERIAL",    # Zephyr's UART class symbol is SERIAL
    "watchdog": "WATCHDOG",
}


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
    "lvgl":          ("CONFIG_LVGL=y",),
    "mbedtls":       ("CONFIG_MBEDTLS=y", "CONFIG_MBEDTLS_BUILTIN=y"),
    "cmsis_dsp":     ("CONFIG_CMSIS_DSP=y",),
    "littlefs":      ("CONFIG_FILE_SYSTEM_LITTLEFS=y", "CONFIG_FILE_SYSTEM=y"),

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
                       "# u8g2: include path + u8g2_config.h via v0.4 loader hook",),
    "gfx_compat":     ("CONFIG_ALP_GFX_COMPAT_SW=y",
                       "# gfx_compat: maintainer-shipped thin shim; no external dep",),

    # §D.lib.industrial
    "madgwick_ahrs":  ("CONFIG_ALP_MADGWICK_LIBM=y",),
    "pid":            ("CONFIG_ALP_PID_INT_MATH=y",),
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
    "catch2":         ("CONFIG_ALP_CATCH2_SW=y",),
}


def _emit_cmake(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    args: list[str] = []
    # SoM family → ALP_SOM
    family = _sku_family(project["som"]["sku"])
    som_family_alias = {"aen": "aen", "v2n": "v2n", "v2n-m1": "v2n", "imx93": "imx93"}[family]
    args.append(f"-DALP_SOM={som_family_alias}")
    args.append(f"-DALP_OS={project['os']}")

    # V2N-M1 → enable DEEPX backend in the Yocto build
    if family == "v2n-m1":
        args.append("-DALP_SDK_USE_DEEPX_DXM1=ON")

    return " \\\n    ".join(args) + "\n"


# ---------------------------------------------------------------------
# DTS overlay emission (v0.3: i2c / spi / uart / pwm / gpio aliases)
# ---------------------------------------------------------------------
#
# Per the project memory note "pending exact hardware configurations
# -- mark unknowns TBD, never invent values", the loader translates
# the macros in include/alp/boards/<carrier>.h verbatim; it does not
# invent gpio bank numbers or per-pad GPIO_ACTIVE_* flags.  The
# emitted .overlay declares the carrier's bus aliases and a stub
# alp,pin-array with one entry per EVK_PIN_* macro, each annotated
# with a comment naming the macro and the E1M_GPIO_IO<N> it
# resolves to.  Customers fill the gpio bank / index columns with
# their SoM's actual DT controller phandles once the upstream board
# files land in alplabai/alp-zephyr-modules.
#
# Bus phandle naming convention matches the manually-written EVK
# overlays at tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay:
# &i2c<N>, &spi<N>, &uart<N>, &pwm<N>.  Per-SoC vendor DT may use
# alternate names (e.g. &lpi2c0 on some Alif boards); the customer
# fixes the phandle if their board file diverges -- the loader's
# job is to surface every alias the carrier wants, not to second-
# guess vendor DT naming.

# Match `#define <NAME> E1M_<CLASS><N>` (with optional trailing
# token).  Class is one of the bus / pwm / gpio names we care about
# at v0.3 scope.
_DEFINE_E1M_RE = re.compile(
    r"^\s*#\s*define\s+(\w+)\s+E1M_(I2C|SPI|UART|PWM|GPIO_IO)(\d+)\b",
    re.MULTILINE,
)

# Bus-alias buckets the loader emits.  Each entry maps the e1m_pinout
# class name -> (alias prefix, Zephyr DT phandle prefix).
_BUS_BUCKETS: tuple[tuple[str, str, str], ...] = (
    ("I2C",  "alp-i2c",  "i2c"),
    ("SPI",  "alp-spi",  "spi"),
    ("UART", "alp-uart", "uart"),
    ("PWM",  "alp-pwm",  "pwm"),
)


def _strip_c_comments(text: str) -> str:
    """Strip /* ... */ and // ... comments from C source text."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _collapse_line_continuations(text: str) -> str:
    """Join `\\<newline>` continuations into single logical lines so a
    multi-line `#define NAME \\\n    VALUE` shows up as one line."""
    return re.sub(r"\\\s*\n\s*", " ", text)


def _carrier_header_path(carrier_name: str, repo_root: Path) -> Path:
    """Resolve include/alp/boards/alp_<carrier>.h for a carrier name.

    Example: 'E1M-EVK' -> include/alp/boards/alp_e1m_evk.h.
    """
    fname = "alp_" + carrier_name.lower().replace("-", "_") + ".h"
    return repo_root / "include" / "alp" / "boards" / fname


def _parse_carrier_macros(
    header_path: Path,
) -> dict[str, list[tuple[str, int]]]:
    """Return {class_name: [(macro_name, channel_index), ...]} for
    each E1M_<CLASS><N> reference in the carrier header."""
    raw = header_path.read_text(encoding="utf-8")
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


def _emit_dts_overlay(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    lines: list[str] = []
    lines.append("/*")
    lines.append(" * Auto-generated by scripts/alp_project.py "
                 "-- do not edit by hand.")
    lines.append(" * Regenerate after changes to board.yaml or "
                 "include/alp/boards/<carrier>.h.")
    lines.append(" *")
    lines.append(" * Per-pad GPIO bank/index values are TBD pending the upstream")
    lines.append(" * alp_<board>_<som>.dts board file (alplabai/alp-zephyr-modules).")
    lines.append(" * The alp,pin-array entries below preserve the EVK_PIN_* macro")
    lines.append(" * ordering so customers can fill the gpio columns in place")
    lines.append(" * without renumbering.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <zephyr/dt-bindings/gpio/gpio.h>")
    lines.append("")

    sku = project["som"]["sku"]
    carrier_name = (carrier_preset or {}).get("name", "")
    if not carrier_name:
        lines.append("// No carrier declared in board.yaml; nothing to emit.")
        return "\n".join(lines) + "\n"

    header_path = _carrier_header_path(carrier_name, REPO)
    if not header_path.is_file():
        sys.exit(f"alp_project: no carrier header at "
                 f"{header_path.relative_to(REPO)} for carrier '{carrier_name}' "
                 f"-- DTS overlay emission requires one.")

    macros = _parse_carrier_macros(header_path)

    lines.append(f"/ {{")
    lines.append(f"    /* Carrier: {carrier_name} (SoM SKU {sku}) */")
    lines.append(f"    /* Source: include/alp/boards/{header_path.name} */")
    lines.append("")

    # Bus aliases -- one per unique channel the carrier wires.
    lines.append("    aliases {")
    for class_name, alp_prefix, phandle_prefix in _BUS_BUCKETS:
        entries = sorted(set(idx for _macro, idx in macros.get(class_name, [])))
        if not entries:
            continue
        lines.append(f"        /* {class_name} */")
        for idx in entries:
            # Comment lists every carrier macro that references this channel.
            referencing = [m for (m, i) in macros[class_name] if i == idx]
            comment = ", ".join(referencing)
            lines.append(
                f"        {alp_prefix}{idx} = &{phandle_prefix}{idx};"
                f"  /* {comment} */"
            )
    lines.append("    };")
    lines.append("")

    # alp,pin-array -- one entry per EVK_PIN_* / EVK_ARD_DIO* macro
    # that resolves to E1M_GPIO_IO<N>.  Preserves macro ordering
    # so the customer can fill in `<&gpioX Y FLAGS>` columns in place.
    gpio_entries = macros.get("GPIO_IO", [])
    if gpio_entries:
        lines.append("    alp_pins: alp-pins {")
        lines.append('        compatible = "alp,pin-array";')
        lines.append("        /* The order MUST match the EVK_PIN_* / EVK_ARD_DIO* macro")
        lines.append("         * declarations in the carrier header.  Each <&gpioX Y FLAGS>")
        lines.append("         * triplet is TBD pending the upstream SoM board file.        */")
        lines.append("        gpios =")
        # Emit each gpio with a placeholder.  The trailing comma /
        # semicolon goes on the last entry.
        for i, (macro_name, idx) in enumerate(gpio_entries):
            terminator = ";" if i == len(gpio_entries) - 1 else ","
            # <&gpio0 IDX GPIO_ACTIVE_HIGH> placeholder; the comment
            # carries the authoritative E1M IO index from the header.
            lines.append(
                f"            <&gpio0 0 GPIO_ACTIVE_HIGH>{terminator}"
                f"  /* {macro_name} = E1M_GPIO_IO{idx} */"
            )
        lines.append("    };")
        lines.append("")

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


def _emit_west_libraries(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    """Emit a west.yml fragment that the customer's manifest can
    import to pin the Zephyr modules board.yaml's `libraries:` array
    requires.  Idempotent: emitting an empty `libraries:` array gives
    an empty (but well-formed) name-allowlist."""
    del sku_preset, carrier_preset  # unused -- libraries are SoM-agnostic
    libs = project.get("libraries") or []
    modules: list[tuple[str, str]] = []   # (library, west-module)
    unsupported: list[str] = []
    for lib in libs:
        mod = _LIBRARY_WEST_MODULES.get(lib)
        if mod is None:
            unsupported.append(lib)
        else:
            modules.append((lib, mod))

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
        lines.append("          # board.yaml `libraries:` is empty -- nothing to pin.")
        lines.append("          []")

    if unsupported:
        lines.append("")
        lines.append("# The following libraries are not Zephyr modules today")
        lines.append("# and don't need a west.yml entry (the loader wires their")
        lines.append("# include path + compile-time profile via metadata/library-")
        lines.append("# profiles/<lib>/ at CMake-configure time):")
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


def _emit_hw_info_h(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    sku = project["som"]["sku"]
    som_hw_rev = (project["som"].get("hw_rev")
                  or sku_preset.get("default_hw_rev")
                  or "unknown")
    family = _sku_family(sku)

    carrier_block = project.get("carrier") or {}
    carrier_name = carrier_block.get("name") or ""
    carrier_hw_rev = ""
    if carrier_name and carrier_preset is not None:
        carrier_hw_rev = (carrier_block.get("hw_rev")
                          or carrier_preset.get("default_hw_rev")
                          or "")

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
    if carrier_name:
        lines.append(f'#define ALP_HW_BUILD_CARRIER_NAME    "{carrier_name}"')
        if carrier_hw_rev:
            lines.append(f'#define ALP_HW_BUILD_CARRIER_HW_REV  "{carrier_hw_rev}"')
    if os_choice:
        lines.append(f'#define ALP_HW_BUILD_OS              "{os_choice}"')
    lines += [
        "",
        "#endif /* ALP_HW_INFO_BUILD_H */",
        "",
    ]
    return "\n".join(lines)


def _emit_yocto(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    carrier_preset: dict[str, Any] | None,
) -> str:
    family = _sku_family(project["som"]["sku"])
    machine = {
        "aen":     "e1m-aen-evk",       # TBD -- meta-alp will land this MACHINE
        "v2n":     "e1m-x-v2n",
        "v2n-m1":  "e1m-x-v2n-m1",
        "imx93":   "e1m-n93",
    }[family]
    lines = [
        f"# Auto-generated by scripts/alp_project.py.  Append to local.conf.",
        f"MACHINE = \"{machine}\"",
    ]
    libs = project.get("libraries") or []
    if libs:
        imageinstall = " ".join(f"lib-{lib.replace('_', '-')}" for lib in libs)
        lines.append(f"IMAGE_INSTALL:append = \" {imageinstall}\"")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile board.yaml -> per-backend native config.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--emit",
                        choices=["zephyr-conf", "cmake-args", "yocto-conf",
                                 "dts-overlay", "hw-info-h", "west-libraries"],
                        default="zephyr-conf",
                        help="Output format (default: zephyr-conf).")
    parser.add_argument("--output", type=Path, default=None,
                        help="Write to this path; default: stdout.")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT,
                        help="Override the metadata search root.")
    args = parser.parse_args()

    project = _load_yaml(args.input)
    _validate(project)

    sku_preset = _resolve_sku(project["som"]["sku"], args.metadata_root)
    carrier_preset = None
    if "carrier" in project and project["carrier"]:
        carrier_preset = _resolve_carrier(project["carrier"]["name"], args.metadata_root)

    # Hardware-rev / SDK-version compatibility.  Skipped silently
    # when sdk_version.yaml is absent so the loader stays usable
    # on pre-v0.3 checkouts.
    sdk_version = _read_sdk_version(args.metadata_root)
    if sdk_version is not None:
        family = _sku_family(project["som"]["sku"])
        family_revs = _load_family_hw_revisions(family, args.metadata_root)
        if family_revs:
            som_rev = _resolve_hw_rev(
                project["som"].get("hw_rev"),
                sku_preset.get("default_hw_rev"),
                f"SoM {project['som']['sku']}",
            )
            if som_rev is not None:
                _check_hw_rev_vs_sdk(
                    som_rev, family_revs, sdk_version,
                    f"SoM {project['som']['sku']}")
        if carrier_preset is not None:
            carrier_revs = carrier_preset.get("hw_revisions") or {}
            if isinstance(carrier_revs, dict) and carrier_revs:
                carrier_rev = _resolve_hw_rev(
                    (project.get("carrier") or {}).get("hw_rev"),
                    carrier_preset.get("default_hw_rev"),
                    f"carrier {carrier_preset.get('name', '?')}",
                )
                if carrier_rev is not None:
                    _check_hw_rev_vs_sdk(
                        carrier_rev, carrier_revs, sdk_version,
                        f"carrier {carrier_preset.get('name', '?')}")

    if args.emit == "zephyr-conf":
        out = _emit_zephyr(project, sku_preset, carrier_preset)
    elif args.emit == "cmake-args":
        out = _emit_cmake(project, sku_preset, carrier_preset)
    elif args.emit == "yocto-conf":
        out = _emit_yocto(project, sku_preset, carrier_preset)
    elif args.emit == "dts-overlay":
        out = _emit_dts_overlay(project, sku_preset, carrier_preset)
    elif args.emit == "hw-info-h":
        out = _emit_hw_info_h(project, sku_preset, carrier_preset)
    elif args.emit == "west-libraries":
        out = _emit_west_libraries(project, sku_preset, carrier_preset)
    else:
        sys.exit(f"alp_project: unknown emit format {args.emit}")

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(out, encoding="utf-8")
        print(f"alp_project: wrote {args.output.relative_to(Path.cwd()) if args.output.is_relative_to(Path.cwd()) else args.output} ({len(out)} bytes)", file=sys.stderr)
    else:
        sys.stdout.write(out)

    return 0


if __name__ == "__main__":
    sys.exit(main())

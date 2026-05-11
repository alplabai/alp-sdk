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
  - The SoM SKU preset under metadata/e1m_modules/<SKU>/som.yaml
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
    # Per-SKU preset lives at metadata/e1m_modules/<SKU>/som.yaml.
    preset_path = metadata_root / "e1m_modules" / sku / "som.yaml"
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
    if subsystems:
        lines.append("# Zephyr subsystems pulled in by the enabled chip drivers")
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
    parser.add_argument("--emit", choices=["zephyr-conf", "cmake-args", "yocto-conf"],
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

    if args.emit == "zephyr-conf":
        out = _emit_zephyr(project, sku_preset, carrier_preset)
    elif args.emit == "cmake-args":
        out = _emit_cmake(project, sku_preset, carrier_preset)
    elif args.emit == "yocto-conf":
        out = _emit_yocto(project, sku_preset, carrier_preset)
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

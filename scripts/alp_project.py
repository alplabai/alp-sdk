#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Load + validate an alp.yaml project config and emit the per-backend
native config it compiles down to.

Usage:

    # Emit a Zephyr Kconfig fragment from ./alp.yaml to stdout:
    python3 scripts/alp_project.py

    # Same, explicit:
    python3 scripts/alp_project.py --input alp.yaml --emit zephyr-conf

    # Plain-CMake -D args:
    python3 scripts/alp_project.py --emit cmake-args

    # Yocto local.conf snippet:
    python3 scripts/alp_project.py --emit yocto-conf

    # Write to a file (typical Zephyr usage: included by prj.conf):
    python3 scripts/alp_project.py --emit zephyr-conf \\
        --output build/generated/alp.conf

The loader resolves:
  - The SoM SKU preset under metadata/e1m_modules/<family>/sku-<sku>.yaml
  - The carrier preset under metadata/carriers/<name>.yaml (if `carrier`
    block present)
  - Per-block overrides in the user's alp.yaml

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
SCHEMA = METADATA_ROOT / "schemas" / "alp-project-v1.schema.json"


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
    family = _sku_family(sku)
    preset_path = metadata_root / "e1m_modules" / family / f"sku-{sku.lower().replace('e1m-', '')}.yaml"
    if not preset_path.is_file():
        sys.exit(
            f"alp_project: no preset for SKU {sku} at {preset_path.relative_to(REPO)} "
            f"(remaining SKUs land alongside the user-supplied HW config writeup)"
        )
    return _load_yaml(preset_path)


def _resolve_carrier(name: str, metadata_root: Path) -> dict[str, Any]:
    preset_path = metadata_root / "carriers" / f"{name.lower()}.yaml"
    if not preset_path.is_file():
        # Custom carrier -- no preset; the alp.yaml's `carrier.populated`
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
    lines.append("# Regenerate after changes to alp.yaml.")
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
    if carrier_preset is not None:
        base = carrier_preset.get("populated", {}) or {}
        user_overrides = (project.get("carrier", {}) or {}).get("populated", {}) or {}
        final = _merge_populated(base, user_overrides)
        if final:
            lines.append(f"# Carrier chip drivers ({carrier_preset.get('name', '?')})")
            for chip, on in sorted(final.items()):
                lines.append(f"CONFIG_ALP_SDK_CHIP_{chip.upper()}={'y' if on else 'n'}")
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
        lines.append("# Optional libraries")
        for lib in libs:
            if lib == "lwrb":
                lines.append("CONFIG_ALP_SDK_USE_LWRB=y")
            elif lib == "nanopb":
                lines.append("CONFIG_ALP_SDK_USE_NANOPB=y")
            else:
                lines.append(f"# TODO: wire CONFIG_ALP_USE_{lib.upper()} once the v0.4 enable lands")
        lines.append("")

    return "\n".join(lines) + "\n"


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
    parser = argparse.ArgumentParser(description="Compile alp.yaml -> per-backend native config.")
    parser.add_argument("--input", type=Path, default=Path("alp.yaml"),
                        help="Path to the project's alp.yaml (default: ./alp.yaml).")
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

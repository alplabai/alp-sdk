#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Customer-side validator for `board.yaml`.

Runs the checks an alp-sdk app author cares about *before* the
build kicks off:

  1. The file is valid YAML.
  2. It conforms to metadata/schemas/board.schema.json.
  3. The referenced SoM SKU has a preset at
     `metadata/e1m_modules/<SKU>.yaml`.
  4. When `preset:` is used, the shared carrier definition at
     `metadata/carriers/<preset>.yaml` exists.

Customer usage (from the app root):

    python3 $ALP_SDK/scripts/validate_board_yaml.py
    python3 $ALP_SDK/scripts/validate_board_yaml.py --input board.yaml

CI usage (smoke-checks every shipped example):

    python3 scripts/validate_board_yaml.py \\
        --input metadata/templates/board.yaml

Exit codes:
  0  clean
  1  YAML parse or schema violation
  2  missing SoM SKU preset or missing carrier preset referenced
     by `preset:`
  3  hardware-revision / SDK-version incompatibility (the chosen
     hw_rev's [min_sdk_version, max_sdk_version] window does not
     cover metadata/sdk_version.yaml)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("validate_board_yaml: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("validate_board_yaml: jsonschema is required.  Install via `pip install jsonschema`.")


REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT_DEFAULT = REPO / "metadata"
SCHEMA = METADATA_ROOT_DEFAULT / "schemas" / "board.schema.json"


def _load_yaml(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        print(f"FAIL {path}: file not found", file=sys.stderr)
        return None
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        print(f"FAIL {path}: invalid YAML ({e})", file=sys.stderr)
        return None
    if not isinstance(data, dict):
        print(f"FAIL {path}: top-level value is not a mapping", file=sys.stderr)
        return None
    return data


def _check_schema(project: dict[str, Any], path: Path) -> bool:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(project), key=lambda e: list(e.absolute_path))
    if not errors:
        print(f"OK   schema: {path}")
        return True
    print(f"FAIL schema: {path}", file=sys.stderr)
    for e in errors:
        loc = "/".join(str(p) for p in e.absolute_path) or "<root>"
        print(f"  · {loc}: {e.message}", file=sys.stderr)
    return False


def _check_som_preset(project: dict[str, Any], metadata_root: Path) -> int:
    """Return 0 on OK, 1 on warning (partial HW config), 2 on missing."""
    sku = project["som"]["sku"]
    preset = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset.is_file():
        print(f"FAIL som preset: no preset for {sku} at {preset.relative_to(REPO) if preset.is_relative_to(REPO) else preset}",
              file=sys.stderr)
        return 2

    try:
        body = yaml.safe_load(preset.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        print(f"FAIL som preset: {preset} failed to parse ({e})", file=sys.stderr)
        return 2

    partial = isinstance(body, dict) and (body.get("status") or {}).get("partial_hw_config", False)
    if partial:
        print(f"WARN som preset: {sku} preset is marked partial_hw_config=true "
              f"(memory / pinout fields still TBD)")
        return 1
    print(f"OK   som preset: {sku}")
    return 0


def _parse_semver(s: str) -> tuple[int, ...]:
    head = s.split("-", 1)[0].split("+", 1)[0]
    parts: list[int] = []
    for chunk in head.split("."):
        try:
            parts.append(int(chunk))
        except ValueError:
            parts.append(0)
    return tuple(parts)


def _check_hw_rev(
    label: str,
    rev: str,
    hw_revisions: dict[str, Any],
    sdk_version: str,
) -> int:
    """Return 0 on OK, 3 on incompatibility, 2 on unknown rev."""
    if rev not in hw_revisions:
        print(f"FAIL {label} hw_rev '{rev}' not in revisions "
              f"{sorted(hw_revisions.keys())}", file=sys.stderr)
        return 2
    entry = hw_revisions[rev] or {}
    sdk_t = _parse_semver(sdk_version)
    min_s = entry.get("min_sdk_version")
    if min_s is not None and sdk_t < _parse_semver(str(min_s)):
        print(f"FAIL {label} hw_rev '{rev}' requires SDK >= {min_s}; "
              f"current SDK is {sdk_version}", file=sys.stderr)
        return 3
    max_s = entry.get("max_sdk_version")
    if max_s is not None and str(max_s).strip() not in ("", "~"):
        if sdk_t > _parse_semver(str(max_s)):
            print(f"FAIL {label} hw_rev '{rev}' supports up to SDK "
                  f"{max_s}; current SDK is {sdk_version}",
                  file=sys.stderr)
            return 3
    print(f"OK   {label} hw_rev: {rev} (SDK {sdk_version} in range)")
    return 0


def _check_hw_compat(
    project: dict[str, Any], metadata_root: Path,
) -> int:
    """Cross-check the chosen SoM + carrier hw_rev against the SDK
    version recorded in metadata/sdk_version.yaml.  Returns the
    highest exit code from the two sub-checks (0 / 2 / 3)."""
    sdk_path = metadata_root / "sdk_version.yaml"
    if not sdk_path.is_file():
        print(f"WARN no metadata/sdk_version.yaml at {sdk_path} -- "
              f"hw_rev check skipped")
        return 0
    sdk_doc = yaml.safe_load(sdk_path.read_text(encoding="utf-8"))
    sdk_version = (sdk_doc or {}).get("version")
    if not sdk_version:
        print("WARN sdk_version.yaml has no `version` field -- "
              "hw_rev check skipped")
        return 0

    rv = 0

    # SoM side -- family-level hw_revisions table.
    sku = project["som"]["sku"]
    sku_preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if sku_preset_path.is_file():
        sku_doc = yaml.safe_load(sku_preset_path.read_text(encoding="utf-8")) or {}
        family = sku_doc.get("family")
        # Map the preset's `family:` value to the directory layout used
        # under metadata/e1m_modules/<family>/.
        family_dir = {
            "alif-ensemble": "aen",
            "renesas-rzv2n": "v2n",
            "renesas-rzv2n-deepx": "v2n-m1",
            "nxp-imx9":  "imx93",
        }.get(family, family)
        hw_revs_path = metadata_root / "e1m_modules" / str(family_dir) / "hw-revisions.yaml"
        if family_dir and hw_revs_path.is_file():
            hw_revs_doc = yaml.safe_load(hw_revs_path.read_text(encoding="utf-8")) or {}
            hw_revs = hw_revs_doc.get("hw_revisions") or {}
            rev = (project["som"].get("hw_rev")
                   or sku_doc.get("default_hw_rev"))
            if rev:
                rv = max(rv, _check_hw_rev(
                    f"som {sku}", rev, hw_revs, sdk_version))
            else:
                print(f"WARN som {sku}: no hw_rev declared, no "
                      f"default_hw_rev in preset -- check skipped")

    # Carrier side -- only the shared-preset path carries a
    # hw_revisions table; inline-carrier projects use top-level
    # `hw_rev:` directly without a min/max-SDK window.
    preset = project.get("preset")
    if preset:
        cp_path = metadata_root / "carriers" / f"{preset}.yaml"
        if cp_path.is_file():
            cp_doc = yaml.safe_load(cp_path.read_text(encoding="utf-8")) or {}
            hw_revs = cp_doc.get("hw_revisions") or {}
            if hw_revs:
                rev = (project.get("hw_rev")
                       or cp_doc.get("default_hw_rev"))
                if rev:
                    rv = max(rv, _check_hw_rev(
                        f"carrier {preset}", rev, hw_revs, sdk_version))
                else:
                    print(f"WARN carrier {preset}: no hw_rev declared, "
                          f"no default_hw_rev in preset -- check "
                          f"skipped")
    return rv


# Map board.yaml `peripherals:` enum values to the soc-spec
# peripherals[] keys that satisfy them.  Multiple keys = OR (any of
# them present at count >= 1 satisfies the requirement).  A value of
# None marks an umbrella class that the validator skips (Zephyr's
# SENSOR class covers too many drivers to map 1:1 to a single SoC
# peripheral count).
_PERIPHERAL_TO_SOC_KEYS: dict[str, tuple[str, ...] | None] = {
    "adc":      ("adc_12bit", "adc_24bit"),
    "can":      ("can_fd", "can"),
    "counter":  ("timer_32bit", "timer_lp"),
    "gpio":     ("gpio_18v", "gpio_18v_or_33v", "gpio"),
    "i2c":      ("i2c", "i2c_lp"),
    "i2s":      ("i2s", "i2s_lp"),
    "pwm":      ("timer_32bit", "pwm"),  # PWM rides the 32-bit timer block
    "rtc":      ("rtc",),
    "sensor":   None,                    # umbrella -- skip
    "spi":      ("spi", "spi_lp"),
    "uart":     ("uart", "uart_lp"),
    "watchdog": ("watchdog",),
    "flash":    None,                    # umbrella -- xSPI / OctalSPI / FlexSPI
    "emmc":     ("sdio_emmc", "sdio"),
    "dac":      ("dac",),
    "qenc":     ("qenc", "timer_qdec"),
}

# Peripherals the GD32G553 supervisor MCU brings into the E1M
# standard set on V2N modules.  When a SoM preset declares
# `on_module.supervisor_mcu: gd32g553`, the validator accepts any of
# these as satisfied even if the host SoC's peripherals[] count is 0
# -- the SDK's V2N backend (src/zephyr/v2n_supervisor.c) routes the
# portable alp_*_open calls through the GD32 bridge transparently.
# See docs/gd32-bridge-protocol.md for the wire surface.
_PERIPHERALS_PROVIDED_BY_GD32_BRIDGE: frozenset[str] = frozenset({
    "pwm", "adc", "dac", "qenc", "counter",
})


def _check_peripherals_vs_soc(
    project: dict[str, Any], metadata_root: Path,
) -> int:
    """Cross-check `peripherals:` against the SoM SKU's SoC spec.

    Returns 0 (clean), 2 (no SoC spec to check against -- warn),
    or 3 (a peripheral the app asks for isn't routed by the SoC)."""
    periphs = project.get("peripherals") or []
    if not periphs:
        return 0

    # Resolve SoC ref via the SKU preset.
    sku = project["som"]["sku"]
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        # Already flagged by _check_som_preset; nothing more to do.
        return 0
    preset = yaml.safe_load(preset_path.read_text(encoding="utf-8")) or {}
    silicon = preset.get("silicon")
    if not silicon:
        print(f"WARN peripherals: {sku} preset has no `silicon:` field -- check skipped")
        return 0

    # alif:ensemble:e7 -> metadata/socs/alif/ensemble/e7.json
    soc_path = metadata_root / "socs" / Path(*silicon.split(":"))
    soc_path = soc_path.with_suffix(".json")
    if not soc_path.is_file():
        print(f"WARN peripherals: no SoC spec at {soc_path.relative_to(REPO) if soc_path.is_relative_to(REPO) else soc_path} for ref '{silicon}' -- check skipped")
        return 2
    try:
        soc = json.loads(soc_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        print(f"WARN peripherals: failed to parse {soc_path} ({e}) -- check skipped")
        return 2
    soc_periphs = soc.get("peripherals") or {}

    # On-module supervisor MCU (if any) widens the satisfied set --
    # see _PERIPHERALS_PROVIDED_BY_GD32_BRIDGE.
    supervisor_mcu = (preset.get("on_module") or {}).get("supervisor_mcu", "")

    rv = 0
    for periph in periphs:
        soc_keys = _PERIPHERAL_TO_SOC_KEYS.get(periph)
        if soc_keys is None:
            # Umbrella class (e.g. sensor) -- skipped.
            continue
        # Pass if any candidate key is present with count >= 1.
        if any(int(soc_periphs.get(k, 0) or 0) > 0 for k in soc_keys):
            print(f"OK   peripheral '{periph}' satisfied by {silicon}")
        elif (supervisor_mcu == "gd32g553" and
              periph in _PERIPHERALS_PROVIDED_BY_GD32_BRIDGE):
            print(f"OK   peripheral '{periph}' satisfied by {silicon} via "
                  f"on-module supervisor MCU '{supervisor_mcu}' (bridge dispatch)")
        else:
            print(f"FAIL peripheral '{periph}' not routed by SoC {silicon} "
                  f"(no {' / '.join(soc_keys)} in metadata/socs/.../{soc_path.name})",
                  file=sys.stderr)
            rv = 3
    return rv


def _check_carrier_preset(project: dict[str, Any], metadata_root: Path) -> int:
    """Return 0 on OK, 2 on missing preset."""
    preset = project.get("preset")
    if preset:
        preset_path = metadata_root / "carriers" / f"{preset}.yaml"
        if preset_path.is_file():
            print(f"OK   carrier preset: {preset}")
            return 0
        print(f"FAIL carrier: `preset: {preset}` does not resolve",
              file=sys.stderr)
        print(f"     expected shared definition at "
              f"{preset_path.relative_to(REPO) if preset_path.is_relative_to(REPO) else preset_path}",
              file=sys.stderr)
        return 2

    # Inline carrier definition.  Schema already enforced
    # `name:` is present; check populated/e1m_routes are non-empty.
    name = project.get("name")
    populated = project.get("populated") or {}
    routes = project.get("e1m_routes") or {}
    if populated or routes:
        print(f"OK   carrier: '{name}' defined inline "
              f"({len(populated)} populated, "
              f"{sum(len(v or []) for v in routes.values())} routes)")
    else:
        print(f"OK   carrier: '{name}' defined inline (empty -- headless / inference-only)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a board.yaml against the SDK's schema + preset library.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT_DEFAULT,
                        help="Override the metadata search root.")
    parser.add_argument("--no-presets", action="store_true",
                        help="Skip the SoM SKU + carrier preset checks (schema-only mode).")
    args = parser.parse_args()

    project = _load_yaml(args.input)
    if project is None:
        return 1

    if not _check_schema(project, args.input):
        return 1

    if args.no_presets:
        print(f"\n{args.input}: schema OK (preset checks skipped via --no-presets)")
        return 0

    som_rv = _check_som_preset(project, args.metadata_root)
    carrier_rv = _check_carrier_preset(project, args.metadata_root)
    hw_rv = _check_hw_compat(project, args.metadata_root)
    periph_rv = _check_peripherals_vs_soc(project, args.metadata_root)

    if hw_rv == 3 or periph_rv == 3:
        print(f"\n{args.input}: hardware / capability incompatibility",
              file=sys.stderr)
        return 3
    if som_rv == 2 or carrier_rv == 2 or hw_rv == 2 or periph_rv == 2:
        print(f"\n{args.input}: missing-preset failures", file=sys.stderr)
        return 2

    if som_rv == 1:
        print(f"\n{args.input}: clean (with warnings)")
    else:
        print(f"\n{args.input}: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

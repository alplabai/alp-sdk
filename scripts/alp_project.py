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
SCHEMA_V2 = METADATA_ROOT / "schemas" / "board-config-v2.schema.json"
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
    if not SCHEMA.is_file():
        # Phase 1 of the 2026-05-15 redesign deleted v1 schema.  Phase 4
        # rewrites every shipped board.yaml; until then the v1 emit
        # modes are kept as a best-effort backwards-compat path.
        print(f"alp_project: board-config-v1.schema.json was removed in "
              f"the Phase 1 metadata land; this v1 board.yaml cannot "
              f"be schema-validated.  Phase 4 rewrites every board.yaml "
              f"to v2; this command runs against v2 inputs only.",
              file=sys.stderr)
        return
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
            # Per-NPU kernel driver selection.  The dispatch gate
            # above (CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y) pulls in the
            # TFLM op resolver; the gates below pull in the matching
            # low-level driver shim:
            #   - U85 lives on Alif E4 / E6 / E8 only (one per SoC,
            #     Transformer-capable).
            #   - U55 lives on every Alif Ensemble SKU (two per SoC).
            #   - U65 lives on i.MX 93 (driver shim is N93-specific).
            # Keep `ethos_u` as the customer-facing token; consumers
            # don't have to know which variant the silicon carries.
            if silicon in ("alif:ensemble:e4", "alif:ensemble:e6", "alif:ensemble:e8"):
                lines.append("CONFIG_ALP_TFLM_ETHOS_U85=y")
            if silicon and silicon.startswith("alif:ensemble:"):
                lines.append("CONFIG_ALP_TFLM_ETHOS_U55=y")
            if silicon == "nxp:imx9:imx93":
                lines.append("CONFIG_ALP_TFLM_ETHOS_U65=y")
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
        # §D.lib.loader -- cross-library HW-backend wiring: for each
        # library that ships a metadata/library-profiles/<name>/
        # hw-backends.yaml, emit the highest-priority matching
        # CONFIG_* per accelerator class given the active SoM family.
        # The SW fallback is already emitted unconditionally above
        # (via _LIBRARY_KCONFIG); this layer adds the HW acceleration
        # on top.
        hw_lines = _emit_library_hw_backends(libs, project["som"]["sku"])
        if hw_lines:
            lines.append("")
            lines.append("# §D.lib.loader -- per-library HW-accelerator wiring (auto-emitted).")
            lines.extend(hw_lines)
        lines.append("")

    return "\n".join(lines) + "\n"


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
    highest-priority matching backend per accelerator class given the
    active SoM SKU and emit the matching `CONFIG_*=y` line.

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
    """
    import re
    from pathlib import Path

    family       = _sku_family(sku)
    soc_token    = _SOC_FAMILY_TOKEN.get(family)
    if soc_token is None:
        return []

    out: list[str] = []
    repo_root = Path(__file__).resolve().parent.parent

    # Pull the SKU's `silicon:` ref + `capabilities:` block out of
    # metadata/e1m_modules/<sku>.yaml so per-silicon and per-capability
    # backends can match.
    silicon_ref: str | None = None
    capabilities: dict[str, str] = {}
    sku_path = repo_root / "metadata" / "e1m_modules" / f"{sku}.yaml"
    if sku_path.exists():
        in_caps = False
        for raw in sku_path.read_text(encoding="utf-8").splitlines():
            sm = re.match(r"^silicon:\s*(\S+)", raw)
            if sm:
                silicon_ref = sm.group(1).strip()
                continue
            # `capabilities:` is a top-level key; entries inside are
            # indented `  key: value` until the next top-level key
            # (no leading whitespace).
            if raw.startswith("capabilities:"):
                in_caps = True
                continue
            if in_caps:
                if raw and not raw[0].isspace():
                    in_caps = False
                else:
                    cm = re.match(r"^\s+(\w+):\s*(\S+)", raw)
                    if cm:
                        capabilities[cm.group(1)] = cm.group(2).rstrip(",")

    def _cap_truthy(name: str) -> bool:
        v = capabilities.get(name)
        if v is None:
            return False
        v = v.lower()
        if v in ("true", "yes"):
            return True
        if v in ("false", "no", "null", "none", "0"):
            return False
        # Numeric value (e.g. ethos_u55_count: 2): truthy when > 0.
        try:
            return int(v) > 0
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
            if not kcv:
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
    *,
    v2_peripherals: list[str] | None = None,
    v2_core_id: str | None = None,
    v2_core_os: str | None = None,
) -> str:
    """Emit a Zephyr DTS overlay describing the carrier wiring.

    v1 path (`v2_peripherals is None`): reads project-level
    `peripherals:` implicitly via the carrier header macros.

    v2 path: the project's peripherals live under `cores.<id>.peripherals`.
    Callers compute the union across Zephyr/baremetal cores (or pick one
    when `--core <id>` is supplied) and pass it in via `v2_peripherals`.
    The list is currently informational -- the bus aliases + `alp,pin-array`
    binding root node are derived from the carrier header, which describes
    the SoM mounting, not the project.  When `v2_core_os` is set to a
    non-Zephyr runtime (`yocto`, `off`, ...), the emitter returns a stub
    overlay with just the header comment.
    """
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

    # v2 short-circuit: a non-Zephyr core has no Zephyr overlay to emit.
    # Customer-passed `--core <id>` may target a yocto / off slice -- the
    # emitter returns a stub so the caller's pipeline doesn't fail.
    if v2_core_os is not None and v2_core_os not in ("zephyr", "baremetal"):
        lines.append(f"// --core {v2_core_id} has os: {v2_core_os}; no Zephyr DTS overlay applies.")
        return "\n".join(lines) + "\n"

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
    *,
    v2_libraries: list[str] | None = None,
) -> str:
    """Emit a west.yml fragment that the customer's manifest can
    import to pin the Zephyr modules board.yaml's `libraries:` array
    requires.  Idempotent: emitting an empty `libraries:` array gives
    an empty (but well-formed) name-allowlist.

    v1 path (`v2_libraries is None`): reads project-level `libraries:`.
    v2 path: callers compute the union across the Zephyr-runtime cores
    (or pick one when `--core <id>` is supplied) and pass it in via
    `v2_libraries`.
    """
    del sku_preset, carrier_preset  # unused -- libraries are SoM-agnostic
    if v2_libraries is not None:
        libs = list(v2_libraries)
    else:
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
    carrier_preset: dict[str, Any] | None,
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

    carrier_block = project.get("carrier") or {}
    carrier_name = carrier_block.get("name") or ""
    carrier_hw_rev = ""
    if carrier_name and carrier_preset is not None:
        carrier_hw_rev = (carrier_block.get("hw_rev")
                          or carrier_preset.get("default_hw_rev")
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
    if carrier_name:
        lines.append(f'#define ALP_HW_BUILD_CARRIER_NAME    "{carrier_name}"')
        if carrier_hw_rev:
            lines.append(f'#define ALP_HW_BUILD_CARRIER_HW_REV  "{carrier_hw_rev}"')
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
# v2 emit shims (Phase 2)
# ---------------------------------------------------------------------
#
# The orchestrator (scripts/alp_orchestrate.py) owns the v2 board.yaml
# loader + carve-out resolver + system-manifest emitter.  The v1
# loader above keeps working unchanged for backwards compatibility;
# these shims route the new v2-only `--emit` modes (and the per-core
# `--emit zephyr-conf --core <id>`) through the orchestrator.
#
# Phase 4 rewrites every shipped board.yaml; once that lands, the v1
# path becomes dead code we can excise.


def _write_or_print(out: str, target: Path | None) -> int:
    if target is not None:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(out, encoding="utf-8")
        try:
            rel = target.relative_to(Path.cwd())
        except ValueError:
            rel = target
        print(f"alp_project: wrote {rel} ({len(out)} bytes)",
              file=sys.stderr)
    else:
        sys.stdout.write(out)
    return 0


def _run_v2_emit(args: argparse.Namespace) -> int:
    """Handle the three project-level v2 emit modes."""
    if args.core is not None:
        print(f"alp_project: --core is ignored for --emit {args.emit} "
              f"(project-level emit)", file=sys.stderr)
    try:
        # Imported here so the v1 path doesn't pay the import cost when
        # the orchestrator module is being modified in-tree.
        from alp_orchestrate import (
            OrchestratorError,
            emit_dts_reservations,
            emit_ipc_contract_h,
            emit_system_manifest,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
        if args.emit == "system-manifest":
            out = emit_system_manifest(project)
        elif args.emit == "ipc-contract-h":
            out = emit_ipc_contract_h(project)
        elif args.emit == "dts-reservations":
            out = emit_dts_reservations(project)
        else:
            print(f"alp_project: unknown v2 emit '{args.emit}'",
                  file=sys.stderr)
            return 1
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    return _write_or_print(out, args.output)


def _run_v2_per_core_emit(args: argparse.Namespace) -> int:
    """v2 board.yaml + per-core --emit zephyr-conf / yocto-conf, plus the
    project-wide legacy emit modes (`dts-overlay`, `hw-info-h`,
    `west-libraries`) re-fitted for the v2 schema.

    The orchestrator owns the per-slice config emitters; this shim
    delegates after resolving the requested core (or summing across
    cores when `--core` is unset).
    """
    try:
        from alp_orchestrate import (
            OrchestratorError,
            _slice_alp_conf,
            _slice_cmake_args,
            _slice_local_conf,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    # Validate --core if supplied (used by every emit path).
    if args.core is not None and args.core not in project.cores:
        print(f"alp_project: --core {args.core} not present in "
              f"board.yaml (known: {sorted(project.cores.keys())})",
              file=sys.stderr)
        return 1

    # Build a v1-shaped dict that the legacy emitters can read from for
    # som / carrier fields.  Used by dts-overlay + hw-info-h + west-
    # libraries v2 paths.
    project_v1_shaped: dict[str, Any] = {
        "som": {
            "sku":    project.sku,
            "hw_rev": project.hw_rev,
        },
        "carrier": ({
            "name":   project.carrier_name,
            "hw_rev": project.carrier_hw_rev,
        } if project.carrier_name else None),
    }

    # --- legacy project-wide emits, v2-flavoured -------------------------
    if args.emit == "dts-overlay":
        # The DTS overlay is shaped by the carrier header (bus aliases +
        # alp,pin-array) which is a SoM-mounting fact, not a per-core
        # fact.  v2 contributes only the peripherals list: union across
        # Zephyr/baremetal cores (or one core when --core is set).
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_peripherals = sorted(set(slice_.peripherals))
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.carrier_preset,
                v2_peripherals=v2_peripherals,
                v2_core_id=args.core,
                v2_core_os=slice_.os,
            )
        else:
            union: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union.update(slice_.peripherals)
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.carrier_preset,
                v2_peripherals=sorted(union),
            )
        return _write_or_print(out, args.output)

    if args.emit == "hw-info-h":
        # hw-info-h is a project-level emit even under v2 -- consumers
        # `#include` it from any slice.  --core picks which slice's OS
        # lands in ALP_HW_BUILD_OS; absent --core, primary-core rules apply.
        v2_cores = {cid: s.os for cid, s in project.cores.items()}
        out = _emit_hw_info_h(
            project_v1_shaped, project.som_preset,
            project.carrier_preset,
            v2_cores=v2_cores,
            v2_selected_core=args.core,
        )
        return _write_or_print(out, args.output)

    if args.emit == "west-libraries":
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_libraries = sorted(set(slice_.libraries))
        else:
            union_l: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union_l.update(slice_.libraries)
            v2_libraries = sorted(union_l)
        out = _emit_west_libraries(
            project_v1_shaped, project.som_preset,
            project.carrier_preset,
            v2_libraries=v2_libraries,
        )
        return _write_or_print(out, args.output)

    # --- per-core emits (zephyr-conf / yocto-conf / cmake-args) ----------
    #
    # If --core is unset, sum across cores.  Per spec §4.6, the new
    # per-core invocation is the canonical entry point; the unscoped
    # invocation is a sum-across-cores convenience for tools that
    # haven't moved off the v1 single-OS world yet.
    if args.core is not None:
        core_ids = [args.core]
    else:
        core_ids = sorted(project.cores.keys())

    parts: list[str] = []
    for cid in core_ids:
        slice_ = project.cores[cid]
        if slice_.os == "off":
            continue
        if args.emit == "zephyr-conf":
            if slice_.os != "zephyr":
                # When --core is unset, filter by os: zephyr; with --core,
                # honour the explicit selection but warn.
                if args.core is None:
                    continue
                print(f"alp_project: --core {cid} has os: {slice_.os}; "
                      f"emitting Kconfig fragment anyway", file=sys.stderr)
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_alp_conf(project, slice_))
        elif args.emit == "yocto-conf":
            if slice_.os != "yocto":
                if args.core is None:
                    continue
                print(f"alp_project: --core {cid} has os: {slice_.os}; "
                      f"emitting local.conf snippet anyway", file=sys.stderr)
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_local_conf(project, slice_))
        elif args.emit == "cmake-args":
            if slice_.os not in ("baremetal", "zephyr"):
                if args.core is None:
                    continue
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_cmake_args(project, slice_))
        else:
            print(f"alp_project: unknown --emit {args.emit} for v2 board.yaml",
                  file=sys.stderr)
            return 1

    out = "\n".join(parts) + ("\n" if parts and not parts[-1].endswith("\n") else "")
    return _write_or_print(out, args.output)


# ---------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile board.yaml -> per-backend native config.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--emit",
                        choices=["zephyr-conf", "cmake-args", "yocto-conf",
                                 "dts-overlay", "hw-info-h", "west-libraries",
                                 # v2 orchestration emits (Phase 2):
                                 "system-manifest", "dts-reservations",
                                 "ipc-contract-h"],
                        default="zephyr-conf",
                        help="Output format (default: zephyr-conf).")
    parser.add_argument("--output", type=Path, default=None,
                        help="Write to this path; default: stdout.")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT,
                        help="Override the metadata search root.")
    parser.add_argument("--core", default=None,
                        help="When the project is v2, limit emits to this "
                             "core ID.  For per-core emit modes "
                             "(zephyr-conf, yocto-conf, cmake-args) this "
                             "picks the single slice to emit.  For "
                             "project-wide emit modes (dts-overlay, "
                             "hw-info-h, west-libraries) this scopes the "
                             "union calculation to a single slice (e.g. "
                             "ALP_HW_BUILD_OS reflects the selected core's "
                             "runtime).  Ignored for system-manifest, "
                             "ipc-contract-h, dts-reservations.")
    args = parser.parse_args()

    # v2 emit modes route through scripts/alp_orchestrate.py.  We resolve
    # them before the v1 path so a v2 input doesn't trip the v1 validator.
    if args.emit in ("system-manifest", "dts-reservations",
                     "ipc-contract-h"):
        return _run_v2_emit(args)

    project = _load_yaml(args.input)

    # v2 board.yamls flow through the per-core / project-wide emit path
    # in _run_v2_per_core_emit.  Project-wide v2 emits (system-manifest,
    # dts-reservations, ipc-contract-h) were already dispatched above.
    # --core is honoured for zephyr-conf / yocto-conf / cmake-args (per-
    # core fan-out) and for dts-overlay / hw-info-h / west-libraries
    # (scopes the union calculation to a single slice).
    schema_version = project.get("schema_version")
    if schema_version == 2:
        return _run_v2_per_core_emit(args)
    elif args.core is not None:
        print(f"alp_project: --core requires a v2 board.yaml; input is "
              f"v{schema_version} -- ignoring", file=sys.stderr)

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

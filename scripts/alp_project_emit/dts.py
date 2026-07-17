#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Zephyr DTS overlay emission (`--emit dts-overlay`).

Board-macro parsing (reads `include/alp/boards/<board>.h`) + the carrier
peripheral DT-wiring catalog + `_emit_dts_overlay` itself.  Split out of
the former flat `alp_project_emit.py` module (issue #673 Phase 1) -- see
`scripts/alp_project_emit/__init__.py` for the package-level contract.
Structural split only, no behaviour change; `check_emit_snapshots.py`
pins the byte-identical output.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Any

from alp_project_loader import REPO, _sku_family

from . import _e1m_gpio_canonical


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
# their SoM's actual DT controller phandles by cross-referencing the
# in-tree board file (zephyr/boards/alp/<board>/*.dts, wired in via
# zephyr/module.yml's `board_root: zephyr`).
#
# Bus phandle naming convention matches the manually-written EVK
# overlays at tests/zephyr/peripheral/boards/alp_e1m_aen801_m55_he.overlay:
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
    lines.append(" * Per-pad GPIO bank/index values are TBD; cross-reference the in-tree")
    lines.append(" * board file at zephyr/boards/alp/<board>/*.dts (zephyr/module.yml board_root).")
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

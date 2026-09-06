#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate include/alp/boards/alp_<board>_routes.h from each
metadata/boards/<name>.yaml `e1m_routes:` + `i2c_devices:` +
`overlay_pins:` + `mux_enums:` blocks.

The generated header mirrors the YAML `e1m_routes:` block into plain
`#define EVK_* ALP_E1M_*` lines so hand-written firmware can keep using
the established board macros (EVK_PIN_BMI323_INT1, EVK_I2C_BUS_SENSORS,
EVK_PWM_LED_RED, ...) while the YAML stays the single editable source
of truth.  The YAML carries the connector-namespace pad names
(`E1M_...` / `E1M_X_...`); the generator prefixes `ALP_` when emitting
the C token.  The `i2c_devices:` block does the same for on-board I2C
device addresses (EVK_I2C_ADDR_*) and INA236 shunt/max-current
calibration (EVK_INA236_SHUNT_*_OHMS / EVK_INA236_MAX_*_A) -- these
carry a literal hex/float value rather than an `ALP_E1M_*` token.  The
`overlay_pins:` block emits `EVK_PIN_OVERLAY_BASE + N` pad indices for
boards that expose repurposed pads past the standard E1M pinout to an
`alp,pin-array` devicetree overlay; N is the entry's position in the
YAML list (an ordinal, not a hardware fact), computed via `enumerate()`
rather than read from the YAML.  The `mux_enums:` block emits
mux-select / IO-expander-pin `typedef enum { ... }` blocks
(evk_sdio_select_t, evk_pcie_ioexp_pin_t, ...) -- issue #637.
Idempotent: running twice produces byte-identical output.

The remaining content of `include/alp/boards/alp_<board>.h` is prose
explaining the hardware those generated macros/enums bind to (plus
any board-specific enum a slice hasn't lifted yet, e.g.
`evk_cam_select_t`).

Run:

    python3 scripts/gen_board_header.py

CI (when wired) regenerates on every PR that touches the shared
board YAMLs, then fails if the working tree diff is non-empty.

The companion `gen_soc_caps.py` does the same trick for SoC capability
macros under `metadata/socs/*.json` -> `include/alp/soc_caps.h`.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("gen_board_header: PyYAML required.  pip install pyyaml")


REPO = Path(__file__).resolve().parent.parent
BOARDS_DIR = REPO / "metadata" / "boards"
OUT_DIR = REPO / "include" / "alp" / "boards"


_SECTIONS: list[tuple[str, str]] = [
    ("gpio",  "GPIO routes (ALP_E1M_GPIO_IO<N> -> board-side feature)"),
    ("buses", "Bus assignments (ALP_E1M_I2C / I3C / SPI / UART -> board role)"),
    ("pwm",   "PWM channels (ALP_E1M_PWM<N> -> board-side feature)"),
    ("adc",   "ADC channels (ALP_E1M_ADC<N> -> board-side signal)"),
    ("dac",   "DAC channels (ALP_E1M_DAC<N> -> board-side signal)"),
    ("i2s",   "I2S instances (ALP_E1M_I2S<N> -> board-side codec / mic role)"),
    ("can",   "CAN buses (ALP_E1M_CAN<N> -> board-side bus role)"),
    ("qenc",  "Quadrature encoder channels (ALP_E1M_ENC<N> -> board-side encoder)"),
]


def _c_token(e1m: str) -> str:
    """YAML `e1m:` pad name (connector namespace, `E1M_...`) -> the C
    macro token the pinout headers define (`ALP_E1M_...`)."""
    return f"ALP_{e1m}"


def _board_slug(name: str) -> str:
    """`E1M-EVK` -> `e1m_evk`."""
    return name.lower().replace("-", "_")


def _pinout_include(routes: dict[str, Any]) -> str:
    """Pick the pinout header the generated routes header must pull.

    E1M (35x35) and E1M-X (45x65) are deliberately separate pinout
    namespaces (`<alp/e1m_pinout.h>` vs `<alp/e1m_x_pinout.h>`); a
    board's routes live entirely in one.  Detect E1M-X by the
    `E1M_X_` pad-name prefix so the generated header includes the
    matching pad definitions.
    """
    for entries in routes.values():
        for entry in entries or []:
            if str(entry.get("e1m", "")).startswith("E1M_X_"):
                return "alp/e1m_x_pinout.h"
    return "alp/e1m_pinout.h"


def _build_doc(entry: dict[str, Any]) -> str:
    """Compose a single-line Doxygen `@brief` for the entry."""
    parts: list[str] = []
    if entry.get("doc"):
        parts.append(str(entry["doc"]).strip())
    if entry.get("active_low"):
        parts.append("Active-low.")
    return " ".join(parts)


def _emit_i2c_devices(devices: list[dict[str, Any]]) -> list[str]:
    """Emit the on-board I2C device address block (+ any `alias`) and,
    for entries carrying a `calibration:` block, the paired INA236
    shunt/max-current macros.  Mirrors `_emit_section`'s column-aligned
    style but the value is a literal hex/float, not an `ALP_E1M_*`
    pinout token."""
    if not devices:
        return []

    addr_lines: list[tuple[str, str, str]] = []  # (macro, value, doc)
    for entry in devices:
        macro = entry["macro"]
        addr_lines.append((macro, f"{entry['address']}u", entry.get("doc", "")))
        alias = entry.get("alias")
        if alias:
            addr_lines.append((alias, macro, f"Alias for {macro}."))

    calib_lines: list[tuple[str, str, str]] = []
    for entry in devices:
        cal = entry.get("calibration")
        if not cal:
            continue
        macro = entry["macro"]
        calib_lines.append(
            (cal["shunt_macro"], f"{cal['shunt_ohms']}f", f"Shunt for {macro}.")
        )
        calib_lines.append(
            (cal["max_macro"], f"{cal['max_current_a']}f", f"Max current for {macro}.")
        )

    out: list[str] = [
        "/* ------------------------------------------------------------------ */",
        "/* On-board I2C device addresses (from `i2c_devices:`) */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    widest = max(len(m) for m, _, _ in addr_lines)
    for macro, value, doc in addr_lines:
        if doc:
            out.append(f"#define {macro:<{widest}} {value}  /**< {doc} */")
        else:
            out.append(f"#define {macro:<{widest}} {value}")
    out.append("")

    if calib_lines:
        out.extend([
            "/* ------------------------------------------------------------------ */",
            "/* INA236 calibration constants (from `i2c_devices[].calibration`) */",
            "/* ------------------------------------------------------------------ */",
            "",
        ])
        widest = max(len(m) for m, _, _ in calib_lines)
        for macro, value, doc in calib_lines:
            out.append(f"#define {macro:<{widest}} {value}  /**< {doc} */")
        out.append("")

    return out


def _emit_overlay_pins(entries: list[dict[str, Any]]) -> list[str]:
    """Emit `EVK_PIN_OVERLAY_BASE` + the board's overlay-extended
    `alp,pin-array` pad indices (`overlay_pins:`).  Each macro is
    `EVK_PIN_OVERLAY_BASE + N`, where N is the entry's position in
    THIS list (0-based) -- an ordinal, not an independent hardware
    fact, so it is computed here via `enumerate()` rather than read
    from the YAML.  See zephyr/dts/bindings/alp,pin-array.yaml for
    the array-index convention this mirrors."""
    if not entries:
        return []

    out: list[str] = [
        "/* ------------------------------------------------------------------ */",
        "/* Overlay-extended pin-array indices (from `overlay_pins:`) */",
        "/* ------------------------------------------------------------------ */",
        "",
        "#define EVK_PIN_OVERLAY_BASE ALP_E1M_GPIO_COUNT",
        "",
    ]
    widest = max(len(e["macro"]) for e in entries)
    for idx, entry in enumerate(entries):
        macro = entry["macro"]
        doc = entry.get("doc", "")
        value = f"(EVK_PIN_OVERLAY_BASE + {idx}u)"
        if doc:
            out.append(f"#define {macro:<{widest}} {value}  /**< {doc} */")
        else:
            out.append(f"#define {macro:<{widest}} {value}")
    out.append("")
    return out


def _emit_mux_enums(enums: list[dict[str, Any]]) -> list[str]:
    """Emit one `typedef enum { ... } <name>;` block per `mux_enums:`
    entry.  Column-aligns each enum's `=` signs independently (resets
    per enum, mirroring `Consecutive` clang-format alignment) so a
    long enumerator name in one enum doesn't reflow a sibling enum."""
    if not enums:
        return []

    out: list[str] = [
        "/* ------------------------------------------------------------------ */",
        "/* Board mux-select enums (from `mux_enums:`) */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    for enum in enums:
        name = enum["name"]
        values = enum["values"]
        doc = enum.get("doc")
        # Duplicate enumerator values emit silently and read as a typo
        # in the header; the schema cannot express cross-item
        # uniqueness, so enforce it here rather than shipping two names
        # bound to the same selector.
        seen: dict[int, str] = {}
        for v in values:
            prev = seen.get(v["value"])
            if prev is not None:
                raise ValueError(
                    f"{name}: {v['name']} and {prev} both use value "
                    f"{v['value']} -- enumerator values must be unique"
                )
            seen[v["value"]] = v["name"]
        if doc:
            out.append(f"/** {doc} */")
        out.append("typedef enum {")
        widest = max(len(v["name"]) for v in values)
        for v in values:
            line = f"\t{v['name']:<{widest}} = {v['value']},"
            vdoc = v.get("doc")
            if vdoc:
                line += f" /**< {vdoc} */"
            out.append(line)
        out.append(f"}} {name};")
        out.append("")
    return out


def _emit_board_aliases(routes: dict[str, Any]) -> list[str]:
    """Emit portable BOARD_* aliases for every entry carrying a
    `board_alias:` (the e1m-spec §7.2 common roles).  Same BOARD_* name
    on every board -> a board-agnostic example resolves per built board."""
    pairs: list[tuple[str, str]] = []
    for entries in routes.values():
        for entry in entries or []:
            alias = entry.get("board_alias")
            if alias:
                pairs.append((alias, entry["macro"]))
    if not pairs:
        return []
    pairs.sort()
    widest = max(len(a) for a, _ in pairs)
    out = [
        "/* ------------------------------------------------------------------ */",
        "/* Portable cross-EVK aliases (e1m-spec STANDARD.md §7.2 common set). */",
        "/* Same BOARD_* names on every board; include via <alp/board.h>.       */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    for alias, macro in pairs:
        out.append(f"#define {alias:<{widest}} {macro}")
    out.append("")
    return out


def _emit_section(title: str, entries: list[dict[str, Any]]) -> list[str]:
    out: list[str] = [
        "/* ------------------------------------------------------------------ */",
        f"/* {title} */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    if not entries:
        out.append("/* (no entries declared in board YAML) */")
        out.append("")
        return out

    widest = max(len(e["macro"]) for e in entries)
    for entry in entries:
        macro = entry["macro"]
        token = _c_token(entry["e1m"])
        doc = _build_doc(entry)
        if doc:
            out.append(f"#define {macro:<{widest}} {token}  /**< {doc} */")
        else:
            out.append(f"#define {macro:<{widest}} {token}")
    out.append("")
    return out


def emit_board(name: str, doc: dict[str, Any]) -> str | None:
    """Return the full text of the generated routes header for one board,
    or `None` if the shared board YAML has none of an `e1m_routes:`,
    `i2c_devices:`, `overlay_pins:`, or `mux_enums:` block.
    """
    routes = doc.get("e1m_routes")
    i2c_devices = doc.get("i2c_devices")
    overlay_pins = doc.get("overlay_pins")
    mux_enums = doc.get("mux_enums")
    if not routes and not i2c_devices and not overlay_pins and not mux_enums:
        return None
    routes = routes or {}
    slug = _board_slug(name)
    guard = f"ALP_BOARDS_{slug.upper()}_ROUTES_H"
    pinout = _pinout_include(routes)

    lines: list[str] = [
        "/*",
        " * Copyright 2026 Alp Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        f" * Auto-generated from metadata/boards/{slug.replace('_', '-')}.yaml",
        " * by scripts/gen_board_header.py.  DO NOT EDIT BY HAND --",
        " * regenerate after changing the YAML.",
        " *",
        " * Mirrors the board YAML's `e1m_routes:` block into plain",
        " * `#define EVK_<NAME> ALP_E1M_<...>` lines so hand-written firmware",
        " * can keep using the board-named macros while the YAML stays",
        " * the single editable source of truth.",
        " *",
        " * @par ABI status: [ABI-STABLE]",
        " *      v0.6 generated; macro names + values track the board YAML.",
        " *      See docs/abi-markers.md for the convention.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        f"#include \"{pinout}\"",
        "",
        "/* This header is auto-generated; clang-format ignores it so the",
        " * generator's column-aligned `#define` blocks survive PR static",
        " * analysis without forcing 100-col wraps on long doc strings. */",
        "/* clang-format off */",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
    ]

    for section_key, section_title in _SECTIONS:
        entries = routes.get(section_key) or []
        if entries:
            lines.extend(_emit_section(section_title, entries))

    lines.extend(_emit_i2c_devices(doc.get("i2c_devices") or []))

    lines.extend(_emit_overlay_pins(doc.get("overlay_pins") or []))

    lines.extend(_emit_mux_enums(mux_enums or []))

    lines.extend(_emit_board_aliases(routes))

    lines.extend([
        "#ifdef __cplusplus",
        "} /* extern \"C\" */",
        "#endif",
        "",
        "/* clang-format on */",
        "",
        f"#endif /* {guard} */",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    if not BOARDS_DIR.is_dir():
        print(
            f"gen_board_header: {BOARDS_DIR.relative_to(REPO)} not found",
            file=sys.stderr,
        )
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    written = 0
    expected: set[Path] = set()
    # slug -> source YAML path that claimed it.  Keyed on the slug ALONE:
    # two board YAMLs with the identical `name:` (a preset copy-pasted
    # and never renamed -- the realistic path) collapse onto the same
    # slug just as surely as two distinct names that `_board_slug()`'s
    # lossy case/`-`/`_` fold happens to collide on (e.g. `FOO-BAR` and
    # `foo_bar`); without this check the second write would silently
    # clobber the first with no error (#1128).
    slug_owners: dict[str, Path] = {}
    for preset_path in sorted(BOARDS_DIR.glob("*.yaml")):
        doc = yaml.safe_load(preset_path.read_text(encoding="utf-8"))
        if not isinstance(doc, dict):
            continue
        name = doc.get("name") or preset_path.stem
        out_text = emit_board(name, doc)
        if out_text is None:
            continue

        slug = _board_slug(name)
        if slug in slug_owners:
            print(
                f"gen_board_header: {slug_owners[slug].relative_to(REPO)} "
                f"and {preset_path.relative_to(REPO)} both slugify to "
                f"{slug!r} -- "
                f"{OUT_DIR.relative_to(REPO)}/alp_{slug}_routes.h would be "
                f"silently overwritten by whichever board is processed "
                f"last.  Rename one of them.",
                file=sys.stderr,
            )
            return 1
        slug_owners[slug] = preset_path

        out_path = OUT_DIR / f"alp_{slug}_routes.h"
        out_path.write_text(out_text, encoding="utf-8", newline="")
        expected.add(out_path)
        print(
            f"wrote {out_path.relative_to(REPO)} "
            f"({len(out_text.splitlines())} lines)"
        )
        written += 1

    # Reconciliation pass: a board YAML that was renamed or deleted must
    # take its generated header with it -- otherwise the stale header
    # stays committed (and the diff-only CI gate stays green, since it
    # only ever compares files this script still touches) forever (#1128).
    for existing in sorted(OUT_DIR.glob("alp_*_routes.h")):
        if existing not in expected:
            existing.unlink()
            print(
                f"removed orphaned {existing.relative_to(REPO)} "
                f"(no matching board YAML)"
            )

    if written == 0:
        print(
            "gen_board_header: no shared board YAMLs with "
            "e1m_routes blocks found",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

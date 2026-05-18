#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate include/alp/boards/alp_<carrier>_routes.h from each
metadata/carriers/<NAME>/board.yaml `e1m_routes:` block.

The generated header mirrors the YAML `e1m_routes:` block into plain
`#define EVK_* E1M_*` lines so hand-written firmware can keep using
the established carrier macros (EVK_PIN_BMI323_INT1, EVK_I2C_BUS_SENSORS,
EVK_PWM_LED_RED, ...) while the YAML stays the single editable source
of truth.  Idempotent: running twice produces byte-identical output.

The remaining sections of `include/alp/boards/alp_<carrier>.h`
(mux enums, INA236 tuning constants, overlay-pad indices, on-board
I2C addresses, prose comments) stay hand-authored until follow-up
slices lift them too.

Run:

    python3 scripts/gen_carrier_header.py

CI (when wired) regenerates on every PR that touches the carrier
YAML, then fails if the working tree diff is non-empty.

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
    sys.exit("gen_carrier_header: PyYAML required.  pip install pyyaml")


REPO = Path(__file__).resolve().parent.parent
CARRIERS_DIR = REPO / "metadata" / "carriers"
OUT_DIR = REPO / "include" / "alp" / "boards"


_SECTIONS: list[tuple[str, str]] = [
    ("gpio",  "GPIO routes (E1M_GPIO_IO<N> -> carrier-side feature)"),
    ("buses", "Bus assignments (E1M peripheral instance -> carrier role)"),
    ("pwm",   "PWM channels (E1M_PWM<N> -> carrier-side feature)"),
]


def _carrier_slug(name: str) -> str:
    """`E1M-EVK` -> `e1m_evk`."""
    return name.lower().replace("-", "_")


def _build_doc(entry: dict[str, Any]) -> str:
    """Compose a single-line Doxygen `@brief` for the entry."""
    parts: list[str] = []
    if entry.get("doc"):
        parts.append(str(entry["doc"]).strip())
    if entry.get("active_low"):
        parts.append("Active-low.")
    return " ".join(parts)


def _emit_section(title: str, entries: list[dict[str, Any]]) -> list[str]:
    out: list[str] = [
        "/* ------------------------------------------------------------------ */",
        f"/* {title} */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    if not entries:
        out.append("/* (no entries declared in carrier YAML) */")
        out.append("")
        return out

    widest = max(len(e["macro"]) for e in entries)
    for entry in entries:
        macro = entry["macro"]
        e1m = entry["e1m"]
        doc = _build_doc(entry)
        if doc:
            out.append(f"#define {macro:<{widest}} {e1m}  /**< {doc} */")
        else:
            out.append(f"#define {macro:<{widest}} {e1m}")
    out.append("")
    return out


def emit_carrier(name: str, doc: dict[str, Any]) -> str | None:
    """Return the full text of the generated routes header for one carrier,
    or `None` if the carrier preset has no `e1m_routes:` block.
    """
    routes = doc.get("e1m_routes")
    if not routes:
        return None
    slug = _carrier_slug(name)
    guard = f"ALP_BOARDS_{slug.upper()}_ROUTES_H"

    lines: list[str] = [
        "/*",
        " * Copyright 2026 ALP Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        f" * Auto-generated from metadata/carriers/{name}/board.yaml",
        " * by scripts/gen_carrier_header.py.  DO NOT EDIT BY HAND --",
        " * regenerate after changing the YAML.",
        " *",
        " * Mirrors the carrier preset's `e1m_routes:` block into plain",
        " * `#define EVK_<NAME> E1M_<...>` lines so hand-written firmware",
        " * can keep using the carrier-named macros while the YAML stays",
        " * the single editable source of truth.",
        " *",
        " * @par ABI status: [ABI-STABLE]",
        " *      v0.6 generated; macro names + values track the carrier YAML.",
        " *      See docs/abi-markers.md for the convention.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include \"alp/e1m_pinout.h\"",
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

    lines.extend([
        "#ifdef __cplusplus",
        "} /* extern \"C\" */",
        "#endif",
        "",
        f"#endif /* {guard} */",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    if not CARRIERS_DIR.is_dir():
        print(
            f"gen_carrier_header: {CARRIERS_DIR.relative_to(REPO)} not found",
            file=sys.stderr,
        )
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    written = 0
    for carrier_dir in sorted(CARRIERS_DIR.iterdir()):
        if not carrier_dir.is_dir():
            continue
        preset = carrier_dir / "board.yaml"
        if not preset.is_file():
            continue
        doc = yaml.safe_load(preset.read_text(encoding="utf-8"))
        if not isinstance(doc, dict):
            continue
        name = doc.get("name") or carrier_dir.name
        out_text = emit_carrier(name, doc)
        if out_text is None:
            continue
        out_path = OUT_DIR / f"alp_{_carrier_slug(name)}_routes.h"
        out_path.write_text(out_text, encoding="utf-8")
        print(
            f"wrote {out_path.relative_to(REPO)} "
            f"({len(out_text.splitlines())} lines)"
        )
        written += 1

    if written == 0:
        print(
            "gen_carrier_header: no carriers with e1m_routes blocks found",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

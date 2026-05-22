#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate include/alp/soc_caps.h from metadata/socs/**/*.json.

Each SoC's capability macros are gated by CONFIG_ALP_SOC_<TOKEN>.
Apps select the active SoC via Kconfig (`CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y`
or similar); the SDK's `alp_*_open` functions consult the matching
`ALP_SOC_*` macros to reject configurations that exceed the SoC's
documented hardware caps.

Run:

    python3 scripts/gen_soc_caps.py

CI (when wired) regenerates the header on every PR that touches
metadata/socs/, then fails if the working tree diff is non-empty.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
META_DIR = REPO / "metadata" / "socs"
OUT = REPO / "include" / "alp" / "soc_caps.h"
CAP_H_OUT = REPO / "include" / "alp" / "cap.h"
CAP_C_OUT = REPO / "src" / "cap.c"

# Capability fields we extract from each SoC's metadata.
# (field_name, lambda: derives the integer value from peripherals dict)
CAPS: list[tuple[str, callable]] = [
    ("I2C_COUNT",
        lambda p: (p.get("i2c", 0) or 0) + (p.get("i2c_lp", 0) or 0)),
    ("SPI_COUNT",
        lambda p: (p.get("spi", 0) or 0) + (p.get("spi_lp", 0) or 0)),
    ("UART_COUNT",
        lambda p: (p.get("uart", 0) or 0) + (p.get("uart_lp", 0) or 0)),
    ("I2S_COUNT",
        lambda p: (p.get("i2s", 0) or 0) + (p.get("i2s_lp", 0) or 0)),
    ("PDM_COUNT",
        lambda p: (p.get("pdm", 0) or 0) + (p.get("pdm_lp", 0) or 0)),
    ("ADC_COUNT",
        lambda p: sum(int(v) for k, v in p.items()
                      if k.startswith("adc_") and isinstance(v, int))),
    ("ADC_MAX_RESOLUTION_BITS",
        lambda p: max(
            (int(m.group(1)) for k in p
             if (m := re.fullmatch(r"adc_(\d+)bit", k))),
            default=0)),
    ("DAC_COUNT",
        lambda p: sum(int(v) for k, v in p.items()
                      if k.startswith("dac_") and isinstance(v, int))),
    ("DAC_MAX_RESOLUTION_BITS",
        lambda p: max(
            (int(m.group(1)) for k in p
             if (m := re.fullmatch(r"dac_(\d+)bit", k))),
            default=0)),
    ("CAN_COUNT",
        lambda p: (p.get("can", 0) or 0) + (p.get("can_fd", 0) or 0)),
    ("CAN_FD_SUPPORTED",
        lambda p: 1 if (p.get("can_fd", 0) or 0) > 0 else 0),
    ("RTC_COUNT",
        lambda p: p.get("rtc", 0) or 0),
    ("WDT_COUNT",
        lambda p: p.get("watchdog", 0) or 0),
    ("QENC_COUNT",
        lambda p: p.get("encoder_quadrature", 0) or 0),
    ("TIMER_COUNT",
        lambda p: (p.get("timer_32bit", 0) or 0) +
                  (p.get("timer_lp", 0) or 0)),
    ("PWM_COUNT",
        # No SoC metadata declares "pwm" directly; PWM channels come
        # off general-purpose timers.  Use timer_32bit as the upper
        # bound until we get a more specific field.
        lambda p: p.get("pwm", p.get("timer_32bit", 0) or 0)),
    ("ETHERNET_COUNT",
        lambda p: p.get("ethernet", 0) or 0),
    ("USB_COUNT",
        lambda p: (p.get("usb_2", 0) or 0) + (p.get("usb_3", 0) or 0)),
    ("MIPI_CSI_COUNT",
        lambda p: p.get("mipi_csi2", 0) or 0),
    ("MIPI_DSI_COUNT",
        lambda p: p.get("mipi_dsi", 0) or 0),
]


# Boolean feature flags derived from each SoC's `capabilities:` block.
# When a key is absent (sparse principle) we emit 0 — feature not present.
# Keys match the `capabilities:` properties in metadata/schemas/soc-spec-v1.schema.json.
BOOL_CAPS: list[str] = [
    "xspi_dma",
    "hexspi_dma",
    "emmc_dma",
    "quadspi_dma",
    "drp_ai",
    "helium_mve",
    "neon",
    "gpu2d",
    "dave2d",
    "cryptocell",
    "inline_aes",
    "cau",
    "dma2d",
]

# Map ALP_SOC_* field name -> ALP_CAP_* name.
# Count-style fields produce HW_<NAME> (presence boolean from count > 0).
# Boolean / flag fields keep their name verbatim.
CAP_ALIASES: list[tuple[str, str, str]] = [
    # (soc_macro_name, cap_macro_name, kind: "count" | "bool")
    ("I2C_COUNT", "HW_I2C", "count"),
    ("SPI_COUNT", "HW_SPI", "count"),
    ("UART_COUNT", "HW_UART", "count"),
    ("I2S_COUNT", "HW_I2S", "count"),
    ("PDM_COUNT", "HW_PDM", "count"),
    ("ADC_COUNT", "HW_ADC", "count"),
    ("DAC_COUNT", "HW_DAC", "count"),
    ("CAN_COUNT", "HW_CAN", "count"),
    ("CAN_FD_SUPPORTED", "HW_CAN_FD", "bool"),
    ("RTC_COUNT", "HW_RTC", "count"),
    ("WDT_COUNT", "HW_WDT", "count"),
    ("QENC_COUNT", "HW_QENC", "count"),
    ("TIMER_COUNT", "HW_TIMER", "count"),
    ("PWM_COUNT", "HW_PWM", "count"),
    ("ETHERNET_COUNT", "HW_ETHERNET", "count"),
    ("USB_COUNT", "HW_USB", "count"),
    ("MIPI_CSI_COUNT", "HW_MIPI_CSI", "count"),
    ("MIPI_DSI_COUNT", "HW_MIPI_DSI", "count"),
    ("XSPI_DMA", "XSPI_DMA", "bool"),
    ("HEXSPI_DMA", "HEXSPI_DMA", "bool"),
    ("EMMC_DMA", "EMMC_DMA", "bool"),
    ("QUADSPI_DMA", "QUADSPI_DMA", "bool"),
    ("DRP_AI", "NPU_DRPAI", "bool"),
    ("HELIUM_MVE", "HELIUM_MVE", "bool"),
    ("NEON", "NEON", "bool"),
    ("GPU2D", "GPU2D", "bool"),
    ("DAVE2D", "DAVE2D", "bool"),
    ("CRYPTOCELL", "CRYPTOCELL", "bool"),
    ("INLINE_AES", "INLINE_AES", "bool"),
    ("CAU", "CAU", "bool"),
    ("DMA2D", "DMA2D", "bool"),
]


def _emit_cap_h() -> str:
    lines: list[str] = [
        "/**",
        " * @file cap.h",
        " * @brief Portable hardware-capability API (auto-generated).",
        " *",
        " * Auto-generated by scripts/gen_soc_caps.py from",
        " * metadata/socs/{vendor}/{family}/{part}.json. DO NOT EDIT BY HAND -- regenerate.",
        " *",
        " * Copyright 2026 ALP Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * @par ABI status: [ABI-STABLE]",
        " *      v0.7 generated; capability identifiers + lookup.",
        " *      See docs/abi-markers.md for the convention.",
        " */",
        "",
        "#ifndef ALP_CAP_H",
        "#define ALP_CAP_H",
        "",
        "#include <stdbool.h>",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "typedef enum {",
    ]
    for _soc, cap_name, _kind in CAP_ALIASES:
        lines.append(f"    ALP_CAP_ID_{cap_name},")
    lines.append("    ALP_CAP_ID_COUNT")
    lines.append("} alp_cap_id_t;")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Test whether the active SoC offers a hardware capability.")
    lines.append(" * @param cap  Capability id from @ref alp_cap_id_t.")
    lines.append(" * @return true if the capability is present, false otherwise.")
    lines.append(" */")
    lines.append("bool alp_has(alp_cap_id_t cap);")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Return the symbolic name of a capability (e.g. \"HW_I2C\").")
    lines.append(" * @param cap  Capability id; out-of-range returns NULL.")
    lines.append(" * @return Pointer to a static string, or NULL.")
    lines.append(" */")
    lines.append("const char *alp_cap_name(alp_cap_id_t cap);")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* ALP_CAP_H */")
    return "\n".join(lines) + "\n"


def _emit_cap_c() -> str:
    # Pre-align designated initialiser keys so clang-format's
    # AlignConsecutiveAssignments doesn't rewrite the generated file.
    designators = [f"[ALP_CAP_ID_{cap_name}]" for _soc, cap_name, _kind in CAP_ALIASES]
    width = max(len(d) for d in designators)

    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " * Auto-generated by scripts/gen_soc_caps.py.  DO NOT EDIT.",
        " */",
        "",
        "#include <stddef.h>",
        "#include <alp/cap.h>",
        "#include <alp/soc_caps.h>",
        "",
        "static const bool _cap_table[ALP_CAP_ID_COUNT] = {",
    ]
    for _soc, cap_name, _kind in CAP_ALIASES:
        key = f"[ALP_CAP_ID_{cap_name}]".ljust(width)
        lines.append(f"    {key} = ALP_CAP_{cap_name},")
    lines.append("};")
    lines.append("")
    lines.append("static const char *const _cap_names[ALP_CAP_ID_COUNT] = {")
    for _soc, cap_name, _kind in CAP_ALIASES:
        key = f"[ALP_CAP_ID_{cap_name}]".ljust(width)
        lines.append(f"    {key} = \"{cap_name}\",")
    lines.append("};")
    lines.append("")
    lines.append("bool alp_has(alp_cap_id_t cap)")
    lines.append("{")
    lines.append("    if ((unsigned)cap >= (unsigned)ALP_CAP_ID_COUNT) {")
    lines.append("        return false;")
    lines.append("    }")
    lines.append("    return _cap_table[cap];")
    lines.append("}")
    lines.append("")
    lines.append("const char *alp_cap_name(alp_cap_id_t cap)")
    lines.append("{")
    lines.append("    if ((unsigned)cap >= (unsigned)ALP_CAP_ID_COUNT) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    return _cap_names[cap];")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _emit_cap_layer(out: list[str]) -> None:
    out.append("")
    out.append("/* ---------------------------------------------------------------")
    out.append(" * Capability layer -- portable, SoM-agnostic.  Derived from the")
    out.append(" * active CONFIG_ALP_SOC_* selection via the macros above.")
    out.append(" *")
    out.append(" * Counts collapse to 0/1 via `> 0`, so ALP_HAS() is always a")
    out.append(" * constant expression and safe inside #if and static_assert.")
    out.append(" * --------------------------------------------------------------- */")
    for soc_name, cap_name, kind in CAP_ALIASES:
        if kind == "count":
            out.append(f"#define ALP_CAP_{cap_name} (ALP_SOC_{soc_name} > 0)")
        else:
            out.append(f"#define ALP_CAP_{cap_name} (ALP_SOC_{soc_name})")
    out.append("")
    out.append("#define ALP_HAS(cap) (ALP_CAP_##cap)")


def kconfig_token(ref: str) -> str:
    """`alif:ensemble:e7` → `ALIF_ENSEMBLE_E7`."""
    return ref.upper().replace(":", "_").replace("-", "_")


def extract_caps(soc: dict[str, Any]) -> dict[str, int]:
    p = soc.get("peripherals", {}) or {}
    return {name: int(fn(p)) for name, fn in CAPS}


def extract_bool_caps(soc: dict[str, Any]) -> dict[str, int]:
    """Extract boolean feature flags from ``soc["capabilities"]``.

    Returns a dict mapping the upper-cased flag name to 1 or 0.
    Absent keys default to 0 (feature not present on this SoC).
    """
    caps = soc.get("capabilities", {}) or {}
    return {key.upper(): (1 if caps.get(key) else 0) for key in BOOL_CAPS}


def emit() -> str:
    socs: list[tuple[str, str, dict[str, int], dict[str, int]]] = []
    for path in sorted(META_DIR.rglob("*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc["ref"]
        socs.append((ref, kconfig_token(ref), extract_caps(soc), extract_bool_caps(soc)))

    lines: list[str] = [
        "/**",
        " * @file soc_caps.h",
        " * @brief Per-SoC peripheral capability macros (auto-generated).",
        " *",
        # Avoid `/**` inside the C comment — gcc -Wcomment treats it as
        # a nested-comment opener.  Use `{vendor}/{family}/{part}.json`
        # with curly braces (not angle brackets) because Doxygen would
        # otherwise parse `<vendor>` as an unknown HTML tag and fail
        # the pr-doxygen WARN_AS_ERROR gate.
        " * Auto-generated from metadata/socs/{vendor}/{family}/{part}.json",
        " * by scripts/gen_soc_caps.py.  DO NOT EDIT BY HAND — regenerate.",
        " *",
        " * Each SoC's capability macros are gated by CONFIG_ALP_SOC_{TOKEN}.",
        " * Apps select the active SoC via Kconfig.  When no SoC is",
        " * selected the macros default to a permissive UINT16_MAX so",
        " * capability checks accept any config — apps that want runtime",
        " * validation must select a specific SoC.",
        " *",
        " * Copyright 2026 ALP Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * @par ABI status: [ABI-STABLE]",
        " *      v0.1 generated; capability constants.",
        " *      See docs/abi-markers.md for the convention.",
        " */",
        "",
        "#ifndef ALP_SOC_CAPS_H",
        "#define ALP_SOC_CAPS_H",
        "",
        "#include <stdint.h>",
        "",
    ]

    for i, (ref, kc, caps, bool_caps) in enumerate(socs):
        keyword = "if" if i == 0 else "elif"
        lines.append(f"#{keyword} defined(CONFIG_ALP_SOC_{kc})")
        lines.append(f"/* {ref} */")
        lines.append(f"#define ALP_SOC_REF_STR \"{ref}\"")
        for cap, _ in CAPS:
            lines.append(f"#define ALP_SOC_{cap} {caps[cap]}")
        for key in BOOL_CAPS:
            lines.append(f"#define ALP_SOC_{key.upper()} {bool_caps[key.upper()]}")
        lines.append("")

    lines.append("#else  /* No SoC selected — accept any config. */")
    lines.append("#define ALP_SOC_REF_STR \"unknown\"")
    for cap, _ in CAPS:
        lines.append(f"#define ALP_SOC_{cap} UINT16_MAX")
    for key in BOOL_CAPS:
        lines.append(f"#define ALP_SOC_{key.upper()} UINT16_MAX")
    lines.append("")
    lines.append("#endif")

    _emit_cap_layer(lines)

    lines.append("")
    lines.append("#endif  /* ALP_SOC_CAPS_H */")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    out_text = emit()
    OUT.write_text(out_text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)} ({len(out_text.splitlines())} lines)")

    cap_h_text = _emit_cap_h()
    CAP_H_OUT.write_text(cap_h_text, encoding="utf-8")
    print(f"wrote {CAP_H_OUT.relative_to(REPO)} ({len(cap_h_text.splitlines())} lines)")

    CAP_C_OUT.parent.mkdir(parents=True, exist_ok=True)
    cap_c_text = _emit_cap_c()
    CAP_C_OUT.write_text(cap_c_text, encoding="utf-8")
    print(f"wrote {CAP_C_OUT.relative_to(REPO)} ({len(cap_c_text.splitlines())} lines)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

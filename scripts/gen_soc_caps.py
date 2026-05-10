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


def kconfig_token(ref: str) -> str:
    """`alif:ensemble:e7` → `ALIF_ENSEMBLE_E7`."""
    return ref.upper().replace(":", "_").replace("-", "_")


def extract_caps(soc: dict[str, Any]) -> dict[str, int]:
    p = soc.get("peripherals", {}) or {}
    return {name: int(fn(p)) for name, fn in CAPS}


def emit() -> str:
    socs: list[tuple[str, str, dict[str, int]]] = []
    for path in sorted(META_DIR.rglob("*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc["ref"]
        socs.append((ref, kconfig_token(ref), extract_caps(soc)))

    lines: list[str] = [
        "/*",
        " * Copyright 2026 ALP Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * Auto-generated from metadata/socs/**.json by",
        " * scripts/gen_soc_caps.py.  DO NOT EDIT BY HAND — regenerate.",
        " *",
        " * Each SoC's capability macros are gated by CONFIG_ALP_SOC_<TOKEN>.",
        " * Apps select the active SoC via Kconfig.  When no SoC is",
        " * selected the macros default to a permissive UINT16_MAX so",
        " * capability checks accept any config — apps that want runtime",
        " * validation must select a specific SoC.",
        " */",
        "",
        "#ifndef ALP_SOC_CAPS_H",
        "#define ALP_SOC_CAPS_H",
        "",
        "#include <stdint.h>",
        "",
    ]

    for i, (ref, kc, caps) in enumerate(socs):
        keyword = "if" if i == 0 else "elif"
        lines.append(f"#{keyword} defined(CONFIG_ALP_SOC_{kc})")
        lines.append(f"/* {ref} */")
        lines.append(f"#define ALP_SOC_REF_STR \"{ref}\"")
        for cap, _ in CAPS:
            lines.append(f"#define ALP_SOC_{cap} {caps[cap]}")
        lines.append("")

    lines.append("#else  /* No SoC selected — accept any config. */")
    lines.append("#define ALP_SOC_REF_STR \"unknown\"")
    for cap, _ in CAPS:
        lines.append(f"#define ALP_SOC_{cap} UINT16_MAX")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif  /* ALP_SOC_CAPS_H */")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    out_text = emit()
    OUT.write_text(out_text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)} ({len(out_text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

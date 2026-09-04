#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate include/alp/soc_caps.h from metadata/socs/**/*.json.

Each SoC's capability macros are gated by CONFIG_ALP_SOC_<TOKEN>.
Apps select the active SoC via Kconfig (`CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y`
or similar); the SDK's `alp_*_open` functions consult the matching
`ALP_SOC_*` macros to reject configurations that exceed the SoC's
documented hardware caps.

Per-SKU granularity: a SoM preset (metadata/e1m_modules/<SKU>.yaml) may
declare `silicon_capabilities.unpopulated` -- silicon capabilities the SKU
leaves unpopulated.  For each such SKU this generator appends an
`ALP_SOM_<SKU>`-gated override block that forces the matching `ALP_SOC_*`
boolean macros to 0, so `ALP_HAS(...)` reflects the SKU, not just the
SoC family.  SKUs without the field emit nothing (full SoC capability
set -- the output is byte-identical to the pre-restriction generator).

Run:

    python3 scripts/gen_soc_caps.py

CI (when wired) regenerates the header on every PR that touches
metadata/socs/, then fails if the working tree diff is non-empty.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml

REPO = Path(__file__).resolve().parent.parent
META_DIR = REPO / "metadata" / "socs"
SOM_DIR = REPO / "metadata" / "e1m_modules"
OUT = REPO / "include" / "alp" / "soc_caps.h"
CAP_H_OUT = REPO / "include" / "alp" / "cap.h"
CAP_C_OUT = REPO / "src" / "cap.c"

# Capability fields we extract from each SoC's metadata.
# (field_name, lambda: derives the integer value from peripherals dict)
CAPS: list[tuple[str, callable]] = [
    ("I2C_COUNT",
        lambda p: (p.get("i2c", 0) or 0) + (p.get("i2c_lp", 0) or 0)),
    ("I3C_COUNT",
        lambda p: (p.get("i3c", 0) or 0) + (p.get("i3c_lp", 0) or 0)),
    # SPI and UART stay EXACT on purpose (#1304 asked for the per-field
    # judgement to be written down rather than left to the next reader).
    #
    # These are `alp_spi_open()` / `alp_uart_open()` ADDRESSABLE-INSTANCE
    # counts, not "does the part have anything SPI-shaped" predicates, and
    # `src/backends/**` bounds-check channel ids against them. The nearby keys
    # a prefix sum would swallow are NOT addressable through those APIs:
    #
    #   * `qspi` (deepx:dx:m1, 1) is a memory-mapped execute-in-place flash
    #     controller, reached as storage, never as an `alp_spi_open()` bus.
    #   * `scif` (renesas:rzv2n:n44, 1) is the Renesas boot/debug serial
    #     interface; the 10 `uart` instances are the addressable ones.
    #
    # Counting either would let a caller open an id that no backend can
    # serve -- the mirror image of the zero-count bug fixed above, and worse,
    # because it fails at runtime rather than at build time.
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
    # Every `timer*` instance key, however the vendor spells the family
    # (#1304).  Exact-key matching read `timer_32bit` + `timer_lp` only, so a
    # part naming its families differently counted ZERO and the derived
    # `ALP_CAP_HW_TIMER` came out FALSE on silicon that has timers:
    # `renesas:rzv2n:n44` declares `timer_32bit_gpt` 16 + `timer_32bit_cmtw` 8
    # + `timer_32bit_gtm` 8 and emitted 0; `deepx:dx:m1` declares
    # `timer_general` 3 and emitted 0; the three Alif parts carrying
    # `timer_lp_32bit` 3 (e4/e6/e8) emitted 16 instead of 19.  Same defect
    # class as #1240's `ethernet_1g`, one field over.
    #
    # Prefix-summing is what `gen_support_matrix.py` already does for the same
    # metadata (`_has_prefix(s, "timer_")`) -- two generators over one metadata
    # tree that answered differently.  #1304 also asks for a gate that fails
    # when the two disagree; that is NOT in this change and #1304 stays open
    # for it.  Until it exists, a new vendor key spelling can split them again.
    ("TIMER_COUNT",
        lambda p: sum(int(v) for k, v in p.items()
                      if k.startswith("timer") and isinstance(v, int))),
    ("PWM_COUNT",
        # DELIBERATELY exact, NOT prefix-summed like TIMER_COUNT above.
        #
        # PWM channels come off general-purpose timers, so this falls back to
        # `timer_32bit` where the part has no explicit `pwm` key.  Prefix-
        # summing here would be actively wrong twice over:
        #
        #   * On `renesas:rzv2n:n44` it would take PWM_COUNT from 0 to 32, but
        #     ADR 0024 records that V2N/V2M PWM is served EXCLUSIVELY by the
        #     GD32 bridge -- no native leg, because no SoC pin reaches an
        #     E1M-standard PWM pad.  Zero is the correct native count there.
        #   * A v0.16.0 sweep tried the opposite simplification,
        #     `p.get("pwm", 0)`, which took PWM_COUNT from 12 to 0 on all six
        #     `alif:ensemble:e3..e8` parts.  `src/backends/pwm/zephyr_drv.c`
        #     refuses `channel_id >= ALP_SOC_PWM_COUNT`, so that would have
        #     made `alp_pwm_open()` return ALP_ERR_OUT_OF_RANGE for every
        #     channel on every E1M-AEN SKU -- on silicon where PWM is bench
        #     PASS (docs/aen-bench-bringup.md) and GA (docs/os-support-matrix.md).
        #
        # Leave it exact until a part declares real PWM instances (#1304).
        lambda p: p.get("pwm", p.get("timer_32bit", 0) or 0)),
    ("ETHERNET_COUNT",
        lambda p: (p.get("ethernet", 0) or 0) + (p.get("ethernet_1g", 0) or 0)),
    # Every `usb*` controller key (#1304).  `usb_2` + `usb_3` missed
    # `renesas:rzv2n:n44`'s `usb_3_2_gen2` (counted 1, has 2) and
    # `deepx:dx:m1`'s `usb_2_otg` (counted 0, has 1).
    ("USB_COUNT",
        lambda p: sum(int(v) for k, v in p.items()
                      if k.startswith("usb") and isinstance(v, int))),
    ("MIPI_CSI_COUNT",
        lambda p: p.get("mipi_csi2", 0) or 0),
    ("MIPI_DSI_COUNT",
        lambda p: p.get("mipi_dsi", 0) or 0),
    # LCDIF / parallel-RGB display controller, distinct from mipi_dsi (issue
    # #379).  imx93 has 1x nxp,imx-lcdifv3; E8 models its parallel display path
    # as `dpi_parallel` (a different interface), so this key stays LCDIF-specific.
    ("LCDIF_COUNT",
        lambda p: p.get("lcdif", 0) or 0),
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
    ("LCDIF_COUNT", "HW_LCDIF", "count"),
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
    # APPENDED, not grouped with the other *_COUNT rows above, on purpose:
    # ALP_CAP_ID_* are implicit enum values, so this list's ORDER is their
    # numbering, and those IDs are recorded in docs/abi/v*-snapshot.json.
    # Inserting I3C_COUNT next to I2C_COUNT where it belongs alphabetically
    # would renumber every ALP_CAP_ID_* after it -- an ABI change far larger
    # than the one symbol being added. New capabilities go at the END.
    ("I3C_COUNT", "HW_I3C", "count"),
]


def _emit_cap_h() -> str:
    lines: list[str] = [
        "/**",
        " * @file cap.h",
        " * @brief Umbrella header for the ALP capability surface.",
        " *",
        " * Aggregates the SoC-level macros (from soc_caps.h) and the",
        " * instance-level types (from cap_instance.h) with the portable",
        " * runtime alp_has() / alp_cap_name() API into one include.",
        " *",
        " * Auto-generated by scripts/gen_soc_caps.py from",
        " * metadata/socs/{vendor}/{family}/{part}.json. DO NOT EDIT BY HAND --",
        " * regenerate with: python3 scripts/gen_soc_caps.py",
        " *",
        " * Copyright 2026 Alp Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * @par ABI status: [ABI-EXPERIMENTAL]",
        " *      Includes cap_instance.h which is v0.7 new and experimental.",
        " *      Marker is upgraded to [ABI-STABLE] when the underlying",
        " *      pieces stabilise.",
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
        '#include "soc_caps.h"',
        '#include "cap_instance.h"',
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


def _emit_aligned_defines(out: list[str], defs: list[tuple[str, str]]) -> None:
    """Emit ``#define NAME VALUE`` with the value column aligned.

    Pads each macro name to the widest in the (consecutive, blank-line-bounded)
    block so the value column matches what clang-format's
    ``AlignConsecutiveMacros: Consecutive`` produces.  This keeps the generated
    header clang-clean without requiring a clang-format pass at generation time
    (same pre-alignment approach already used for the capability table below).
    """
    if not defs:
        return
    width = max(len(name) for name, _ in defs)
    for name, value in defs:
        out.append(f"#define {name:<{width}} {value}")


def _emit_cap_layer(out: list[str]) -> None:
    out.append("")
    out.append("/* ---------------------------------------------------------------")
    out.append(" * Capability layer -- portable, SoM-agnostic.  Derived from the")
    out.append(" * active CONFIG_ALP_SOC_* selection via the macros above.")
    out.append(" *")
    out.append(" * Counts collapse to 0/1 via `> 0`, so ALP_HAS() is always a")
    out.append(" * constant expression and safe inside #if and static_assert.")
    out.append(" * --------------------------------------------------------------- */")
    cap_defs: list[tuple[str, str]] = []
    for soc_name, cap_name, kind in CAP_ALIASES:
        if kind == "count":
            cap_defs.append((f"ALP_CAP_{cap_name}", f"(ALP_SOC_{soc_name} > 0)"))
        else:
            cap_defs.append((f"ALP_CAP_{cap_name}", f"(ALP_SOC_{soc_name})"))
    _emit_aligned_defines(out, cap_defs)
    out.append("")
    out.append("#define ALP_HAS(cap) (ALP_CAP_##cap)")


def kconfig_token(ref: str) -> str:
    """`alif:ensemble:e7` → `ALIF_ENSEMBLE_E7`."""
    return ref.upper().replace(":", "_").replace("-", "_")


def som_token(sku: str) -> str:
    """`E1M-AEN801` → `E1M_AEN801` (the ALP_SOM_* macro suffix).

    Mirrors alp_orchestrate.slugs._board_define_slug's transform so the
    `-DALP_SOM_<TOKEN>` define the emitters pass matches this header's gate.
    """
    return sku.upper().replace("-", "_")


def load_som_restrictions(som_dir: Path) -> list[tuple[str, list[str]]]:
    """Collect per-SKU `silicon_capabilities.unpopulated` lists.

    Returns [(sku, [cap_name, ...]), ...] sorted by SKU, containing ONLY
    SKUs that declare a non-empty restriction list.  Today no SKU does,
    so the returned list is empty and emit() output is unchanged.
    Validation of the names (must exist truthy in the referenced SoC's
    `capabilities:` block) is scripts/validate_metadata.py's job; this
    loader just transcribes.
    """
    restrictions: list[tuple[str, list[str]]] = []
    if not som_dir.is_dir():
        return restrictions
    for path in sorted(som_dir.glob("E1M-*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(doc, dict):
            continue
        block = doc.get("silicon_capabilities") or {}
        names = block.get("unpopulated") or [] if isinstance(block, dict) else []
        if names:
            restrictions.append((str(doc.get("sku", path.stem)),
                                 [str(n) for n in names]))
    return restrictions


def _emit_som_restrictions(out: list[str], restrictions: list[tuple[str, list[str]]]) -> None:
    """Emit ALP_SOM_<SKU>-gated ALP_SOC_* overrides for restricted SKUs.

    Only capability names that materialise as ALP_SOC_* boolean macros
    (BOOL_CAPS) get an override here; count-style silicon capabilities
    (e.g. `ethos_u55_count`) have no ALP_SOC_* macro and act through the
    loader layer only (resolve_capabilities() in scripts/alp_project.py) --
    they are surfaced as a comment so the header stays self-explaining.
    Emits nothing when no SKU is restricted, keeping the header
    byte-identical for the no-restriction (= current) catalogue.
    """
    if not restrictions:
        return
    out.append("")
    out.append("/* ---------------------------------------------------------------")
    out.append(" * Per-SKU capability restrictions -- from the SoM preset's")
    out.append(" * `silicon_capabilities.unpopulated` (metadata/e1m_modules/<SKU>.yaml).")
    out.append(" * A SKU can only NARROW its SoC's capability set, never extend it.")
    out.append(" * The build system defines ALP_SOM_<SKU> for restricted SKUs")
    out.append(" * (scripts/alp_orchestrate/kconfig.py); builds without the define")
    out.append(" * keep the full SoC capability set.")
    out.append(" * --------------------------------------------------------------- */")
    for sku, names in restrictions:
        out.append(f"#if defined(ALP_SOM_{som_token(sku)})")
        out.append(f"/* {sku}: unpopulated on this SKU. */")
        loader_only: list[str] = []
        for name in names:
            if name in BOOL_CAPS:
                out.append(f"#undef ALP_SOC_{name.upper()}")
                out.append(f"#define ALP_SOC_{name.upper()} 0")
            else:
                loader_only.append(name)
        if loader_only:
            out.append(f"/* Loader-layer only (no ALP_SOC_* macro): "
                       f"{', '.join(loader_only)}. */")
        out.append("#endif")


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


def extract_unverified_peripherals(soc: dict[str, Any]) -> list[str]:
    """Peripheral keys on this SoC whose count has no primary-source citation.

    #936: an audited-but-incomplete file lists its gaps in
    `peripherals_unverified` (e.g. `pdm`/`pdm_lp`, uncited on every Alif
    Ensemble part).  A file whose peripherals block was never independently
    ingested for this part at all (`pending_reference_manual_ingestion: true`,
    e.g. E5 inheriting from E7) is treated as ALL of its `peripherals` keys
    being unverified, so the header doesn't understate the gap -- UNLESS the
    file carries its own `peripherals_unverified` (even `[]`), which means
    the file itself already grounds its populated keys individually (e.g.
    i.MX93: `pending_reference_manual_ingestion` covers the still-zero rest
    of the block, but `mipi_dsi`/`lcdif` are cited in `notes` and so are
    correctly declared with `peripherals_unverified: []`).  An explicit
    per-file list always wins over the wholesale fallback.
    """
    if "peripherals_unverified" in soc:
        return sorted(str(k) for k in (soc.get("peripherals_unverified") or []))
    if soc.get("pending_reference_manual_ingestion"):
        return sorted((soc.get("peripherals") or {}).keys())
    return []


def emit(meta_dir: Path = META_DIR, som_dir: Path = SOM_DIR) -> str:
    socs: list[tuple[str, str, dict[str, int], dict[str, int], int, list[str]]] = []
    for path in sorted(meta_dir.rglob("*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc["ref"]
        # 0 on every real SoC today, deliberately -- this is an
        # integration/partition decision, not a datasheet constant any
        # vendor publishes (issue #1731 investigation).  Do not "fix" this
        # by filling in a plausible-looking number here; see
        # metadata/schemas/soc-spec-v1.schema.json's field description and
        # src/backends/inference/alp_model_select.c's _fits().
        arena_kib = int(soc.get("inference_arena_sram_kib", 0))
        socs.append((ref, kconfig_token(ref), extract_caps(soc), extract_bool_caps(soc),
                     arena_kib, extract_unverified_peripherals(soc)))

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
        " * Copyright 2026 Alp Lab AB",
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

    for i, (ref, kc, caps, bool_caps, arena_kib, unverified) in enumerate(socs):
        keyword = "if" if i == 0 else "elif"
        lines.append(f"#{keyword} defined(CONFIG_ALP_SOC_{kc})")
        lines.append(f"/* {ref} */")
        if unverified:
            # #936: this SoC's own metadata file has no datasheet/DFP/HWRM
            # citation backing the COUNT for these `peripherals` keys -- the
            # ALP_SOC_*_COUNT macros derived from them (and thus ALP_HAS())
            # are asserted, not confirmed.  This says nothing about whether
            # the peripheral's mere EXISTENCE is cited (e.g. E4's pdm/pdm_lp:
            # the instance is DFP-confirmed, only the channel count isn't --
            # see `peripherals_unverified` in the source JSON under
            # metadata/socs/).
            lines.append(f"/* UNVERIFIED (count not backed by a primary source): {', '.join(unverified)} */")
        soc_defs: list[tuple[str, str]] = [("ALP_SOC_REF_STR", f"\"{ref}\"")]
        soc_defs += [(f"ALP_SOC_{cap}", str(caps[cap])) for cap, _ in CAPS]
        soc_defs += [(f"ALP_SOC_{key.upper()}", str(bool_caps[key.upper()])) for key in BOOL_CAPS]
        soc_defs.append(("ALP_SOC_NPU_ARENA_SRAM_KIB", str(arena_kib)))
        _emit_aligned_defines(lines, soc_defs)
        lines.append("")

    lines.append("#else /* No SoC selected — accept any config. */")
    else_defs: list[tuple[str, str]] = [("ALP_SOC_REF_STR", "\"unknown\"")]
    else_defs += [(f"ALP_SOC_{cap}", "UINT16_MAX") for cap, _ in CAPS]
    else_defs += [(f"ALP_SOC_{key.upper()}", "UINT16_MAX") for key in BOOL_CAPS]
    else_defs.append(("ALP_SOC_NPU_ARENA_SRAM_KIB", "UINT16_MAX"))
    _emit_aligned_defines(lines, else_defs)
    lines.append("")
    lines.append("#endif")

    # Per-SKU restriction overrides sit between the per-SoC blocks and the
    # ALP_CAP_* layer so a restricted SKU's build resolves ALP_HAS() against
    # the narrowed set.  No restricted SKU (the current catalogue) = no output.
    _emit_som_restrictions(lines, load_som_restrictions(som_dir))

    _emit_cap_layer(lines)

    lines.append("")
    lines.append("#endif /* ALP_SOC_CAPS_H */")
    lines.append("")

    return "\n".join(lines)


def _clang_format_exe() -> str:
    """Resolve the pinned clang-format binary, or fail naming what's missing.

    Pinned to clang-format-22 (the CI version; installed via the pip wheel
    `clang-format==22.*`, which provides the unsuffixed `clang-format`).
    Prefer a v22-named binary if present, else the pinned `clang-format`.

    Formerly this degraded to a warning and left the file unformatted --
    the caller (test-all.sh's generated-files gate) then diffed raw,
    pre-aligned emitter output against the clang-formatted files already
    committed and reported that as tabs-vs-spaces "drift" (alp-sdk#1109),
    when the real problem was clang-format never having run at all.
    """
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if exe is None:
        # Exit 99, not the default 1: this is a missing-tool refusal, not
        # a generator failure, and test-all.sh's stage_generated_files
        # maps 99 to SKIP (same contract as every other prerequisite
        # check in that script) rather than FAIL (alp-sdk#1221).
        print(
            "error: clang-format not found on PATH; cannot format the "
            "generated SoC-caps files to match the repo .clang-format "
            "(install clang-format==22.* -- see docs/testing.md)",
            file=sys.stderr,
        )
        raise SystemExit(99)
    return exe


def _clang_format(path: Path, exe: str) -> None:
    """Format a generated file in place to match the repo .clang-format.

    The emitters above produce best-effort, pre-aligned output, but the canonical
    formatting (tab indentation, AlignConsecutive* columns) is owned by
    /.clang-format -- so run clang-format here to guarantee the generated file is
    byte-identical to what the CI diff-only gate expects, regardless of how this
    script lays out whitespace.
    """
    subprocess.run([exe, "-i", "--style=file", str(path)], check=True)


def main() -> int:
    exe = _clang_format_exe()

    OUT.parent.mkdir(parents=True, exist_ok=True)
    out_text = emit()
    OUT.write_text(out_text, encoding="utf-8", newline="")
    _clang_format(OUT, exe)
    print(f"wrote {OUT.relative_to(REPO)} ({len(OUT.read_text().splitlines())} lines)")

    cap_h_text = _emit_cap_h()
    CAP_H_OUT.write_text(cap_h_text, encoding="utf-8", newline="")
    _clang_format(CAP_H_OUT, exe)
    print(f"wrote {CAP_H_OUT.relative_to(REPO)} ({len(CAP_H_OUT.read_text().splitlines())} lines)")

    CAP_C_OUT.parent.mkdir(parents=True, exist_ok=True)
    cap_c_text = _emit_cap_c()
    CAP_C_OUT.write_text(cap_c_text, encoding="utf-8", newline="")
    _clang_format(CAP_C_OUT, exe)
    print(f"wrote {CAP_C_OUT.relative_to(REPO)} ({len(CAP_C_OUT.read_text().splitlines())} lines)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

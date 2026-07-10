#!/usr/bin/env python3
# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
`--emit zephyr-board` -- generate the per-core Zephyr board tree
(`board.yml`, `Kconfig.alp_<board>`, the twister board metadata `.yaml`,
and -- where the underlying hardware facts already exist in metadata --
the `_defconfig`, pinctrl `.dtsi`, `Kconfig.defconfig`, and board `.dts`)
straight from the SoM preset + SoC JSON, instead of hand-authoring them
under `zephyr/boards/alp/<board>/` (issue #523).

Wired into `scripts/alp_project.py` as:

    python3 scripts/alp_project.py --input <board.yaml> --core <core_id> \\
        --emit zephyr-board --output build/boards/<board_dir>/

Unlike every other `--emit` mode this one writes a *directory* of files,
not a single stream -- `alp_project.py` handles that distinction; this
module only returns ``{relative_path: content}`` (paths relative to the
board-tree root, e.g. ``"alp_e1m_aen801_m55_hp/board.yml"``).

Scope (issue #523, first slice):

  - Alif Ensemble (the `aen` family, e.g. E1M-AEN801 m55_hp/m55_he) is
    fully generated: every file in the hand-authored board tree except
    `board.cmake` (see NOT GENERATED below) is produced here,
    byte-identical to the committed tree -- proven by
    `tests/scripts/test_gen_zephyr_board.py`.
  - Renesas RZ/V2N-family boards (`v2n` / `v2n-m1`, e.g. E1M-V2N101,
    E1M-V2M101 `m33_sm`) generate only the family-agnostic files
    (`board.yml`, `Kconfig.alp_<board>`, the twister `.yaml`).  Their
    `_defconfig` / pinctrl `.dtsi` / board `.dts` stay hand-authored:
    the Renesas-side pin assignments for the on-module GD32G553
    supervisor bridge (SCI7 Simple-SPI, RIIC8/BRD_I2C, the disabled
    SCI0 console) are not yet captured anywhere in
    `metadata/e1m_modules/*.yaml` or `metadata/pinmux/*.yaml` -- only
    the E1M-X *application*-facing `pad_routes:` (which GD32 pin an
    `E1M_X_*` app pad dispatches to) are.  Per the project's
    no-inventing-values rule this generator does not guess those
    SoM-internal Renesas pin numbers; a future slice adds a
    `metadata/pinmux/<family>-internal.yaml` (or similar) source and
    extends this module.

  NOT GENERATED (any family): `board.cmake`.  The hand-authored AEN
  `board.cmake` pair (HP/HE) intentionally carries asymmetric prose --
  one board's file hosts the full SETOOLS/J-Link bench-bring-up
  runbook, the sibling's just points at it -- which is a documentation
  choice, not a hardware fact, so there is nothing in metadata for it
  to derive from.  Flasher/debugger `board_runner_args()` wiring stays
  hand-authored until that asymmetry is either resolved (duplicate the
  full rationale in both) or the prose itself is promoted into a
  metadata field.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from alp_project_loader import _load_yaml, _resolve_sku

_COPYRIGHT_C = (
    "/*\n"
    " * Copyright (c) 2026 Alp Lab AB\n"
    " * SPDX-License-Identifier: Apache-2.0\n"
)
_COPYRIGHT_HASH = (
    "# Copyright (c) 2026 Alp Lab AB\n"
    "# SPDX-License-Identifier: Apache-2.0\n"
)


class ZephyrBoardEmitError(ValueError):
    """Raised when metadata needed for a requested board is missing."""


# ---------------------------------------------------------------------
# Family-agnostic naming + resolution
# ---------------------------------------------------------------------


def _load_soc_spec(sku_preset: dict[str, Any], metadata_root: Path) -> dict[str, Any]:
    silicon = sku_preset.get("silicon")
    if not silicon:
        raise ZephyrBoardEmitError(f"SoM {sku_preset.get('sku')!r} declares no silicon:")
    parts = silicon.split(":")
    if len(parts) != 3:
        raise ZephyrBoardEmitError(f"silicon ref {silicon!r} is not a triple-colon string")
    soc_path = metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"
    if not soc_path.is_file():
        raise ZephyrBoardEmitError(f"no SoC spec at {soc_path}")
    return json.loads(soc_path.read_text(encoding="utf-8"))


def _resolve_variant(sku_preset: dict[str, Any], soc_spec: dict[str, Any]) -> dict[str, Any]:
    declared = sku_preset.get("silicon_variant")
    variants = soc_spec.get("variants") or []
    if declared and declared != "TBD":
        for v in variants:
            if v.get("order_code") == declared:
                return v
    sku = sku_preset.get("sku")
    for v in variants:
        if sku in (v.get("alp_module_skus") or []):
            return v
    raise ZephyrBoardEmitError(
        f"can't resolve a silicon_variant for {sku_preset.get('sku')!r}")


def _find_core(soc_spec: dict[str, Any], core_id: str) -> dict[str, Any]:
    for c in soc_spec.get("cores", []):
        if c.get("id") == core_id:
            return c
    raise ZephyrBoardEmitError(
        f"core {core_id!r} not found in {soc_spec.get('ref')} cores[]")


def _board_dir_name(sku: str, core_id: str) -> str:
    """`E1M-AEN801` + `m55_hp` -> `alp_e1m_aen801_m55_hp`."""
    if not sku.startswith("E1M-"):
        raise ZephyrBoardEmitError(f"unrecognised SKU prefix: {sku!r}")
    slug = sku[len("E1M-"):].lower()
    return f"alp_e1m_{slug}_{core_id}"


def _board_naming(
    sku_preset: dict[str, Any], core_id: str, variant: dict[str, Any], core: dict[str, Any],
) -> tuple[str, str, str, str]:
    """Return (dir_name, cluster, variant_tag, basename)."""
    dir_name = _board_dir_name(sku_preset["sku"], core_id)
    cluster = core.get("zephyr_cpucluster")
    if not cluster:
        raise ZephyrBoardEmitError(
            f"core {core_id!r} has no zephyr_cpucluster in its SoC spec entry -- "
            "add one before generating this board (see soc-spec-v1.schema.json)")
    variant_tag = (variant.get("zephyr_soc_variant") or variant["order_code"]).lower()
    basename = f"{dir_name}_{variant_tag}_{cluster}"
    return dir_name, cluster, variant_tag, basename


def _topology_entry(sku_preset: dict[str, Any], core_id: str) -> dict[str, Any]:
    topo = (sku_preset.get("topology") or {}).get(core_id)
    if not topo:
        raise ZephyrBoardEmitError(
            f"SoM {sku_preset.get('sku')!r} has no topology.{core_id} entry")
    return topo


def _generated_banner(style: str, sku: str, soc_json_rel: str) -> list[str]:
    """3-line "auto-generated, do not edit" banner, matching the wording
    convention `gen_soc_caps.py` uses for `include/alp/soc_caps.h`."""
    c = " *" if style == "c" else "#"
    sku_rel = f"metadata/e1m_modules/{sku}.yaml"
    return [
        f"{c} Auto-generated by `scripts/alp_project.py --emit zephyr-board` from",
        f"{c} {sku_rel} + {soc_json_rel}.",
        f"{c} DO NOT EDIT BY HAND -- regenerate.",
    ]


def _with_generated_banner(content: str, style: str, sku: str, soc_json_rel: str) -> str:
    """Insert the generated-file banner into *content*.

    - When *content* already opens with the standard Copyright/SPDX
      header (every AEN file; the V2N-family Kconfig/twister files),
      the banner is inserted right after the SPDX line, ahead of the
      file's own prose -- purely additive, no existing line moves or
      disappears.
    - When *content* has no header at all (the V2N-family `board.yml`,
      matching its committed hand-authored original -- see `_board_yml`),
      the banner is prepended as a leading comment block instead.
    """
    lines = content.split("\n")
    banner = _generated_banner(style, sku, soc_json_rel)
    if style == "c" and len(lines) >= 3 and lines[0] == "/*" and lines[1].startswith(" * Copyright"):
        lines[3:3] = banner
        return "\n".join(lines)
    if style == "hash" and lines and lines[0].startswith("# Copyright"):
        lines[2:2] = banner
        return "\n".join(lines)
    # No pre-existing header: prepend the banner as its own block.
    return "\n".join(banner) + "\n" + content


def _sku_family_slug(sku: str) -> str:
    """AEN/V2N/V2M -- see alp_project_loader._sku_family (kept independent
    here so this module has no import-cycle risk on alp_project_loader's
    fuller SKU-family table; the two must agree, pinned by the test)."""
    m = re.match(r"^E1M-(AEN|V2N|V2M|NX9)", sku)
    if not m:
        raise ZephyrBoardEmitError(f"unrecognised SoM SKU pattern: {sku!r}")
    return {"AEN": "aen", "V2N": "v2n", "V2M": "v2n-m1", "NX9": "imx93"}[m.group(1)]


# ---------------------------------------------------------------------
# Family-agnostic files: board.yml, Kconfig.alp_<board>, twister .yaml
# ---------------------------------------------------------------------


def _board_yml(dir_name: str, full_name: str, variant_tag: str, family: str) -> str:
    # Hand-authored inconsistency across families: AEN's board.yml carries
    # the copyright header, the V2N-family one doesn't.  Preserved exactly
    # rather than "fixed", per the byte-equivalence pin -- see the
    # `--emit zephyr-board` module docstring.
    header = _COPYRIGHT_HASH if family == "aen" else ""
    return (
        header +
        "board:\n"
        f"  name: {dir_name}\n"
        f"  full_name: {full_name}\n"
        "  vendor: alp\n"
        "  socs:\n"
        f"    - name: {variant_tag}\n"
    )


def _kconfig_board(dir_name: str, variant_tag: str, cluster: str) -> str:
    soc_sym = f"SOC_{variant_tag.upper()}_{cluster.upper()}"
    board_sym = dir_name.upper()
    return (
        _COPYRIGHT_HASH +
        "\n"
        f"config BOARD_{board_sym}\n"
        f"\tselect {soc_sym}\n"
    )


def _aen_dtcm_kib(variant: dict[str, Any], core_id: str) -> int:
    role = core_id.split("_")[-1].upper()  # "m55_hp" -> "HP"
    banks = variant.get("sram_banks_kb") or {}
    for name, size in banks.items():
        if name.endswith(f"_{role}_DTCM"):
            return int(size)
    raise ZephyrBoardEmitError(
        f"variant {variant.get('order_code')} sram_banks_kb has no *_{role}_DTCM entry")


def _twister_yaml(
    dir_name: str, variant_tag: str, cluster: str, twister_name: str,
    family: str, sku_preset: dict[str, Any], core_id: str, variant: dict[str, Any],
) -> str:
    identifier = f"{dir_name}/{variant_tag}/{cluster}"
    has_supervisor_mcu = bool((sku_preset.get("on_module") or {}).get("supervisor_mcu"))

    if family == "aen":
        ram_kib = _aen_dtcm_kib(variant, core_id)
        flash_kib = round(float(variant["mram_mb"]) * 1024)
        return (
            f"identifier: {identifier}\n"
            f"name: {twister_name}\n"
            "type: mcu\n"
            "arch: arm\n"
            "vendor: alp\n"
            "toolchain:\n"
            "  - zephyr\n"
            "  - gnuarmemb\n"
            "supported:\n"
            "  - uart\n"
            f"ram: {ram_kib}\n"
            f"flash: {flash_kib}\n"
        )

    # Non-AEN families: no on-die MRAM/TCM concept surfaced here (the A55
    # cluster boots Linux from eMMC/xSPI; the M33 board file has no
    # zephyr,flash of its own), so no ram:/flash:.  supported: reflects
    # the on-module supervisor-MCU bridge (SPI + I2C + a GPIO chip-select)
    # plus the always-declared (possibly disabled) console UART -- the
    # shape shared by every current V2N-family SoM (same PCB, same GD32
    # bridge; see the porting-a-new-som "same-PCB families" rule).
    supported = ["gpio", "i2c", "spi", "uart"] if has_supervisor_mcu else ["uart"]
    lines = [
        f"identifier: {identifier}",
        f"name: {twister_name}",
        "type: mcu",
        "arch: arm",
        "toolchain:",
        "  - zephyr",
        "  - gnuarmemb",
        "supported:",
    ]
    lines += [f"  - {p}" for p in supported]
    lines.append("vendor: alp")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# Alif Ensemble (aen) family -- fully generated
# ---------------------------------------------------------------------

# Fixed MCUboot flash-partition sizes for the AEN sysbuild profile
# (MCUboot + a signed application image, swap-using-scratch).  These are
# an SDK build-policy choice, not a per-chip hardware fact -- every AEN
# SKU's on-die MRAM is split the same way regardless of total size, so
# they stay generator constants rather than metadata fields; only the
# two symmetric image slots derive from the variant's actual MRAM size.
_AEN_MCUBOOT_KIB = 64
_AEN_SCRATCH_KIB = 64
_AEN_STORAGE_KIB = 128

# Per-role documentation asymmetries in the committed AEN board tree.
# These are prose choices (which sibling's ITCM comment got the
# bench-verified annotation), not hardware facts -- like the comment
# scaffolding in alp_project_emit._emit_dts_overlay / _emit_hw_info_h,
# they live as generator template text keyed by role rather than as
# metadata.
_AEN_BENCH_KNOWN_ITCM = {"hp": False, "he": True}

# Vendor-short + family + part, as used in AEN board-tree prose ("Alif
# Ensemble E8").  soc_spec's own `vendor:` field spells out "Alif
# Semiconductor" (the legal vendor name for provenance blocks), so this
# short form isn't mechanically derivable from it; kept as a small
# per-family generator constant like _AEN_MCUBOOT_KIB above.
_AEN_FAMILY_DISPLAY = "Alif Ensemble E8"


def _aen_flash_partitions(total_kib: int) -> "list[tuple[str, int, int]]":
    """Return [(label, offset_bytes, size_kib), ...] for the AEN layout."""
    reserved = _AEN_MCUBOOT_KIB + _AEN_SCRATCH_KIB + _AEN_STORAGE_KIB
    remaining = total_kib - reserved
    if remaining <= 0 or remaining % 2:
        raise ZephyrBoardEmitError(
            f"AEN MRAM size {total_kib} KiB doesn't split evenly into two "
            f"image slots after reserving {reserved} KiB for "
            "mcuboot/scratch/storage")
    image_kib = remaining // 2

    offset = 0
    out: list[tuple[str, int, int]] = []
    for label, size_kib in (
        ("mcuboot", _AEN_MCUBOOT_KIB),
        ("image-0", image_kib),
        ("image-1", image_kib),
        ("image-scratch", _AEN_SCRATCH_KIB),
        ("storage", _AEN_STORAGE_KIB),
    ):
        out.append((label, offset, size_kib))
        offset += size_kib * 1024
    if offset != total_kib * 1024:
        raise ZephyrBoardEmitError("AEN flash partition arithmetic didn't sum to total")
    return out


def _aen_console_pinmux_rows(metadata_root: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    """Return (rx_row, tx_row) for the E1M `UART0` pad from `pinmux/aen.yaml`."""
    pm = _load_yaml(metadata_root / "pinmux" / "aen.yaml")
    rows = {r.get("e1m_function"): r for r in (pm.get("pads") or [])
            if r.get("e1m_function") in ("UART0_TX", "UART0_RX")}
    if "UART0_RX" not in rows or "UART0_TX" not in rows:
        raise ZephyrBoardEmitError(
            "metadata/pinmux/aen.yaml has no UART0_TX/UART0_RX rows for the "
            "E1M console pad")
    return rows["UART0_RX"], rows["UART0_TX"]


def _pin_macro(row: dict[str, Any]) -> str:
    return f"PIN_{row['silicon_pad']}__{row['silicon_peripheral']}"


def _uart_node_label(row: dict[str, Any]) -> str:
    m = re.match(r"^([A-Za-z]+\d+)", row["silicon_peripheral"])
    if not m:
        raise ZephyrBoardEmitError(
            "can't derive a UART node label from silicon_peripheral "
            f"{row['silicon_peripheral']!r}")
    return m.group(1).lower()


def _aen_pinctrl_dtsi(
    role: str, sku_display: str, rx_row: dict[str, Any], tx_row: dict[str, Any],
) -> str:
    other_role = "he" if role == "hp" else "hp"
    rx_macro, tx_macro = _pin_macro(rx_row), _pin_macro(tx_row)
    uart_node = _uart_node_label(rx_row)

    if role == "hp":
        confirm_block = (
            f" * (In an {other_role.upper()}+{role.upper()} AMP image the two cores share this single physical console;\n"
            " * during bring-up each core is flashed/run independently.)  Confirmed against\n"
            " * the SoM netlist + metadata/e1m_modules/aen/from-alif.tsv.\n"
        )
    else:
        confirm_block = (
            " * Confirmed against the SoM netlist + metadata/e1m_modules/aen/from-alif.tsv\n"
            f" * (rows {tx_row['e1m_pad']} = {tx_row['e1m_function']} = "
            f"{tx_row['silicon_peripheral']}/{tx_row['silicon_pad']}, "
            f"{rx_row['e1m_pad']} = {rx_row['e1m_function']} = "
            f"{rx_row['silicon_peripheral']}/{rx_row['silicon_pad']}).\n"
        )

    return (
        _COPYRIGHT_C +
        " *\n"
        f" * {sku_display} ({_AEN_FAMILY_DISPLAY}) Cortex-M55-{role.upper()} carrier pin control.\n"
        " *\n"
        " * Console: the E1M edge \"UART0\" maps to Alif "
        f"{uart_node.upper()} ({rx_row['silicon_pad']} RX_A / "
        f"{tx_row['silicon_pad']} TX_A) on\n"
        " * the AEN module PCB; the Alp E1M-EVK carrier wires its USB-UART console to it.\n"
        + confirm_block +
        " *\n"
        " * GPIO / I2C / SPI / Ethernet pin groups are added alongside their drivers\n"
        " * (the alp-sdk Alif peripheral drivers); only the console is wired here.\n"
        " */\n"
        "\n"
        "#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>\n"
        "\n"
        "&pinctrl {\n"
        f"\tpinctrl_{uart_node}: pinctrl_{uart_node} {{\n"
        "\t\tgroup0 {\n"
        f"\t\t\tpinmux = <{rx_macro}>;\n"
        "\t\t\tinput-enable;\n"
        "\t\t};\n"
        "\n"
        "\t\tgroup1 {\n"
        f"\t\t\tpinmux = <{tx_macro}>;\n"
        "\t\t};\n"
        "\t};\n"
        "};\n"
    )


def _aen_defconfig(uart_node: str) -> str:
    return (
        _COPYRIGHT_HASH +
        "\n"
        "CONFIG_ARM_MPU=y\n"
        "CONFIG_HW_STACK_PROTECTION=y\n"
        "\n"
        f"# Console: Alif {uart_node.upper()} (E1M edge \"UART0\", "
        "P3_4/P3_5) -- NS16550-class UART.\n"
        "CONFIG_SERIAL=y\n"
        "CONFIG_CONSOLE=y\n"
        "CONFIG_UART_CONSOLE=y\n"
        "CONFIG_UART_INTERRUPT_DRIVEN=y\n"
        "CONFIG_UART_NS16550=y\n"
    )


def _aen_kconfig_defconfig(dir_name: str, role: str) -> str:
    board_sym = dir_name.upper()
    role_u = role.upper()
    return (
        _COPYRIGHT_HASH +
        "\n"
        f"if BOARD_{board_sym}\n"
        "\n"
        f"# The Ensemble E8 RTSS-{role_u} has CONFIG_NUM_IRQS=480, so the ARMv8-M vector table is\n"
        "# (16 + 480) * 4 = 1984 bytes and the hardware VTOR must be 2 KiB (0x800) aligned.\n"
        "# For an MCUboot chain-loaded image the linker therefore places _vector_table at\n"
        "# slot_base + 0x800, but Zephyr hard-defaults the MCUboot header (ROM_START_OFFSET,\n"
        "# which is also imgtool --header-size) to 0x200 under BOOTLOADER_MCUBOOT and makes\n"
        "# it non-promptable. MCUboot then jumps to slot_base + 0x200 (header end) and reads\n"
        "# a garbage SP/PC from the 0x600 padding gap -> instant HardFault -> reset/reboot\n"
        "# loop, never reaching the app. Bump the header reservation to match the vector\n"
        "# alignment so the jump target == _vector_table. (Affects only chain-loaded images;\n"
        "# MCUboot itself builds with BOOTLOADER_MCUBOOT=n.)\n"
        "config ROM_START_OFFSET\n"
        "\tdefault 0x800 if BOOTLOADER_MCUBOOT\n"
        "\n"
        f"endif # BOARD_{board_sym}\n"
    )


def _aen_dts(
    sku: str, core_id: str, soc_spec: dict[str, Any], variant: dict[str, Any],
    dir_name: str, basename: str, rx_row: dict[str, Any], tx_row: dict[str, Any],
) -> str:
    role = core_id.split("_")[-1]                     # "hp" / "he"
    role_u = role.upper()
    other_role = "he" if role == "hp" else "hp"
    other_role_u = other_role.upper()
    other_core_id = f"m55_{other_role}"

    core = _find_core(soc_spec, core_id)
    other_core = _find_core(soc_spec, other_core_id)

    itcm_base, dtcm_base = core["itcm_global_base"], core["dtcm_global_base"]
    other_itcm, other_dtcm = other_core["itcm_global_base"], other_core["dtcm_global_base"]

    display_variant = variant["order_code"]
    slug = sku[len("E1M-"):].lower()
    compatible = f"alp,e1m-{slug}-m55-{role}"
    model = f"Alp {sku} Cortex-M55 {role_u} carrier"
    uart_node = _uart_node_label(rx_row)

    total_kib = round(float(variant["mram_mb"]) * 1024)
    partitions = _aen_flash_partitions(total_kib)
    image_kib = dict((label, size) for label, _off, size in partitions)["image-0"]

    bench_known = _AEN_BENCH_KNOWN_ITCM[role]
    itcm_known_suffix = "  (also bench-known)" if bench_known else ""

    lines: list[str] = []
    lines += [
        "/*",
        " * Copyright (c) 2026 Alp Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        f" * {sku} (Alif Ensemble E8, {display_variant}) Cortex-M55-{role_u} carrier,",
        " * for the Alp E1M-EVK.",
        " *",
        f" * Reuses the upstream Alif E8 SoC + RTSS-{role_u} cluster devicetree and:",
        " *   - retargets the console from the DevKit's UART2 to the E1M carrier console",
        f" *     (Alif {uart_node.upper()}, {rx_row['silicon_pad']}/{tx_row['silicon_pad']} -- the E1M edge \"UART0\");",
        " *   - runs boot + storage from on-die MRAM only.  The SoM's OSPI0 NOR",
        " *     (MX25UM25645) + HyperRAM (W958D8NB) are BOM-optional and NOT populated",
        " *     on the current batch, so there is no external XIP / flash device;",
        " *   - lays down a production MCUboot partition map in MRAM.",
    ]
    if role == "hp":
        lines += [
            " *",
            " * NOTE (AMP): this per-core board takes the full MRAM view for single-image",
            f" * bring-up (one core flashed/run at a time).  An {other_role_u}+{role_u} co-resident AMP image",
            " * partitions MRAM between the two cores at the system-integration layer",
            " * (sysbuild); see docs/heterogeneous-builds.md.",
        ]
    lines += [
        " *",
        f" * Inter-core IPC (the APSS<->RTSS-{role_u} MHUv2 doorbell pair) + the on-module",
        " * peripheral buses (GPIO / I2C / SPI / Ethernet) are added alongside the",
        " * alp-sdk Alif peripheral drivers; this base wires boot + console only.",
        " */",
        "",
        "/dts-v1/;",
        "",
        f"#include <alif/ensemble/{display_variant.lower()}.dtsi>",
        f"#include <alif/ensemble/common/ensemble_rtss_{role}.dtsi>",
        "#include <alif/ensemble_e8_peripherals.dtsi>",
        f'#include "{dir_name}-pinctrl.dtsi"',
        "",
        "/ {",
        f'\tmodel = "{model}";',
        f'\tcompatible = "{compatible}";',
        "",
        "\tchosen {",
        "\t\tzephyr,flash = &mram_storage;",
        "\t\t/*",
        "\t\t * MCUboot's flash_map_extended.c derives FLASH_DEVICE_BASE /",
        "\t\t * FLASH_DEVICE_ID from zephyr,flash-controller (the controller",
        "\t\t * node, NOT the soc-nv-flash child) -- mirrors the fork's",
        "\t\t * e1.dtsi chosen.  Without it MCUboot's flash_map fails to link.",
        "\t\t */",
        "\t\tzephyr,flash-controller = &mram;",
        "\t\tzephyr,sram = &dtcm;",
        f"\t\tzephyr,console = &{uart_node};",
        f"\t\tzephyr,shell-uart = &{uart_node};",
        "\t\tzephyr,code-partition = &slot0_partition;",
        "\t};",
        "};",
        "",
        "/*",
        " * ITCM/DTCM global_base -- central CPU-local -> system-bus address translation.",
        " *",
        " * hal_alif's soc_memory_map.h local_to_global() translates a CPU-local TCM",
        " * pointer into the address other bus masters (the Ethos-U NPU AXI master, the",
        " * SD/DMA controllers) must be programmed with, by reading",
        " * DT_PROP(DT_NODELABEL(itcm|dtcm), global_base).  Upstream Zephyr's",
        f" * arm,itcm/arm,dtcm bindings (ensemble_rtss_{role}.dtsi, included above) declare no",
        " * such property, so re-`compatible` the nodes onto the alp-sdk",
        " * alif,itcm/alif,dtcm bindings (zephyr/dts/bindings/memory-controllers/), which",
        " * require global_base.  Done here in the per-core board (not a per-example",
        f" * overlay) so every {role_u} app on this board inherits the same translation.",
        " *",
        " * Values transcribed from the Apache-2.0 zephyr_alif fork",
        f" * dts/arm/alif/ensemble/common/ensemble_rtss_{role}.dtsi:",
        f" *   itcm global_base = {hex(itcm_base)}{itcm_known_suffix}",
        f" *   dtcm global_base = {hex(dtcm_base)}",
        f" * These are the RTSS-{role_u} global TCM aliases; the RTSS-{other_role_u} core uses a different",
        f" * window ({hex(other_itcm)} / {hex(other_dtcm)}), set in the _{other_role} board dts.",
        " */",
        "&itcm {",
        f'\t/* fork ensemble_rtss_{role}.dtsi: itcm compatible = "alif,itcm" */',
        '\tcompatible = "alif,itcm";',
        f"\t/* fork ensemble_rtss_{role}.dtsi: itcm global_base = <{hex(itcm_base)}> */",
        f"\tglobal_base = <{hex(itcm_base)}>;",
        "};",
        "",
        "&dtcm {",
        f'\t/* fork ensemble_rtss_{role}.dtsi: dtcm compatible = "alif,dtcm" */',
        '\tcompatible = "alif,dtcm";',
        f"\t/* fork ensemble_rtss_{role}.dtsi: dtcm global_base = <{hex(dtcm_base)}> */",
        f"\tglobal_base = <{hex(dtcm_base)}>;",
        "};",
        "",
        "/*",
        " * On-die MRAM as a real flash controller (ADR 0017 Tier-2).",
        " *",
        " * Upstream Zephyr's E8 SoC dtsi exposes the MRAM only as a memory region",
        ' * (`mram: flash@80000000`, compatible "zephyr,memory-region","soc-nv-flash"):',
        " * XIP-readable but with NO flash driver, so FLASH_HAS_DRIVER_ENABLED stays",
        " * unset and MCUboot's flash_map has nothing to read/write slot0 with.",
        " *",
        " * We retarget the upstream `&mram` node into the fork's flash-controller",
        " * hierarchy WITHOUT editing upstream zephyr/: override `&mram`'s compatible to",
        ' * the controller compatible "alif,mram-flash-controller" (which the in-tree',
        " * flash_mram_alif driver binds, setting FLASH_HAS_DRIVER_ENABLED), and add a",
        " * `mram_storage` soc-nv-flash child carrying the program/erase geometry the",
        " * driver reads (DT_NODELABEL(mram_storage): erase-block-size / write-block-size",
        " * / reg).  This mirrors the fork e1.dtsi node structure exactly (controller",
        " * `mram_flash@80000000` + `mram_storage@80000000` soc-nv-flash child); the only",
        " * difference is we reuse the upstream `mram` nodelabel as the controller rather",
        " * than declaring a fresh `mram_flash` node, since upstream owns the node.",
    ]
    if role == "hp":
        lines += [
            f" * Kept byte-for-byte identical to the {other_role_u}-sibling board so a future HE+HP AMP",
            " * sysbuild sees one MRAM partition map.",
        ]
    lines += [
        " *",
        f" * MRAM partition map ({total_kib} KiB total) for the sysbuild/aen MCUboot profile",
        " * (MCUboot + a signed application image, swap-using-scratch).  Offsets are",
        " * relative to the soc-nv-flash child base (MRAM 0x80000000):",
        " *",
    ]
    labels_display = {
        "mcuboot": "mcuboot ",
        "image-0": "image-0 ",
        "image-1": "image-1 ",
        "image-scratch": "scratch ",
        "storage": "storage ",
    }
    trailers = {
        "image-0": "   (primary slot, code-partition)",
        "image-1": "   (secondary slot for OTA)",
        "storage": "    (settings / NVS)",
    }
    for label, off, size in partitions:
        lines.append(
            f" *   {labels_display[label]} 0x{off:06x}  {size} KiB{trailers.get(label, '')}"
        )
    lines += [
        f" *                      = {total_kib} KiB",
        " *",
        " * MRAM-only: the SoM OSPI NOR + HyperRAM are not populated on this batch, so",
        " * all of boot, both image slots, scratch, and storage live in MRAM.",
        " */",
        "&mram {",
        '\tcompatible = "alif,mram-flash-controller";',
        "\t#address-cells = <1>;",
        "\t#size-cells = <1>;",
        "\t/*",
        "\t * Drop the upstream memory-region properties: this node is no longer a",
        '\t * "zephyr,memory-region" (it is a flash controller now), and the',
        "\t * controller addresses its soc-nv-flash child by absolute `reg`, not via",
        "\t * `ranges`.  flash-controller.yaml / base.yaml declare neither property,",
        "\t * so they must be removed or DT binding-check fails.",
        "\t */",
        "\t/delete-property/ zephyr,memory-region;",
        "\t/delete-property/ ranges;",
        "",
        "\tmram_storage: mram_storage@80000000 {",
        '\t\tcompatible = "soc-nv-flash";',
        f"\t\treg = <0x80000000 DT_SIZE_K({total_kib})>;",
        "\t\terase-block-size = <1024>;",
        "\t\twrite-block-size = <16>;",
        "",
        "\t\tpartitions {",
        '\t\t\tcompatible = "fixed-partitions";',
        "\t\t\t#address-cells = <1>;",
        "\t\t\t#size-cells = <1>;",
        "",
    ]

    partition_node_labels = {
        "mcuboot": "boot_partition",
        "image-0": "slot0_partition",
        "image-1": "slot1_partition",
        "image-scratch": "scratch_partition",
        "storage": "storage_partition",
    }
    partition_dt_labels = {
        "mcuboot": "mcuboot",
        "image-0": "image-0",
        "image-1": "image-1",
        "image-scratch": "image-scratch",
        "storage": "storage",
    }
    for i, (label, off, size) in enumerate(partitions):
        node = partition_node_labels[label]
        dt_label = partition_dt_labels[label]
        lines += [
            f"\t\t\t{node}: partition@{off:x} {{",
            f'\t\t\t\tlabel = "{dt_label}";',
            f"\t\t\t\treg = <{hex(off)} DT_SIZE_K({size})>;",
            "\t\t\t};",
        ]
        if i != len(partitions) - 1:
            lines.append("")

    lines += [
        "\t\t};",
        "\t};",
        "};",
        "",
        f"/* Carrier console: Alif {uart_node.upper()} on {rx_row['silicon_pad']} (RX) / "
        f"{tx_row['silicon_pad']} (TX), 115200 8N1. */",
        f"&{uart_node} {{",
        '\tstatus = "okay";',
        f"\tpinctrl-0 = <&pinctrl_{uart_node}>;",
        '\tpinctrl-names = "default";',
        "\tcurrent-speed = <115200>;",
        "};",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------
# Top-level entry point
# ---------------------------------------------------------------------


def emit_zephyr_board(
    sku: str, core_id: str, metadata_root: Path,
) -> dict[str, str]:
    """Return {relative_path: content} for the requested SoM+core's board tree.

    *relative_path* is rooted at the board directory (e.g.
    ``"alp_e1m_aen801_m55_hp/board.yml"``) so the caller can write the
    result straight under a `--board-root`-style output directory.
    """
    sku_preset = _resolve_sku(sku, metadata_root)
    soc_spec = _load_soc_spec(sku_preset, metadata_root)
    variant = _resolve_variant(sku_preset, soc_spec)
    core = _find_core(soc_spec, core_id)
    dir_name, cluster, variant_tag, basename = _board_naming(
        sku_preset, core_id, variant, core)
    topo = _topology_entry(sku_preset, core_id)
    full_name = topo.get("zephyr_full_name")
    if not full_name:
        raise ZephyrBoardEmitError(
            f"topology.{core_id}.zephyr_full_name is required for "
            f"--emit zephyr-board (SoM {sku!r}); add it to "
            f"metadata/e1m_modules/{sku}.yaml")
    twister_name = topo.get("zephyr_twister_name") or full_name
    family = _sku_family_slug(sku)

    files: dict[str, str] = {
        f"{dir_name}/board.yml": _board_yml(dir_name, full_name, variant_tag, family),
        f"{dir_name}/Kconfig.{dir_name}": _kconfig_board(dir_name, variant_tag, cluster),
        f"{dir_name}/{basename}.yaml": _twister_yaml(
            dir_name, variant_tag, cluster, twister_name, family,
            sku_preset, core_id, variant),
    }

    if family == "aen":
        rx_row, tx_row = _aen_console_pinmux_rows(metadata_root)
        role = core_id.split("_")[-1]
        uart_node = _uart_node_label(rx_row)
        files[f"{dir_name}/{dir_name}-pinctrl.dtsi"] = _aen_pinctrl_dtsi(
            role, sku, rx_row, tx_row)
        files[f"{dir_name}/{basename}_defconfig"] = _aen_defconfig(uart_node)
        files[f"{dir_name}/Kconfig.defconfig"] = _aen_kconfig_defconfig(dir_name, role)
        files[f"{dir_name}/{basename}.dts"] = _aen_dts(
            sku, core_id, soc_spec, variant, dir_name, basename, rx_row, tx_row)

    silicon_parts = sku_preset["silicon"].split(":")
    soc_json_rel = f"metadata/socs/{silicon_parts[0]}/{silicon_parts[1]}/{silicon_parts[2]}.json"
    for relpath in list(files):
        style = "c" if relpath.endswith((".dts", "-pinctrl.dtsi")) else "hash"
        files[relpath] = _with_generated_banner(files[relpath], style, sku, soc_json_rel)

    return files

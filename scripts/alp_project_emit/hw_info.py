#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C header emission for `<alp/hw_info.h>` (`--emit hw-info-h`).

Split out of the former flat `alp_project_emit.py` module (issue #673
Phase 1) -- see `scripts/alp_project_emit/__init__.py` for the package-
level contract.  Structural split only, no behaviour change;
`check_emit_snapshots.py` pins the byte-identical output.
"""

from __future__ import annotations

from typing import Any

from alp_project_loader import METADATA_ROOT, _sku_family


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
    board_preset: dict[str, Any] | None,
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

    # Carry the composed board designator (`2626-r2`) rather than the bare
    # revision key, matching what the module's EEPROM identity holds and what
    # CONFIG_ALP_SDK_SOM_HW_REV emits.  These three must agree: the boot banner
    # compares the build value against the live EEPROM, and
    # CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL can turn a disagreement into k_panic().
    # The bare key is still what board.yaml and the loader's hw_revisions lookup
    # use -- see sdk_compat.board_designator().
    # Imported lazily: alp_orchestrate imports alp_project_emit, so a
    # module-level import here closes a cycle.
    from alp_orchestrate.sdk_compat import board_designator, load_family_table
    som_hw_rev = board_designator(
        load_family_table(METADATA_ROOT, family), som_hw_rev)

    board_block = project.get("board") or {}
    board_name = board_block.get("name") or ""
    board_hw_rev = ""
    if board_name and board_preset is not None:
        board_hw_rev = (board_block.get("hw_rev")
                          or board_preset.get("default_hw_rev")
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
    if board_name:
        lines.append(f'#define ALP_HW_BUILD_BOARD_NAME      "{board_name}"')
        if board_hw_rev:
            lines.append(f'#define ALP_HW_BUILD_BOARD_HW_REV    "{board_hw_rev}"')
    if os_choice:
        lines.append(f'#define ALP_HW_BUILD_OS              "{os_choice}"')
    if v2_cores is not None:
        # `active` and the loop below range over every non-off core in the
        # PROJECT, not the compiling slice, so this block's output --
        # `CORES` and the `HAS_<id>` macros -- is identical in every
        # slice's copy of the header.  `PRIMARY_CORE` is the one
        # exception: it tracks --core when the caller passes one.
        # Primary-core selection rule (used when --core is not given):
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
            lines.append("/* Per-core presence flags: `ALP_HW_BUILD_HAS_<id>` means "
                         "core <id> exists in")
            lines.append(" * the project; its value is that core's OS string.  Defined "
                         "for EVERY active")
            lines.append(" * core in EVERY slice -- it cannot say which core is "
                         "compiling.")
            lines.append(" *")
            lines.append(" * `ALP_HW_BUILD_PRIMARY_CORE` names the compiling slice ONLY "
                         "when this header")
            lines.append(" * was emitted --core-scoped (the per-slice build path).  "
                         "Emitted without")
            lines.append(" * --core -- as the project-wide recipe in "
                         "docs/board-config-emit.md does -- it")
            lines.append(" * names the project's primary core (first M-class core "
                         "alphabetically),")
            lines.append(" * which on a multi-core project is not necessarily the one "
                         "you are")
            lines.append(" * building.")
            lines.append(" *")
            lines.append(" * Zephyr's own CONFIG_BOARD and CONFIG_BOARD_TARGET differ "
                         "per slice too --")
            lines.append(" * the board name and the board/soc/cpucluster target "
                         "passed to `west build")
            lines.append(" * -b` -- and do so no matter how this header was emitted.  "
                         "E.g. on E1M-AEN801,")
            lines.append(" * the HE slice's CONFIG_BOARD is "
                         "\"alp_e1m_aen801_m55_he\" and its")
            lines.append(" * CONFIG_BOARD_TARGET ends \".../rtss_he\"; the HP "
                         "slice's end \"_hp\" and")
            lines.append(" * \".../rtss_hp\".  Prefer them over PRIMARY_CORE where "
                         "that portability")
            lines.append(" * matters. */")
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

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Kconfig / board-define slug derivation -- a shared leaf.

The slug + symbol helpers the per-slice config emitters (alp.conf, cmake-args)
share: the board-define slug and the on-module / helper-firmware Kconfig slug
derivation (+ the non-chip field set they skip). A dependency-free leaf so
every per-slice emitter pulls them from one place instead of duplicating.
Extracted as a #285 leaf seam (the paths.py / memregion.py move) ahead of the
kconfig emitter.

The peripheral->Kconfig symbol table used to live here too, as a module-level
constant computed once at import time from the SDK's own in-tree metadata/ --
a project's `--metadata-root` override never reached it (#1485). Callers now
import `alp_registries.peripheral_kconfig` directly and pass
`project.effective_metadata_root()` at the point of use.
"""

from __future__ import annotations

from sentinels import is_tbd


def _board_define_slug(name: str) -> str:
    """'E1M-X-EVK' -> 'E1M_X_EVK': the ALP_BOARD_* compile-define suffix.

    Mirrors gen_board_header._board_slug (lower + '-'->'_') then uppercases
    for the C macro. Used by <alp/board.h>'s board-selection facade.
    """
    return name.lower().replace("-", "_").upper()


def _som_define_slug(sku: str) -> str:
    """'E1M-AEN801' -> 'E1M_AEN801': the ALP_SOM_* compile-define suffix.

    Same transform as _board_define_slug; kept as its own name so the
    SoM-selection define (per-SKU capability restrictions in
    <alp/soc_caps.h>, gen_soc_caps.som_token) reads distinctly from the
    board facade define at call sites.
    """
    return _board_define_slug(sku)


# on_module fields that carry non-chip-slug values — skip them when
# walking the block for chip-driver enables.  Numeric fields, silicon
# identifiers, and structured sub-blocks are excluded by name rather
# than by type so the logic stays explicit and easy to audit.
_ON_MODULE_NON_CHIP_FIELDS: frozenset[str] = frozenset({
    "silicon",             # e.g. "renesas:rzv2n:n44" — SoC identifier, not a driver
    "ethernet_phy_count",  # integer count, not a chip slug
    "i2c_devices",         # sub-block: handled by extracting chip: entries below
    "ospi_memories",       # sub-block: storage parts (flash/HyperRAM); MPNs have no chips/ driver -- excluded like nor_flash/emmc below
    # Storage-class fields encode the SoC controller / peripheral name
    # that reaches the on-module storage (e.g. `nor_flash: xspi` -> the
    # NOR flash is wired to the xSPI controller; `emmc: sd0` -> eMMC on
    # SD/MMC controller 0).  They are routing annotations, not chip
    # slugs, and have no `chips/<part>/` driver behind them; emitting
    # them as CHIP_<NAME> trips the Zephyr build with an undefined-symbol
    # warning (no CONFIG_ALP_SDK_CHIP_XSPI / SD0 declaration exists).
    "nor_flash",
    "emmc",
})

# Any `<x>_driver_status` field (nor_flash_driver_status, emmc_driver_status,
# and whatever the next one is) is a maturity tier -- none/planned/partial/
# complete -- never a chip slug.  A denylist entry per field re-opens this
# leak every time a new one is added (it did once already: the literal
# string "none" reached CONFIG_ALP_SDK_CHIP_NONE=y before this suffix check
# existed).  Matched by suffix, not enumerated by name, so it can't miss the
# next one (#1169).
_DRIVER_STATUS_SUFFIX = "_driver_status"


def _slugs_from_on_module(on_module: dict) -> list[str]:
    """Extract unique, non-TBD chip slugs from an ``on_module:`` block.

    Walks every scalar field that is NOT in ``_ON_MODULE_NON_CHIP_FIELDS``,
    then recurses into the ``i2c_devices`` sub-block (extracting the
    ``chip:`` field from each device entry).  ``ospi_memories`` and the
    ``hyperram`` block are storage parts (NOR flash / HyperRAM) with no
    ``chips/<part>/`` driver, so their MPNs are NOT extracted as chip
    slugs (emitting them as ``CONFIG_ALP_SDK_CHIP_<X>`` would trip
    Zephyr's undefined-symbol guard).  Duplicate slugs and values of
    ``TBD`` / ``null`` are silently dropped.

    Returns a sorted, deduplicated list of slug strings.
    """
    seen: set[str] = set()

    def _add(val: object) -> None:
        if not val or is_tbd(val):
            return
        if not isinstance(val, str):
            return
        seen.add(val)

    # 1. Scalar fields — every key whose value is a plain string and
    #    is not in the exclusion list (or a `*_driver_status` field).
    for key, val in on_module.items():
        if key in _ON_MODULE_NON_CHIP_FIELDS or key.endswith(_DRIVER_STATUS_SUFFIX):
            continue
        if isinstance(val, str):
            _add(val)

    # 2. i2c_devices sub-block — each bus entry contains a `devices:`
    #    list; extract the `chip:` field from each device.
    #    Devices marked `assembled: optional` are DNI (do-not-install)
    #    on some builds and must NOT be auto-enabled as chip drivers —
    #    the customer explicitly enables them via `board.populated:`.
    i2c_buses = on_module.get("i2c_devices")
    if isinstance(i2c_buses, dict):
        for _bus, bus_entry in i2c_buses.items():
            if not isinstance(bus_entry, dict):
                continue
            for dev in bus_entry.get("devices") or []:
                if isinstance(dev, dict):
                    if dev.get("assembled") == "optional":
                        continue
                    _add(dev.get("chip"))

    return sorted(seen)


def _slugs_from_helper_firmware(helper_firmware: list) -> list[str]:
    """Extract unique, non-TBD chip slugs from a ``helper_firmware:`` list.

    Each entry is a dict; we pull the ``chip:`` field.  TBD values and
    missing fields are skipped.  Returns a sorted, deduplicated list.
    """
    seen: set[str] = set()
    for entry in helper_firmware or []:
        if not isinstance(entry, dict):
            continue
        chip = entry.get("chip")
        if chip and not is_tbd(chip):
            seen.add(chip)
    return sorted(seen)


# Slugs that map to BLOCK_ Kconfig symbols rather than CHIP_.
# These live under blocks/ + <alp/blocks/*.h> because they are
# SDK-level *block* utilities (`alp_button_led_*`, `alp_pdm_mic_*`)
# rather than third-party-IC chip drivers; see blocks/README.md.
#
# Single source of truth -- kconfig.py's emitter, check_example_portability.py
# and alp_cli/validator.py's ALP-B008 chip-token check all consult this same
# set rather than each carrying its own copy (a prior state that had drifted
# into three independent copies).
_BLOCK_SLUGS = frozenset({"button_led", "pdm_mic"})

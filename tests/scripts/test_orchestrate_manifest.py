# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- IPC contract header, DTS
reservations, and the system manifest emitters (emit_ipc_contract_h(),
emit_dts_reservations(), emit_system_manifest()).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_manifest.py -v
"""

from __future__ import annotations

import json
import re
import shutil
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    emit_dts_reservations,
    emit_ipc_contract_h,
    emit_system_manifest,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# 7. emit_ipc_contract_h
# ---------------------------------------------------------------------


def test_emit_ipc_contract_h_shape(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_ipc_contract_h(project)

    # Header guard.
    assert "#ifndef ALP_SYSTEM_IPC_H" in out
    assert "#define ALP_SYSTEM_IPC_H" in out
    assert out.rstrip().endswith("#endif /* ALP_SYSTEM_IPC_H */")

    # Required ALP_IPC_<NAME>_* macros.
    expected_macros = [
        "ALP_IPC_ALP_DEFAULT_RPMSG_NAME",
        "ALP_IPC_ALP_DEFAULT_RPMSG_ADDR",
        "ALP_IPC_ALP_DEFAULT_RPMSG_SIZE",
        "ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT",
        "ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT",
        "ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH",
    ]
    for m in expected_macros:
        assert m in out, f"missing macro {m}"

    # All addr/size values must be hex u suffixed.
    addr_pattern = re.compile(
        r"#define\s+ALP_IPC_ALP_DEFAULT_RPMSG_ADDR\s+0x[0-9a-fA-F]+u")
    assert addr_pattern.search(out)


# ---------------------------------------------------------------------
# 8. emit_dts_reservations
# ---------------------------------------------------------------------


def test_emit_dts_reservations_shape(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_dts_reservations(project)

    # Required scaffolding.
    assert "reserved-memory" in out
    assert "#address-cells = <2>;" in out
    assert "#size-cells = <2>;" in out

    # Per-entry shape.
    assert 'compatible = "shared-dma-pool";' in out
    assert "no-map;" in out
    assert 'label = "alp_default_rpmsg";' in out

    # Node name + label match.
    node_re = re.compile(
        r"alp_default_rpmsg:\s+alp_default_rpmsg@[0-9a-fA-F]+\s*\{")
    assert node_re.search(out), "missing alp_default_rpmsg node"


# ---------------------------------------------------------------------
# 9. emit_system_manifest
# ---------------------------------------------------------------------


def test_emit_system_manifest_round_trip(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_system_manifest(project)
    parsed = yaml.safe_load(out)

    assert isinstance(parsed, dict)
    # Required top-level keys.
    for k in ("slices", "ipc", "helper_mcus", "boot_order", "hw_info"):
        assert k in parsed, f"missing top-level key {k}"

    assert parsed["hw_info"]["sku"] == "E1M-V2N101"
    assert parsed["hw_info"]["silicon"] == "renesas:rzv2n:n44"

    # Slices carry the per-core ids.
    cids = [s["core_id"] for s in parsed["slices"]]
    assert "a55_cluster" in cids
    assert "m33_sm" in cids

    # Carve-out projected into manifest.
    assert len(parsed["ipc"]) == 1
    assert parsed["ipc"][0]["name"] == "alp_default_rpmsg"

    # Helper-MCU registration: V2N101's Phase-3 `helper_firmware:`
    # block lists gd32_bridge (the GD32G553 supervisor firmware
    # image).  The manifest carries the chip slug + flash_policy
    # verbatim; firmware_path is absent (#852/#936 review fix, see
    # test_emit_system_manifest_populates_helper_mcus below).
    helper_names = [h["name"] for h in parsed["helper_mcus"]]
    assert "gd32_bridge" in helper_names
    gd32 = next(h for h in parsed["helper_mcus"]
                if h["name"] == "gd32_bridge")
    assert gd32["chip"] == "gd32g553"
    # #1439: no local flash path is declared any more.
    assert "flash_method" not in gd32
    assert gd32["flash_policy"] == "recovery_only"


def test_emit_system_manifest_includes_hw_info_eeprom_feature(
    tmp_path: Path,
) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-V2N101

preset: e1m-x-evk
cores:
  a55_cluster:
    os: "off"
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [i2c]

chips:
  - eeprom_24c128

features:
  hw_info:
    eeprom:
      bus: e1m_i2c0
      addr_7bit: 0x54
      offset: 32
""")
    parsed = yaml.safe_load(emit_system_manifest(load_board_yaml(path)))

    assert parsed["hw_info"]["eeprom"] == {
        "bus":       "e1m_i2c0",
        "bus_id":    0,
        "addr_7bit": 0x54,
        "offset":    32,
    }


# ---------------------------------------------------------------------
# Phase 3 follow-up: system-manifest helper-MCU + flash-method
# population.
# ---------------------------------------------------------------------


def test_emit_system_manifest_populates_helper_mcus(tmp_path: Path) -> None:
    """Phase 3 helper-MCU population.

    V2N101's preset declares one helper_firmware entry (gd32_bridge);
    the manifest must carry the chip slug + flash_policy verbatim.
    `firmware_path` is entirely ABSENT from the preset (#852 review fix,
    2026-07): the old `firmware_path: TBD` sentinel wasn't actually treated
    as a sentinel by the frozen v0.4.1 Rust flash planner
    (`crates/tan-cli/src/commands/flash/mod.rs`) -- it became the artefact
    string and a real flasher was spawned against a nonexistent `TBD`
    path. Dropping the field entirely means nothing resolves that sentinel
    path; it is `flash_policy: recovery_only`, not `update_channel`, that
    decides this entry's fate -- Python Tan's
    `python/tan/core/flash_plan.py::helper_flash_gate` (tan-cli#611)
    declines it ahead of any `flash_method` check ("is programmed by Alp
    Lab in production and is customer-flashable only to recover a bricked
    device...; skipping. Field updates arrive over update_channel:
    alp_ota_spi_bridge." -- status `skipped`, rc `-1`). No `firmware_path`
    key means no TBD note either.
    """
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_system_manifest(project)
    parsed = yaml.safe_load(out)

    helpers = parsed["helper_mcus"]
    assert isinstance(helpers, list)
    by_name = {h["name"]: h for h in helpers}
    assert "gd32_bridge" in by_name
    gd32 = by_name["gd32_bridge"]
    assert gd32["chip"] == "gd32g553"
    assert "firmware_path" not in gd32
    assert "note" not in gd32
    # #1439: GD32 programming left tan, so the preset declares neither
    # `flash_method` nor `flash_args` and the row must carry neither.
    assert "flash_method" not in gd32
    assert "flash_args" not in gd32
    assert gd32["flash_policy"] == "recovery_only"
    assert gd32["update_channel"] == "alp_ota_spi_bridge"


def test_helper_mcu_keeps_sibling_keys_alongside_update_channel(
    tmp_path: Path,
) -> None:
    """#1357: an `update_channel` must not delete its sibling keys.

    `_helper_mcus` used to project the axes as an either/or, so the GD32
    bridge -- which carries a field-update channel (protocol v0.6 Path A,
    slot-A/B application bootloader) alongside the fact of WHO may flash
    it locally -- would have had that second fact dropped from the
    manifest.

    #1439 removed `flash_method`/`flash_args` from the preset entirely,
    so this no longer guards those two keys; `flash_policy` is now the
    sibling at risk, and the projection bug it pins is the same one.
    Every key the preset declares must survive independently.
    """
    path = _write_board(tmp_path, V2N_HAPPY)
    parsed = yaml.safe_load(emit_system_manifest(load_board_yaml(path)))

    gd32 = next(h for h in parsed["helper_mcus"]
                if h["name"] == "gd32_bridge")

    # The field-update channel is projected...
    assert gd32["update_channel"] == "alp_ota_spi_bridge"
    # ...and it did NOT suppress the policy beside it.  Assert the key
    # SURVIVED before reading it, so the regression reports as "the
    # channel deleted its sibling" rather than a bare KeyError.
    assert "flash_policy" in gd32, (
        "update_channel suppressed flash_policy; who may flash this part "
        f"vanished from the manifest: {sorted(gd32)}"
    )
    assert gd32["flash_policy"] == "recovery_only"
    # And the removed axis stays removed -- GD32 programming is out of
    # tan's scope (#1439, tan-cli#732); no preset names a local flash
    # path for it.
    assert "flash_method" not in gd32
    assert "flash_args" not in gd32


def test_helper_mcu_omits_keys_the_preset_does_not_declare(
    tmp_path: Path,
) -> None:
    """The converse of the test above: independent projection must not
    start emitting `null` placeholders for absent keys.

    AEN801's `cc3501e_otp` declares no `flash_method`, so the row must
    carry no `flash_method` key at all -- `tan flash` reads a present-
    but-null key as a recipe.
    """
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801

preset: e1m-evk
cores:
  m55_he:
    os: zephyr
    app: ./he
""")
    parsed = yaml.safe_load(emit_system_manifest(load_board_yaml(path)))

    cc = next(h for h in parsed["helper_mcus"]
              if h["name"] == "cc3501e_otp")
    assert cc["update_channel"] == "alp_ota_spi_otp"
    assert cc["flash_policy"] == "recovery_only"
    assert "flash_method" not in cc
    assert "flash_args" not in cc


def test_emit_system_manifest_populates_flash_method(tmp_path: Path) -> None:
    """Phase 3 per-slice flash_method + flash_args.

    Every non-off slice in a heterogeneous V2N project must carry
    a `flash_method:` + `flash_args:` block in the manifest so
    `west alp-flash` can dispatch each slice without re-deriving
    the backend.
    """
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_system_manifest(project)
    parsed = yaml.safe_load(out)

    by_core = {s["core_id"]: s for s in parsed["slices"]}

    a55 = by_core["a55_cluster"]
    assert a55["flash_method"] == "yocto_wic_to_sd_or_emmc"
    assert isinstance(a55["flash_args"], dict)
    assert "target" in a55["flash_args"]

    m33 = by_core["m33_sm"]
    assert m33["flash_method"] == "zephyr_west_flash"
    assert isinstance(m33["flash_args"], dict)
    # No runner is forced: not every in-tree board registers openocd
    # (e.g. AEN's board.cmake sets flash-runner: alif_flash), so the
    # slice defers to the board.cmake default runner.
    assert m33["flash_args"] == {}


# ---------------------------------------------------------------------
# `flash_args.jlink_flash_device` -- the AEN Flow D arming fact
# ---------------------------------------------------------------------

# Off-topology a32_cluster (nothing to build in these flash-recipe-only
# tests, mirrors examples/aen/aen-analog-validate/board.yaml); m55_hp and
# m55_he fall back to the SoM preset's `topology:` stock-shim defaults, so
# no `app:` needs restating here.
AEN_HAPPY = """
som:
  sku: E1M-AEN801

cores:
  a32_cluster:
    os: "off"
"""


def test_emit_system_manifest_aen_flash_args_carries_jlink_flash_device(
    tmp_path: Path,
) -> None:
    """E1M-AEN801's resolved SoC variant (silicon_variant:
    AE822FA0E5597LS0) publishes `debug.jlink_flash_device:
    AE822FA0E5597LS0_M55_HE` (metadata/socs/alif/ensemble/e8.json) -- the
    J-Link part-number profile that arms Flow D's built-in MRAM loader.
    Both M55 zephyr slices must carry it in `flash_args` so a downstream
    consumer (tan) can pick that path over the SETOOLS/SE-UART fallback.
    """
    path = _write_board(tmp_path, AEN_HAPPY)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        slice_ = by_core[core_id]
        assert slice_["flash_method"] == "zephyr_west_flash"
        assert slice_["flash_args"]["jlink_flash_device"] == \
            "AE822FA0E5597LS0_M55_HE"


# E1M-AEN401's resolved SoC variant (silicon_variant: AE402FA0E5597LE0 in
# metadata/socs/alif/ensemble/e4.json) DOES publish a `debug:` block --
# `jlink_device` -- but `jlink_flash_device` is an explicit `null` (#1295's
# one declared known-unknown: SEGGER ships no J-Link profile for any
# Alif-declared E4 part). That's the branch that actually needs coverage:
# V2N's n44.json has no `debug:` block at all, so asserting against it only
# proves the all-absent case, not "variant resolved, `debug:` present, key
# null".
#
# E1M-AEN701 (AE722F80F55D5LS) covered this case before #1295 populated
# `jlink_flash_device` for every Alif Ensemble variant it could positively
# identify; that landed on this branch and AEN701 now carries a real value,
# so it no longer isolates the "unarmed" branch these three tests protect.
# E4 has no A32 cluster (2x M55 only), so unlike AEN_HAPPY there is no
# off-topology core to park here; `m55_hp: {}` exists only to satisfy the
# schema's `cores: minProperties: 1` while taking every default. m55_he is
# left out of this block entirely and, per the schema, inherits the SoM
# preset's topology defaults exactly the same way.
AEN_NO_JLINK_FLASH_DEVICE = """
som:
  sku: E1M-AEN401

cores:
  m55_hp: {}
"""


def test_emit_system_manifest_carries_a_declared_null_jlink_flash_device(
    tmp_path: Path,
) -> None:
    """A DECLARED `jlink_flash_device: null` must reach `flash_args` as a
    PRESENT null -- not be dropped (#1295, tan-cli#734).

    This asserts the opposite of what it did before. It used to require the
    tidy `{}`, on the reading that "an absent key IS the correct unknown
    state". That conflates two facts the schema deliberately separates:

      * ABSENT  -- the variant says nothing; the Flow A default stands.
      * NULL    -- the variant publishes "no known J-Link flash profile";
                   soc-spec-v1.schema.json's own description says consumers
                   must refuse rather than silently choose another transport.

    `E1M-AEN401`'s e4.json variant is the one real declared-null in the
    corpus: SEGGER ships no device profile for any Alif-declared E4 part
    (#1443), so there is nothing honest to put there. Dropping the key
    re-collapses that into "absent", and tan's presence-based
    `flow_d_available()` then silently downgrades Flow D to the SE-UART
    Flow A path -- which is Linux-only, so a Windows operator's flash fails
    later somewhere else with the real cause already discarded.

    Asserted on KEY PRESENCE, not on the value: absent and declared-null
    both read as `None`, so a value assertion cannot tell them apart and
    would pass against the bug.
    """
    path = _write_board(tmp_path, AEN_NO_JLINK_FLASH_DEVICE)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        flash_args = by_core[core_id]["flash_args"]
        assert "jlink_flash_device" in flash_args, flash_args
        assert flash_args["jlink_flash_device"] is None, flash_args


def test_a_declared_null_still_resolves_its_slot0_load_address(
    tmp_path: Path,
) -> None:
    """The third truthiness gate on the same field, and the only one no
    unit test reaches.

    `_validate_topology_cores` gated `slot0_load_address` resolution on
    `if jlink_flash_device` -- truthy -- so a declared null silently
    produced no slot0 address at all. That cannot be exercised with a
    constructed `Slice`: the gate lives inside a stage needing a fully
    resolved SoM preset and SoC spec. `E1M-AEN401` is the one SKU that
    can reach it -- e4.json declares the null, and #1445 gave the SoM the
    disjoint per-role windows the address comes from.

    Verbatim addresses, per #1445's shared AEN layout: `he_slot0` at
    0x80010000 and `hp_slot0` at 0x802b0000 -- the same pair this branch
    writes into all five AEN SoM presets, and the pair #1069 exists to keep
    apart. Asserted BY VALUE rather than by presence: a regression that
    collapsed both roles onto one address would still satisfy a presence
    check while being exactly the corruption the disjointness rule forbids.
    """
    path = _write_board(tmp_path, AEN_NO_JLINK_FLASH_DEVICE)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    addresses = {
        core_id: by_core[core_id]["flash_args"].get("slot0_load_address")
        for core_id in ("m55_he", "m55_hp")
    }
    assert addresses == {
        "m55_he": "0x80010000",
        "m55_hp": "0x802b0000",
    }, addresses


# ---------------------------------------------------------------------
# `flash_args.expect_dpidr` + `flash_args.jlink_device` -- the wrong-board
# SW-DP IDR preflight PAIR (#1355)
#
# Why a pair and not two independent keys: tan's
# `validate_flow_d_preflight_args` raises `FlashPlanError` on
# `(expected is None) != (read_device is None)`, at PLAN time, for every
# Flow D entry -- so emitting either key alone would hard-refuse every AEN
# flash, dry runs included. Both or neither is the only legal shape.
# ---------------------------------------------------------------------


def test_emit_system_manifest_aen_flash_args_carries_dpidr_preflight_pair(
    tmp_path: Path,
) -> None:
    """E1M-AEN801's resolved SoC variant (AE822FA0E5597LS0) publishes
    `debug.expect_dpidr: 0x4C013477` -- the SW-DP IDR measured on this
    board -- and `debug.jlink_device` for both M55s. Both M55 slices must
    carry BOTH in `flash_args`, so a downstream flasher can read the debug
    port and abort on the wrong board while the session is still read-only.

    Values asserted verbatim: `0x4C013477` is a measurement (re-confirmed on
    silicon 2026-08-10), and a wrong digit here flashes the wrong board.
    """
    path = _write_board(tmp_path, AEN_HAPPY)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        flash_args = by_core[core_id]["flash_args"]
        assert flash_args.get("expect_dpidr") == "0x4C013477"
        # The LIVE-CORE attach profile -- deliberately the generic
        # `Cortex-M55`, NOT the part-number `jlink_flash_device` profile
        # alongside it, which is the write-time flash-algorithm device.
        assert flash_args.get("jlink_device") == "Cortex-M55"
        assert flash_args.get("jlink_device") != \
            flash_args.get("jlink_flash_device")


def test_emit_system_manifest_dpidr_preflight_pair_is_never_half_armed(
    tmp_path: Path,
) -> None:
    """The invariant, checked across every shipped SoM preset rather than
    the one SKU that arms it today: NO emitted slice may carry exactly one
    of `expect_dpidr` / `jlink_device`.

    Driven off `metadata/e1m_modules/E1M-*.yaml` on disk, not a hand-written
    SKU list, so a SoM added later is covered without editing this test --
    and a future SoC JSON that publishes `expect_dpidr` while forgetting a
    core's attach profile fails HERE rather than at a customer's flash.
    """
    presets = sorted(
        (REPO / "metadata" / "e1m_modules").glob("E1M-*.yaml"))
    assert presets, "no SoM presets found -- the sweep would be vacuous"

    checked = 0
    armed = 0
    for preset in presets:
        sku = preset.stem
        topology = list(
            (yaml.safe_load(preset.read_text(encoding="utf-8")) or {})
            .get("topology") or {})
        assert topology, f"{sku}: preset declares no topology"
        sku_dir = tmp_path / sku
        sku_dir.mkdir()
        # `cores:` is schema-required; one topology key with an empty body
        # is the minimum that validates, and every other core falls back to
        # the preset's own topology defaults.
        path = _write_board(
            sku_dir, f"som:\n  sku: {sku}\ncores:\n  {topology[0]}: {{}}\n")
        try:
            project = load_board_yaml(path)
        except OrchestratorError as exc:
            # A SoM whose only hw_rev is still `tbd` cannot be loaded at
            # all (E1M-NX9101 today). Skip ONLY that, and never a real
            # failure -- a silent blanket `except` would let this whole
            # sweep degrade to a no-op without anyone noticing.
            if "not buildable" not in str(exc):
                raise
            continue
        parsed = yaml.safe_load(emit_system_manifest(project))
        for slice_ in parsed["slices"]:
            flash_args = slice_.get("flash_args") or {}
            assert ("expect_dpidr" in flash_args) == \
                ("jlink_device" in flash_args), (
                    f"{sku}/{slice_['core_id']}: half-armed DPIDR preflight "
                    f"{flash_args!r} -- a downstream flasher refuses this "
                    f"shape outright rather than skipping the check")
            checked += 1
            armed += "expect_dpidr" in flash_args
    assert checked, "no slices emitted -- the invariant went unexercised"
    # An all-absent sweep would satisfy the invariant vacuously; at least
    # E1M-AEN801's two M55s must actually arm it.
    assert armed >= 2, \
        f"only {armed} slice(s) armed the preflight -- expected AEN801's two"


def test_emit_system_manifest_omits_dpidr_preflight_when_unmeasured(
    tmp_path: Path,
) -> None:
    """E1M-AEN401's variant publishes `debug.jlink_device` but NO
    `expect_dpidr` (nobody has measured that part's DPIDR). Neither key may
    reach `flash_args`: `jlink_device` alone is the half-armed shape a
    downstream flasher refuses, so an unmeasured DPIDR must leave the
    preflight entirely absent -- unarmed, not broken.
    """
    path = _write_board(tmp_path, AEN_NO_JLINK_FLASH_DEVICE)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        flash_args = by_core[core_id]["flash_args"]
        assert "jlink_device" not in flash_args
        assert "expect_dpidr" not in flash_args


# ---------------------------------------------------------------------
# `flash_args.slot0_load_address` -- the AEN MRAM slot0-XIP address (tan-
# cli#353): before this, alp-sdk emitted no such key, so tan correctly
# armed Flow D and then refused (`flash_args.slot0_load_address is
# required to auto-sign via SETOOLS`), forcing a customer to hand-edit
# `system-manifest.yaml` to flash an AEN.
# ---------------------------------------------------------------------


def test_emit_system_manifest_aen_flash_args_carries_slot0_load_address(
    tmp_path: Path,
) -> None:
    """E1M-AEN801 declares a #1069 disjoint-slot0 `memory_map:` (HE and HP
    boot from DIFFERENT physical MRAM windows, deliberately -- the stock
    symmetric layout put them at the SAME address and one flash silently
    clobbered the other). Each M55 slice's `flash_args` must carry ITS OWN
    core's address, not a value shared with its sibling.

    Values asserted verbatim against `metadata/e1m_modules/E1M-AEN801.yaml`
    `memory_map:` (`he_slot0`/`hp_slot0` `base:`) -- a wrong digit here
    writes the application blob into the wrong core's MRAM window, or into
    the sibling core's live slot0.
    """
    path = _write_board(tmp_path, AEN_HAPPY)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    assert by_core["m55_he"]["flash_args"]["slot0_load_address"] == \
        "0x80010000"
    assert by_core["m55_hp"]["flash_args"]["slot0_load_address"] == \
        "0x802b0000"


def test_emit_system_manifest_flash_args_omits_slot0_load_address_when_flow_d_unarmed(
    tmp_path: Path,
) -> None:
    """`slot0_load_address` is meaningless without Flow D and must never
    appear on its own.

    The invariant is unchanged; its FIXTURE had to move. This used to run
    against `E1M-AEN401` on the reading that a `jlink_flash_device: null`
    leaves Flow D unarmed. That is no longer what a declared null means:
    tan's `flow_d_available()` is presence-based (`FLOW_D_KEYS` is exactly
    `("jlink_flash_device",)`), so a published null deliberately DOES arm
    Flow D -- that is the whole point, so the null reaches
    `plan_alif_mram_jlink` and becomes a loud refusal instead of a silent
    SE-UART fallback. `test_a_declared_null_still_resolves_its_slot0_load_address`
    now pins that side.

    The state this test guards is the OTHER one -- a variant that declares
    NOTHING -- and after #1295 no shipped Alif Ensemble variant is in it,
    so the premise is unreachable from the corpus. Rather than delete a
    real invariant along with its dead fixture, the key is stripped from a
    throwaway copy of the metadata tree, which reaches the same state
    honestly and keeps failing if the emitter ever starts publishing an
    orphan slot0.
    """
    meta = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", meta)
    e4 = meta / "socs" / "alif" / "ensemble" / "e4.json"
    spec = json.loads(e4.read_text(encoding="utf-8"))
    removed = [v["debug"].pop("jlink_flash_device", "__missing__")
               for v in spec["variants"]]
    assert removed == [None], (
        f"e4.json no longer declares exactly one null jlink_flash_device "
        f"({removed}) -- this fixture's premise moved")
    e4.write_text(json.dumps(spec, indent=2) + "\n",
                  encoding="utf-8", newline="\n")

    path = _write_board(tmp_path, AEN_NO_JLINK_FLASH_DEVICE)
    project = load_board_yaml(path, metadata_root=meta)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        flash_args = by_core[core_id]["flash_args"]
        assert "slot0_load_address" not in flash_args, flash_args
        # ... and the key that would have armed it is genuinely gone, so
        # this is the absent case and not a mis-built fixture.
        assert "jlink_flash_device" not in flash_args, flash_args


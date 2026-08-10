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

import re
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
    # image).  The manifest carries the chip slug + flash_method
    # verbatim; firmware_path is absent (#852/#936 review fix, see
    # test_emit_system_manifest_populates_helper_mcus below).
    helper_names = [h["name"] for h in parsed["helper_mcus"]]
    assert "gd32_bridge" in helper_names
    gd32 = next(h for h in parsed["helper_mcus"]
                if h["name"] == "gd32_bridge")
    assert gd32["chip"] == "gd32g553"
    assert gd32["flash_method"] == "swd_probe"


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
    the manifest must carry the chip slug + flash_method verbatim.
    `firmware_path` is entirely ABSENT from the preset (#852 review fix,
    2026-07): the old `firmware_path: TBD` sentinel wasn't actually treated
    as a sentinel by the frozen v0.4.1 Rust flash planner
    (`crates/tan-cli/src/commands/flash/mod.rs`) -- it became the artefact
    string and a real flasher was spawned against a nonexistent `TBD`
    path. Dropping the field entirely makes both the oracle and Python Tan's
    `python/tan/commands/flash_cmd.py` clean-refusal
    path fire instead ("has no output_artefact / firmware_path; can't
    flash"). No `firmware_path` key means no TBD note either.
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
    assert gd32["flash_method"] == "swd_probe"
    assert isinstance(gd32["flash_args"], dict)
    assert gd32["flash_args"]["target"] == "gd32g553"


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


# E1M-AEN701's resolved SoC variant (silicon_variant: AE722F80F55D5LS in
# metadata/socs/alif/ensemble/e7.json) DOES publish a `debug:` block --
# `jlink_device` -- but no `jlink_flash_device` key.  That's the branch that
# actually needs coverage: V2N's n44.json has no `debug:` block at all, so
# asserting against it only proves the all-absent case, not "variant
# resolved, `debug:` present, key missing".
AEN_NO_JLINK_FLASH_DEVICE = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
"""


def test_emit_system_manifest_flash_args_omits_jlink_flash_device_when_absent(
    tmp_path: Path,
) -> None:
    """`flash_args` must stay the tidy `{}` with NO `jlink_flash_device`
    key, never a `null` placeholder (the schema's published-unknown
    contract: an absent key IS the correct unknown state) -- even though
    this variant's `debug:` block exists and carries its sibling
    `jlink_device`."""
    path = _write_board(tmp_path, AEN_NO_JLINK_FLASH_DEVICE)
    project = load_board_yaml(path)
    parsed = yaml.safe_load(emit_system_manifest(project))

    by_core = {s["core_id"]: s for s in parsed["slices"]}
    for core_id in ("m55_hp", "m55_he"):
        assert by_core[core_id]["flash_args"] == {}


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
    """E1M-AEN701's variant publishes `debug.jlink_device` but NO
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


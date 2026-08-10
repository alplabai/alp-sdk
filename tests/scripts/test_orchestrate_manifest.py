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

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
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


def test_helper_mcu_keeps_flash_keys_alongside_update_channel(
    tmp_path: Path,
) -> None:
    """#1357: an `update_channel` must not delete the local flash path.

    `_helper_mcus` used to project the two as an either/or, so the GD32
    bridge -- which has BOTH a field-update channel (protocol v0.6 Path
    A, slot-A/B application bootloader) and a recovery-only SWD flash --
    would have had its `flash_method`/`flash_args` dropped from the
    manifest. That is worse than declining the recovery path: the path
    vanishes, so a tool cannot even explain why it is unavailable to
    someone with a bricked bridge.

    Every key the preset declares must survive independently.
    """
    path = _write_board(tmp_path, V2N_HAPPY)
    parsed = yaml.safe_load(emit_system_manifest(load_board_yaml(path)))

    gd32 = next(h for h in parsed["helper_mcus"]
                if h["name"] == "gd32_bridge")

    # The field-update channel is projected...
    assert gd32["update_channel"] == "alp_ota_spi_bridge"
    # ...and it did NOT suppress the local flash recipe.  Assert the keys
    # SURVIVED before reading them, so the regression reports as "the
    # channel deleted the recovery path" rather than a bare KeyError.
    assert {"flash_method", "flash_args", "flash_policy"} <= set(gd32), (
        "update_channel suppressed the local flash keys; the recovery "
        f"path vanished from the manifest: {sorted(gd32)}"
    )
    assert gd32["flash_method"] == "swd_probe"
    assert gd32["flash_args"]["target"] == "gd32g553"
    assert gd32["flash_args"]["jlink_device"] == "GD32G553MEY7TR"
    assert gd32["flash_args"]["base"] == "0x08000000"
    # ...and the fact that decides who may invoke it came along too.
    assert gd32["flash_policy"] == "recovery_only"


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
    assert cc["flash_policy"] == "factory"
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


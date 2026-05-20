# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate.py (Phase 2 of the
2026-05-15 heterogeneous-OS orchestration design).

Run locally:

    python -m pytest tests/scripts/test_alp_orchestrate.py -v
"""

from __future__ import annotations

import re
import shutil
import sys
import textwrap
from pathlib import Path

import pytest
import yaml


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

# Imported lazily after sys.path is augmented above.
from alp_orchestrate import (                       # noqa: E402
    BoardProject,
    OrchestratorError,
    Orchestrator,
    Slice,
    _slice_alp_conf,
    _slugs_from_helper_firmware,
    _slugs_from_on_module,
    emit_dts_reservations,
    emit_ipc_contract_h,
    emit_system_manifest,
    load_board_yaml,
    resolve_carve_outs,
)


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""


V2N_TOPOLOGY_FALLBACK = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


V1_REJECT = """
som:
  sku: E1M-V2N101

os: zephyr

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


UNKNOWN_CORE = """
som:
  sku: E1M-V2N101

cores:
  m99_garbage:
    os: zephyr
    app: ./garbage
"""


# AEN701 has TBD memory_map entries (per the Phase 1 metadata land);
# any ipc carve-out must surface that as a clear OrchestratorError.
AEN_TBD = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: zephyr
    app: ./m55_he

ipc:
  - kind: rpmsg
    endpoints: [m55_hp, m55_he]
    carve_out_kb: 64
    name: alp_test_rpmsg
"""


# ---------------------------------------------------------------------
# 1. load_board_yaml -- happy path
# ---------------------------------------------------------------------


def test_load_board_yaml_v2n_happy(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    assert isinstance(project, BoardProject)
    assert project.sku == "E1M-V2N101"
    assert project.hw_rev == "r1"
    assert project.board_name == "test-v2n-board"
    assert set(project.cores.keys()) == {"a55_cluster", "m33_sm"}

    a55 = project.cores["a55_cluster"]
    m33 = project.cores["m33_sm"]
    assert a55.os == "yocto"
    assert a55.app == "./linux"               # customer override wins
    assert a55.image == "alp-image-edge"
    assert a55.machine == "e1m-v2n101-a55"    # inherited from SoM topology
    assert a55.toolchain == "poky-glibc"
    assert a55.peripherals == ["ethernet", "usb"]

    assert m33.os == "zephyr"
    assert m33.app == "./m33"
    assert m33.board == "alp_e1m_v2n101_m33_sm"      # inherited
    assert m33.toolchain == "arm-zephyr-eabi"
    assert m33.peripherals == ["adc", "pwm", "i2c", "gpio"]
    assert m33.libraries == ["cmsis_dsp"]

    assert len(project.ipc) == 1
    assert project.ipc[0].name == "alp_default_rpmsg"
    assert project.ipc[0].carve_out_kb == 512


# ---------------------------------------------------------------------
# 2. Loader topology fallback
# ---------------------------------------------------------------------


def test_load_board_yaml_topology_fallback(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_TOPOLOGY_FALLBACK)
    project = load_board_yaml(path)

    # The a55_cluster wasn't declared, so it picks up the V2N101
    # topology default: os: yocto + app: alp-image-edge.
    a55 = project.cores["a55_cluster"]
    assert a55.os == "yocto"
    assert a55.app == "alp-image-edge"
    assert a55.image is None
    assert a55.machine == "e1m-v2n101-a55"

    m33 = project.cores["m33_sm"]
    assert m33.os == "zephyr"
    assert m33.app == "./m33"


# ---------------------------------------------------------------------
# 3. Loader rejects v1 top-level `os:`
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_v1_os(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V1_REJECT)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    # The schema says `not: required: [os]`, so the message should
    # mention the violation.  We accept any clear failure.
    assert "schema" in str(excinfo.value).lower() or \
           "os" in str(excinfo.value).lower()


# ---------------------------------------------------------------------
# 4. Loader rejects unknown core id
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_unknown_core(tmp_path: Path) -> None:
    path = _write_board(tmp_path, UNKNOWN_CORE)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m99_garbage" in msg
    # Phase B gap fix G-4: error now points at the SoM SKU's
    # `topology:` (the customer-actionable surface) and offers a
    # "did you mean" hint listing the preset's actual core keys.
    assert "topology" in msg
    assert "did you mean" in msg.lower()


# ---------------------------------------------------------------------
# 4b. Phase B gap fix G-4: cross-class `som.sku:` swap diagnostic
# ---------------------------------------------------------------------


# Customer kept the AEN-shaped `cores.m55_hp:` block but swapped
# `som.sku:` to E1M-NX9101 (i.MX 93 topology: m33 + a55_cluster, no
# m55_hp).  Pre-fix the orchestrator silently dropped the m55_hp
# entry; the customer got an empty slice with no diagnostic.
G4_CROSS_CLASS_SWAP = """
som:
  sku: E1M-NX9101

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [i2c]
"""


# Customer remembered to rename one core (m33) but forgot the second
# (m55_hp).  m33 builds; m55_hp triggers a soft WARN so the customer
# notices the drop without losing the working m33 slice.
G4_PARTIAL_MATCH = """
som:
  sku: E1M-NX9101

cores:
  m33:
    os: zephyr
    app: ./m33
    peripherals: [i2c]
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [spi]
"""


def test_unknown_cores_key_raises(tmp_path: Path) -> None:
    """G-4 hard-fail: NO `cores:` key matches `topology:`."""
    path = _write_board(tmp_path, G4_CROSS_CLASS_SWAP)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "did you mean" in msg.lower()
    assert "m33" in msg
    assert "a55_cluster" in msg
    assert "E1M-NX9101" in msg
    assert "topology" in msg


def test_partial_match_warns(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """G-4 soft-warn: SOME `cores:` keys match, SOME don't.  The
    matching keys still build slices; the dropped keys emit a stderr
    WARN so the customer sees what was discarded."""
    path = _write_board(tmp_path, G4_PARTIAL_MATCH)
    project = load_board_yaml(path)

    captured = capsys.readouterr()
    assert "m55_hp" in captured.err
    assert "WARN" in captured.err
    assert "E1M-NX9101" in captured.err
    # The matching `m33` slice still builds.
    assert "m33" in project.cores
    assert project.cores["m33"].app == "./m33"
    # The unmatched key is NOT in the slice map (silently dropped by
    # the soc_core_ids loop, but the WARN above made it visible).
    assert "m55_hp" not in project.cores


# ---------------------------------------------------------------------
# 5. resolve_carve_outs happy path
# ---------------------------------------------------------------------


def test_resolve_carve_outs_happy(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    carve_outs = resolve_carve_outs(project)
    assert len(carve_outs) == 1
    c = carve_outs[0]
    assert c.name == "alp_default_rpmsg"
    # ocram_low (size 512 KiB, non-cacheable, both cores).  Default
    # is non-cacheable so the resolver should pick ocram_low over
    # ddr_main.
    assert c.region == "ocram_low"
    # Page-aligned, top-down from ocram_low top.
    # ocram_low: base=0x00010000, size=512 KiB -> top=0x00090000.
    # carve=512 KiB page-aligned = 0x80000.  new_top = 0x00090000 - 0x80000 = 0x00010000.
    assert c.base == 0x00010000
    assert c.size == 0x80000

    # Endpoint IDs computed via FNV-1a(name).low_byte | 0x400.
    # We don't hard-code the hash digit here; we just assert the
    # masking is correct and the channel is reserved on the
    # V2N101 preset.
    assert (c.src_ept & 0xFFFFFF00) == 0x400
    assert c.dst_ept == c.src_ept + 1
    assert c.mailbox_channel == 0       # V2N101 channel 0 is alp_default_rpmsg


# ---------------------------------------------------------------------
# 6. resolve_carve_outs emits a blocked entry on TBD metadata
# ---------------------------------------------------------------------


def test_resolve_carve_outs_blocks_on_tbd(tmp_path: Path) -> None:
    """When the SoM preset still carries TBD metadata (mailbox /
    memory_map), resolve_carve_outs MUST return a `status: blocked`
    entry rather than raise.  The manifest stays emit-able so CI can
    see the gap; the actual slice-build step trips on the C header's
    `#error` directive."""
    path = _write_board(tmp_path, AEN_TBD)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "TBD" in entry.reason
    assert "E1M-AEN701" in entry.reason


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
    # image).  The manifest carries the chip slug + firmware_path
    # + flash_method verbatim.
    helper_names = [h["name"] for h in parsed["helper_mcus"]]
    assert "gd32_bridge" in helper_names
    gd32 = next(h for h in parsed["helper_mcus"]
                if h["name"] == "gd32_bridge")
    assert gd32["chip"] == "gd32g553"
    assert gd32["flash_method"] == "swd_v2n_host"


# ---------------------------------------------------------------------
# 10. Orchestrator.fan_out end-to-end with missing tools
# ---------------------------------------------------------------------


def test_orchestrator_fan_out_skips_when_tools_absent(
    tmp_path: Path, monkeypatch
) -> None:
    """On a host where neither bitbake nor west exists, every slice
    must end up `status: skipped` (not failed) and the manifest must
    be well-formed."""
    # Stub `shutil.which` to claim neither tool is on PATH.
    import alp_orchestrate
    monkeypatch.setattr(alp_orchestrate.shutil, "which",
                        lambda name: None)

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root)
    manifest = orch.fan_out(parallel=False)

    # Manifest reflects two slices, both skipped (tools missing).
    statuses = {s.core_id: s.status for s in manifest.slices}
    assert statuses["a55_cluster"] == "skipped"
    assert statuses["m33_sm"] == "skipped"

    # Reasons are descriptive (mention the missing tool).
    a55 = next(s for s in manifest.slices if s.core_id == "a55_cluster")
    m33 = next(s for s in manifest.slices if s.core_id == "m33_sm")
    assert "bitbake" in (a55.reason or "")
    assert "west" in (m33.reason or "")

    # Shared artefacts materialised.  The IPC header lands under
    # `alp/` so consumer code uses `#include <alp/system_ipc.h>`
    # canonically (build_root/generated ends up on the include path).
    assert (build_root / "generated" / "alp" / "system_ipc.h").is_file()
    assert (build_root / "generated" / "dts-reservations.dtsi").is_file()
    assert (build_root / "system-manifest.yaml").is_file()

    # State cache: no slice was ok, so nothing should be persisted as
    # `status: ok` in the cache file.  The cache file may or may not
    # exist; if it does, it should be empty / have no `ok` entries.
    if orch.state_path.is_file():
        import json
        state = json.loads(orch.state_path.read_text(encoding="utf-8"))
        for entry in state.values():
            assert entry.get("status") != "ok" or entry.get("hash") is None


# ---------------------------------------------------------------------
# Bonus -- determinism check (spec §6.1: re-runs produce byte-identical
# layouts).
# ---------------------------------------------------------------------


def test_resolve_carve_outs_deterministic(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project_a = load_board_yaml(path)
    project_b = load_board_yaml(path)

    a = resolve_carve_outs(project_a)
    b = resolve_carve_outs(project_b)
    assert len(a) == len(b)
    for ca, cb in zip(a, b):
        assert ca.base == cb.base
        assert ca.size == cb.size
        assert ca.src_ept == cb.src_ept
        assert ca.dst_ept == cb.dst_ept


# ---------------------------------------------------------------------
# Phase 3 follow-up tests
# ---------------------------------------------------------------------


def test_resolve_carve_outs_blocks_on_no_reserved_channel(
    tmp_path: Path, monkeypatch
) -> None:
    """Phase 3 strict mailbox-reservation check (spec §6.4), Phase 4
    blocked-entry semantics.

    Synthesises a V2N-like preset whose mailbox channels DON'T
    reserve a channel for alp_default_rpmsg, then loads + resolves
    a project whose ipc[] declares a `kind: rpmsg` entry.  The
    resolver MUST emit a blocked entry rather than silently fall
    back to channel 0.
    """
    import alp_orchestrate

    # Compose the synthetic SoM preset on a scratch metadata root.
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "renesas" / "rzv2n"
    schemas = meta / "schemas"
    for d in (e1m, socs, schemas):
        d.mkdir(parents=True)

    # Symlink / copy the v2 board-config schema + SoC + som-preset
    # schemas from the real repo so the validator finds them.
    import shutil
    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs / "n44.json")

    preset = e1m / "E1M-V2N101.yaml"
    preset.write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-V2N101
        family: renesas-rzv2n
        silicon: renesas:rzv2n:n44
        topology:
          a55_cluster:
            os: yocto
            app: alp-image-edge
            machine: e1m-v2n101-a55
            toolchain: poky-glibc
          m33_sm:
            os: zephyr
            app: alp-stock-shim
            board: alp_e1m_v2n101_m33_sm
            toolchain: arm-zephyr-eabi
        memory_map:
          - { name: ocram_low, base: 0x00010000, size_kib: 512, accessible_from: [a55_cluster, m33_sm], cacheable: false }
        mailbox:
          controller: renesas_mhu
          channels:
            - { id: 0, reserved_for: app }
            - { id: 1, reserved_for: power_mgmt }
        default_hw_rev:  r1
        default_board: E1M-X-EVK
    """).lstrip("\n"), encoding="utf-8")

    # Patch the orchestrator's METADATA_ROOT for this test.
    monkeypatch.setattr(alp_orchestrate, "METADATA_ROOT", meta)
    monkeypatch.setattr(alp_orchestrate, "BOARD_SCHEMA",
                        schemas / "board.schema.json")

    path = _write_board(tmp_path, V2N_HAPPY)
    project = alp_orchestrate.load_board_yaml(path, metadata_root=meta)
    resolved = resolve_carve_outs(project)
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "alp_default_rpmsg" in entry.reason
    assert "E1M-V2N101" in entry.reason


def test_emit_system_manifest_populates_helper_mcus(tmp_path: Path) -> None:
    """Phase 3 helper-MCU population.

    V2N101's preset declares one helper_firmware entry (gd32_bridge);
    the manifest must carry the chip slug + firmware_path + flash_method
    verbatim.
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
    assert gd32["firmware_path"] == \
        "firmware/gd32-bridge/build/gd32_bridge.bin"
    assert gd32["flash_method"] == "swd_v2n_host"
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
    assert m33["flash_args"]["runner"] == "openocd"


# ---------------------------------------------------------------------
# SoM-intrinsic chip driver auto-enable (Phase 4 on_module fix)
# Tests cover: _slugs_from_on_module, _slugs_from_helper_firmware,
# and _slice_alp_conf integration for V2N101 / AEN701 / NX9101.
# ---------------------------------------------------------------------


def test_slugs_from_on_module_v2n101() -> None:
    """V2N101 on_module: all non-TBD scalar chip slugs extracted;
    tps628640 (assembled: optional) is excluded; silicon field excluded."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-V2N101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    # Expected on-module chips (non-optional).
    for expected in ("act8760", "clk_5l35023b", "da9292", "eeprom_24c128",
                     "gd32g553", "murata_lbee5hy2fy", "optiga_trust_m",
                     "rtl8211fdi", "rv3028c7", "tmp112"):
        assert expected in slugs, f"missing expected slug: {expected}"

    # These must NOT appear.
    assert "tps628640" not in slugs, "optional assembled device must be excluded"
    assert "renesas:rzv2n:n44" not in slugs, "silicon field must be excluded"
    assert "TBD" not in slugs, "TBD values must be excluded"


def test_slugs_from_on_module_aen701() -> None:
    """AEN701 on_module: cc3501e, optiga_trust_m, rv3028c7, tmp112,
    eeprom_24c128 present; TBD ospi entries excluded."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-AEN701.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    for expected in ("cc3501e", "eeprom_24c128", "optiga_trust_m",
                     "rv3028c7", "tmp112"):
        assert expected in slugs, f"missing expected slug: {expected}"

    assert "TBD" not in slugs, "TBD values must be excluded"
    assert "alif:ensemble:e7" not in slugs, "silicon field must be excluded"


def test_slugs_from_on_module_nx9101_tbd_filtered() -> None:
    """NX9101 on_module: TBD wifi_ble and ethernet_phy are filtered out;
    only pca9451a (the one non-TBD scalar chip) survives."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-NX9101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    assert "pca9451a" in slugs, "pca9451a must be present"
    assert "TBD" not in slugs, "TBD values must be excluded"
    # NX9101 has no i2c_devices or ospi_memories, so the list is short.
    for slug in slugs:
        assert slug != "TBD"


def test_slugs_from_helper_firmware_v2n101() -> None:
    """V2N101 helper_firmware: gd32g553 chip slug extracted."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-V2N101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert "gd32g553" in slugs


def test_slugs_from_helper_firmware_aen701_tbd_filtered() -> None:
    """AEN701 helper_firmware: firmware_path is TBD but chip cc3501e
    is still a valid slug (chip: field is not TBD)."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-AEN701.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert "cc3501e" in slugs


def test_slugs_from_helper_firmware_nx9101_empty() -> None:
    """NX9101 has no helper MCUs; helper_firmware: [] returns empty list."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-NX9101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert slugs == []


def _make_som_only_project(tmp_path: Path, sku_yaml_content: str,
                           board_yaml_content: str,
                           sku: str = "E1M-TST001") -> "BoardProject":
    """Build a minimal BoardProject from an inline SoM preset + board.yaml.

    Creates a throwaway metadata root under tmp_path, writes the supplied
    preset YAML as the SoM file, and loads a board.yaml with no board.
    The board.schema schema copy has its ``som.sku`` pattern relaxed to
    also accept ``E1M-TST*`` names used by fixture tests.  The renesas n44
    SoC JSON is copied so presets that reference ``renesas:rzv2n:n44`` can
    resolve without the full repo metadata tree.
    """
    import json as _json
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    schemas = meta / "schemas"
    socs_v2n = meta / "socs" / "renesas" / "rzv2n"
    for d in (e1m, schemas, socs_v2n):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    # Copy the board-config schema and relax the sku pattern so synthetic
    # E1M-TST* SKUs used by fixture tests validate without error.
    bc_schema_text = (real_meta / "schemas" / "board.schema.json"
                      ).read_text(encoding="utf-8")
    bc_schema = _json.loads(bc_schema_text)
    bc_schema["properties"]["som"]["properties"]["sku"]["pattern"] = (
        r"^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3}|TST[0-9]{3})$"
    )
    (schemas / "board.schema.json").write_text(
        _json.dumps(bc_schema), encoding="utf-8")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # Copy the renesas n44 SoC JSON so silicon refs resolve in the temp root.
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs_v2n / "n44.json")

    (e1m / f"{sku}.yaml").write_text(
        textwrap.dedent(sku_yaml_content).lstrip("\n"), encoding="utf-8")
    board_path = tmp_path / "board.yaml"
    board_path.write_text(
        textwrap.dedent(board_yaml_content).lstrip("\n"), encoding="utf-8")

    return alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)


_SYNTHETIC_V2N_WITH_ON_MODULE = """\
    schema_version: 1
    sku: E1M-TST001
    family: renesas-rzv2n
    silicon: renesas:rzv2n:n44
    silicon_variant: R9A09G056N44GBG
    on_module:
      silicon:            renesas:rzv2n:n44
      pmic_main:          act8760
      rtc_external:       rv3028c7
      secure_element:     optiga_trust_m
      supervisor_mcu:     gd32g553
    helper_firmware:
      - name: gd32_bridge
        chip: gd32g553
        firmware_path: firmware/gd32-bridge/build/gd32_bridge.bin
        flash_method:  swd_v2n_host
        flash_args:
          interface: cmsis-dap
          target: gd32g553
          base: "0x08000000"
    topology:
      a55_cluster:
        app: alp-image-edge
        machine: e1m-tst001-a55
        toolchain: poky-glibc
      m33_sm:
        app: alp-stock-shim
        board: alp_e1m_tst001_m33_sm
        toolchain: arm-zephyr-eabi
    default_hw_rev: r1
    default_board: E1M-EVK
"""

_BOARD_WITH_SOM_ONLY = """\
    som:
      sku: E1M-TST001
      hw_rev: r1
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
"""


def test_slice_alp_conf_emits_som_intrinsic_chips(tmp_path: Path) -> None:
    """_slice_alp_conf must include CONFIG_ALP_SDK_CHIP_* for every chip
    derived from on_module: + helper_firmware: when no board is present."""
    project = _make_som_only_project(
        tmp_path,
        _SYNTHETIC_V2N_WITH_ON_MODULE,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # All four on-module chip slugs must appear.
    assert "CONFIG_ALP_SDK_CHIP_ACT8760=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_RV3028C7=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_GD32G553=y" in conf

    # The SoM-intrinsic comment header must appear.
    assert "SoM-intrinsic chip drivers" in conf

    # Subsystems driven by on-module chips (rv3028c7, optiga_trust_m,
    # act8760 are all I2C devices).
    assert "CONFIG_I2C=y" in conf


def test_slice_alp_conf_deduplicate_som_vs_board(tmp_path: Path) -> None:
    """A chip listed in both on_module: and board populated: must appear
    exactly once in the emitted conf (no duplicate CONFIG lines)."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    schemas = meta / "schemas"
    boards = meta / "boards"
    for d in (e1m, schemas, boards):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    # Relax the sku pattern so E1M-TST002 validates.
    import json as _json2
    bc_schema_text = (real_meta / "schemas" / "board.schema.json"
                      ).read_text(encoding="utf-8")
    bc_schema = _json2.loads(bc_schema_text)
    bc_schema["properties"]["som"]["properties"]["sku"]["pattern"] = (
        r"^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3}|TST[0-9]{3})$"
    )
    (schemas / "board.schema.json").write_text(
        _json2.dumps(bc_schema), encoding="utf-8")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # Copy SoC JSON so silicon ref renesas:rzv2n:n44 resolves in temp root.
    socs_v2n = meta / "socs" / "renesas" / "rzv2n"
    socs_v2n.mkdir(parents=True, exist_ok=True)
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs_v2n / "n44.json")

    # SoM preset lists rv3028c7 as on-module.
    (e1m / "E1M-TST002.yaml").write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-TST002
        family: renesas-rzv2n
        silicon: renesas:rzv2n:n44
        silicon_variant: R9A09G056N44GBG
        on_module:
          silicon:        renesas:rzv2n:n44
          rtc_external:   rv3028c7
        helper_firmware: []
        topology:
          a55_cluster:
            app: alp-image-edge
            machine: e1m-tst002-a55
            toolchain: poky-glibc
          m33_sm:
            app: alp-stock-shim
            board: alp_e1m_tst002_m33_sm
            toolchain: arm-zephyr-eabi
        default_hw_rev: r1
        default_board: E1M-EVK
    """).lstrip("\n"), encoding="utf-8")

    # Board preset also lists rv3028c7 in populated:.
    (boards / "e1m-evk.yaml").write_text(textwrap.dedent("""
        name: E1M-EVK
        populated:
          rv3028c7: true
          bmi323: true
    """).lstrip("\n"), encoding="utf-8")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-TST002
          hw_rev: r1
        preset: e1m-evk
        cores:
          m33_sm:
            os: zephyr
            app: ./m33
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # rv3028c7 must appear exactly once.
    count = conf.count("CONFIG_ALP_SDK_CHIP_RV3028C7=y")
    assert count == 1, (
        f"rv3028c7 appears {count} times; expected exactly 1 (deduplicated)")

    # bmi323 is board-only; it must still appear.
    assert "CONFIG_ALP_SDK_CHIP_BMI323=y" in conf


def test_slice_alp_conf_tbd_values_excluded(tmp_path: Path) -> None:
    """on_module entries with value TBD must NOT generate CONFIG lines."""
    project = _make_som_only_project(
        tmp_path,
        """\
            schema_version: 1
            sku: E1M-TST001
            family: renesas-rzv2n
            silicon: renesas:rzv2n:n44
            silicon_variant: R9A09G056N44GBG
            on_module:
              silicon:      renesas:rzv2n:n44
              wifi_ble:     TBD
              ethernet_phy: TBD
              pmic_main:    act8760
            helper_firmware: []
            topology:
              a55_cluster:
                app: alp-image-edge
                machine: e1m-tst001-a55
                toolchain: poky-glibc
              m33_sm:
                app: alp-stock-shim
                board: alp_e1m_tst001_m33_sm
                toolchain: arm-zephyr-eabi
            default_hw_rev: r1
            default_board: E1M-EVK
        """,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    assert "CONFIG_ALP_SDK_CHIP_TBD" not in conf, "TBD must never be emitted"
    assert "CONFIG_ALP_SDK_CHIP_ACT8760=y" in conf


def test_slice_alp_conf_no_on_module_no_som_block(tmp_path: Path) -> None:
    """A SoM preset without on_module: must emit no SoM-intrinsic chip block
    (no regression on the synthetic presets used by other tests)."""
    project = _make_som_only_project(
        tmp_path,
        """\
            schema_version: 1
            sku: E1M-TST001
            family: renesas-rzv2n
            silicon: renesas:rzv2n:n44
            silicon_variant: R9A09G056N44GBG
            topology:
              a55_cluster:
                app: alp-image-edge
                machine: e1m-tst001-a55
                toolchain: poky-glibc
              m33_sm:
                app: alp-stock-shim
                board: alp_e1m_tst001_m33_sm
                toolchain: arm-zephyr-eabi
            default_hw_rev: r1
            default_board: E1M-EVK
        """,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    assert "SoM-intrinsic chip drivers" not in conf


def test_slice_alp_conf_real_v2n101(tmp_path: Path) -> None:
    """End-to-end: loading real E1M-V2N101 preset produces CONFIG lines
    for its on-module chip set and does not include TBD or silicon strings."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "renesas" / "rzv2n"
    schemas = meta / "schemas"
    for d in (e1m, socs, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs / "n44.json")
    shutil.copy(real_meta / "e1m_modules" / "E1M-V2N101.yaml",
                e1m / "E1M-V2N101.yaml")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-V2N101
          hw_rev: r1
        cores:
          m33_sm:
            os: zephyr
            app: ./m33
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # Core V2N101 on-module chips.
    for chip in ("gd32g553", "optiga_trust_m", "rv3028c7", "tmp112",
                 "eeprom_24c128", "act8760", "da9292", "murata_lbee5hy2fy"):
        assert f"CONFIG_ALP_SDK_CHIP_{chip.upper()}=y" in conf, (
            f"missing CONFIG_ALP_SDK_CHIP_{chip.upper()}=y")

    # tps628640 is assembled: optional -- must NOT appear.
    assert "CONFIG_ALP_SDK_CHIP_TPS628640" not in conf

    # I2C subsystem from the many I2C chips.
    assert "CONFIG_I2C=y" in conf

    # No raw silicon string or TBD strings should appear in chip lines.
    for line in conf.splitlines():
        if line.startswith("CONFIG_ALP_SDK_CHIP_"):
            assert "TBD" not in line
            assert "RENESAS" not in line  # silicon slug must not appear


def test_slice_alp_conf_real_aen701(tmp_path: Path) -> None:
    """End-to-end: loading real E1M-AEN701 preset emits cc3501e, optiga_trust_m,
    rv3028c7, tmp112, eeprom_24c128; TBD ospi entries absent."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs_alif = meta / "socs" / "alif" / "ensemble"
    schemas = meta / "schemas"
    for d in (e1m, socs_alif, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # AEN SoC JSON for capability resolution.
    real_soc_dir = real_meta / "socs" / "alif" / "ensemble"
    if real_soc_dir.is_dir():
        for soc_f in real_soc_dir.iterdir():
            shutil.copy(soc_f, socs_alif / soc_f.name)
    shutil.copy(real_meta / "e1m_modules" / "E1M-AEN701.yaml",
                e1m / "E1M-AEN701.yaml")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m55_slice = project.cores["m55_hp"]
    conf = _slice_alp_conf(project, m55_slice)

    for chip in ("cc3501e", "optiga_trust_m", "rv3028c7", "tmp112",
                 "eeprom_24c128"):
        assert f"CONFIG_ALP_SDK_CHIP_{chip.upper()}=y" in conf, (
            f"missing CONFIG_ALP_SDK_CHIP_{chip.upper()}=y for AEN701")

    assert "CONFIG_ALP_SDK_CHIP_TBD" not in conf
    assert "SoM-intrinsic chip drivers" in conf


# ---------------------------------------------------------------------
# v0.6 P2.1 -- extra_libraries: escape hatch
# ---------------------------------------------------------------------


_V2N_BASE_FOR_EXTRA = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
{extra}
"""


def _v2n_with_extra(extra_yaml: str) -> str:
    """Build a V2N101 board.yaml with the given extra_libraries block
    (indented two spaces under the m33_sm core)."""
    return _V2N_BASE_FOR_EXTRA.format(extra=extra_yaml)


def test_extra_libraries_inline_kconfig_happy(tmp_path: Path) -> None:
    """An entry with `name:` + `kconfig:` loads cleanly and lands in
    the slice's emitted alp.conf verbatim."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib\n"
        "        include_path: third_party/mylib/include\n"
        "        kconfig:\n"
        "          - CONFIG_MYLIB=y\n"
        "          - CONFIG_MYLIB_FEATURE_X=y\n")
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    slice_ = project.cores["m33_sm"]
    assert len(slice_.extra_libraries) == 1
    assert slice_.extra_libraries[0]["name"] == "mylib"
    conf = _slice_alp_conf(project, slice_)
    assert "extra_libraries[mylib]" in conf
    assert "CONFIG_MYLIB=y" in conf
    assert "CONFIG_MYLIB_FEATURE_X=y" in conf


def test_extra_libraries_profile_happy(tmp_path: Path) -> None:
    """An entry with `name:` + `profile:` resolves the profile file
    and emits its accelerators / sw_fallback Kconfig per the same
    silicon / soc_family / requires_cap matcher used by curated libs."""
    # Profile file lives under metadata/library-profiles/ -- mbedtls
    # ships with one out of the box, and it carries an `optiga_trust_m`
    # priority entry plus an sw_fallback that always matches.  Reuse
    # it as the test stub.
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_profile\n"
        "        profile: metadata/library-profiles/mbedtls/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    slice_ = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, slice_)
    assert "extra_libraries[mylib_profile]" in conf
    # V2N101 has optiga_trust_m capability + cau capability;
    # _emit_extra_library_profile is deterministic in priority order.
    # mbedtls profile's first-matching V2N entry is `cau` (priority
    # ordering); regardless, *some* CONFIG_ALP_MBEDTLS_* must land,
    # and the sw_fallback line is always emitted.
    assert "CONFIG_ALP_MBEDTLS_PURE_C=y" in conf
    assert "sw_fallback" in conf


def test_extra_libraries_both_kconfig_and_profile_rejected(
    tmp_path: Path,
) -> None:
    """An entry declaring BOTH `kconfig:` and `profile:` must fail
    the cross-field validator (the exactly-one rule)."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_bad\n"
        "        kconfig: [CONFIG_X=y]\n"
        "        profile: metadata/library-profiles/mbedtls/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mylib_bad" in msg
    assert "exactly one" in msg
    assert "both" in msg.lower()


def test_extra_libraries_neither_kconfig_nor_profile_rejected(
    tmp_path: Path,
) -> None:
    """An entry declaring NEITHER `kconfig:` nor `profile:` must
    fail the cross-field validator (the exactly-one rule)."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_empty\n"
        "        include_path: third_party/foo/include\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mylib_empty" in msg
    assert "exactly one" in msg
    assert "neither" in msg.lower()


def test_extra_libraries_name_collides_with_curated(tmp_path: Path) -> None:
    """A name that matches the curated `libraries:` enum (e.g.
    `mbedtls`) must be rejected -- the escape hatch is for
    non-curated entries only."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mbedtls\n"
        "        kconfig: [CONFIG_FOO=y]\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mbedtls" in msg
    assert "curated" in msg.lower()


def test_extra_libraries_name_collides_across_cores(tmp_path: Path) -> None:
    """Names must be globally unique across all cores' extra_libraries."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    extra_libraries:
      - name: shared_slug
        kconfig: [CONFIG_X=y]
  m33_sm:
    os: zephyr
    app: ./m33
    extra_libraries:
      - name: shared_slug
        kconfig: [CONFIG_Y=y]
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "shared_slug" in msg
    assert "globally unique" in msg.lower() or "collides" in msg.lower()


def test_extra_libraries_profile_file_missing(tmp_path: Path) -> None:
    """A `profile:` path that doesn't resolve to a file must fail."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: phantom\n"
        "        profile: metadata/library-profiles/does-not-exist/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "phantom" in msg
    assert "does not resolve" in msg


# ---------------------------------------------------------------------
# v0.6 P2.3 -- cross-field validator pass
# ---------------------------------------------------------------------


def test_consistency_mender_without_yocto_rejected(tmp_path: Path) -> None:
    """Rule 1: ota.provider: mender on an all-Zephyr project must fail.

    AEN701 has three cores (a32_cluster + m55_hp + m55_he); explicitly
    set the A-class core to `off` so no slice ends up yocto -- the
    rule triggers on the absence of any yocto slice."""
    body = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: "off"

ota:
  provider: mender
  artifact_name: alp-aen-test
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mender" in msg
    assert "yocto" in msg


def test_consistency_mender_with_yocto_ok(tmp_path: Path) -> None:
    """Rule 1 happy path: V2N (A55 yocto + M33 zephyr) with mender OK."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: zephyr
    app: ./m33

ota:
  provider: mender
  artifact_name: alp-v2n-test
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert project.ota.get("provider") == "mender"


def test_consistency_boot_signing_unsupported_for_family(
    tmp_path: Path,
) -> None:
    """Rule 2: AEN family with rsa2048 must fail (AEN only supports
    ECDSA-P256 + ed25519 under the OPTIGA Trust M attestation flow)."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

boot:
  method: mcuboot
  signing:
    algorithm: rsa2048
    key_file: keys/dev_rsa.pem
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "rsa2048" in msg
    assert "alif-ensemble" in msg
    assert "ecdsa_p256" in msg or "ed25519" in msg


def test_consistency_boot_signing_supported_aen_ecdsa(tmp_path: Path) -> None:
    """Rule 2 happy path: AEN + ecdsa_p256 OK."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/dev_ecdsa_p256.pem
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert (project.boot.get("signing") or {}).get("algorithm") == "ecdsa_p256"


def test_consistency_boot_signing_supported_v2n_rsa(tmp_path: Path) -> None:
    """Rule 2: V2N family accepts rsa2048 (different SoM family)."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33

boot:
  method: mcuboot
  signing:
    algorithm: rsa2048
    key_file: keys/dev_rsa.pem
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert (project.boot.get("signing") or {}).get("algorithm") == "rsa2048"


def test_consistency_tls_without_provider_rejected(tmp_path: Path) -> None:
    """Rule 3: iot.tls: true with no mbedtls / bearssl in libraries
    or extra_libraries must fail."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m33_sm" in msg
    assert "tls" in msg.lower()
    assert "mbedtls" in msg
    assert "bearssl" in msg


def test_consistency_tls_satisfied_by_curated_mbedtls(tmp_path: Path) -> None:
    """Rule 3 happy path: mbedtls in `libraries:` covers iot.tls."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    libraries: [mbedtls]
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "mbedtls" in project.cores["m33_sm"].libraries


def test_consistency_tls_satisfied_by_curated_bearssl(tmp_path: Path) -> None:
    """Rule 3: `bearssl` declared via the curated `libraries:` enum
    also satisfies iot.tls.  (bearssl + mbedtls are the two TLS
    providers rule 3 accepts.)"""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    libraries: [bearssl]
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "bearssl" in project.cores["m33_sm"].libraries


def test_consistency_arena_larger_than_heap_warns(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 4: arena_kib > heap_kib emits a WARN, but doesn't fail."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    memory: { heap_kib: 32 }
    inference: { default_arena_kib: 128 }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    err = capsys.readouterr().err
    assert "WARN" in err
    assert "default_arena_kib=128" in err
    assert "heap_kib=32" in err
    # Project loaded successfully (warning, not error).
    assert project.cores["m33_sm"].inference["default_arena_kib"] == 128


def test_consistency_arena_within_heap_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 4 happy path: arena fits in heap; no WARN emitted."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    memory: { heap_kib: 256 }
    inference: { default_arena_kib: 128 }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    # Rule 4 should not emit a WARN; G-4 partial-match WARN unrelated.
    assert "default_arena_kib" not in err


def test_consistency_sleep_mode_without_wakeup_warns(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5: sleep_mode != disabled without wakeup_sources emits a WARN."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power: { sleep_mode: deep }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "WARN" in err
    assert "sleep_mode=deep" in err
    assert "wakeup_sources" in err


def test_consistency_sleep_mode_with_wakeup_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5 happy path: sleep_mode with wakeup_sources declared -- no WARN."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power:
      sleep_mode: standby
      wakeup_sources: [uart, gpio]
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "sleep_mode" not in err


def test_consistency_sleep_mode_disabled_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5: explicit `sleep_mode: disabled` (the default) doesn't WARN
    even when wakeup_sources is empty -- the device isn't sleeping."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power: { sleep_mode: disabled }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "sleep_mode" not in err

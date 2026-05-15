# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate.py (Phase 2 of the
2026-05-15 heterogeneous-OS orchestration design).

Run locally:

    python -m pytest tests/scripts/test_alp_orchestrate.py -v
"""

from __future__ import annotations

import re
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
schema_version: 2

som:
  sku: E1M-V2N101
  hw_rev: r1

carrier:
  name: E1M-X-EVK

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
    inference:   { backend: cpu }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""


V2N_TOPOLOGY_FALLBACK = """
schema_version: 2

som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


V1_REJECT = """
schema_version: 2

som:
  sku: E1M-V2N101

os: zephyr

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


UNKNOWN_CORE = """
schema_version: 2

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
schema_version: 2

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
    assert project.carrier_name == "E1M-X-EVK"
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
    assert "unknown core ID" in msg or "core IDs" in msg


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
# 6. resolve_carve_outs raises on TBD
# ---------------------------------------------------------------------


def test_resolve_carve_outs_raises_on_tbd(tmp_path: Path) -> None:
    path = _write_board(tmp_path, AEN_TBD)
    project = load_board_yaml(path)
    with pytest.raises(OrchestratorError) as excinfo:
        resolve_carve_outs(project)
    msg = str(excinfo.value)
    assert "TBD" in msg
    assert "E1M-AEN701" in msg


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

    # Shared artefacts materialised.
    assert (build_root / "generated" / "alp_system_ipc.h").is_file()
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


def test_resolve_carve_outs_raises_on_no_reserved_channel(
    tmp_path: Path, monkeypatch
) -> None:
    """Phase 3 strict mailbox-reservation check (spec §6.4).

    Synthesises a V2N-like preset whose mailbox channels DON'T
    reserve a channel for alp_default_rpmsg, then loads + resolves
    a project whose ipc[] declares a `kind: rpmsg` entry.  The
    resolver MUST raise rather than silently fall back to channel 0.
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
    shutil.copy(real_meta / "schemas" / "board-config-v2.schema.json",
                schemas / "board-config-v2.schema.json")
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
        default_carrier: E1M-X-EVK
    """).lstrip("\n"), encoding="utf-8")

    # Patch the orchestrator's METADATA_ROOT for this test.
    monkeypatch.setattr(alp_orchestrate, "METADATA_ROOT", meta)
    monkeypatch.setattr(alp_orchestrate, "SCHEMA_V2",
                        schemas / "board-config-v2.schema.json")

    path = _write_board(tmp_path, V2N_HAPPY)
    project = alp_orchestrate.load_board_yaml(path, metadata_root=meta)
    with pytest.raises(OrchestratorError) as excinfo:
        resolve_carve_outs(project)
    msg = str(excinfo.value)
    assert "alp_default_rpmsg" in msg
    assert "E1M-V2N101" in msg


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

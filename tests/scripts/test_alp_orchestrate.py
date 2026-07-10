# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ (Phase 2 of the
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
    ResolvedPartition,
    Slice,
    StorageEntry,
    _slice_alp_conf,
    _slice_local_conf,
    _slugs_from_helper_firmware,
    _slugs_from_on_module,
    emit_dts_partitions,
    emit_dts_reservations,
    emit_ipc_contract_h,
    emit_storage_mounts_c,
    emit_system_manifest,
    emit_tfm_sysbuild_conf,
    load_board_yaml,
    resolve_carve_outs,
    resolve_storage_partitions,
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


# NX9101 still has TBD mailbox metadata; any ipc carve-out must
# surface that as a clear blocked entry.
NX_TBD = """
som:
  sku: E1M-NX9101

cores:
  m33:
    os: zephyr
    app: ./m33

ipc:
  - kind: rpmsg
    endpoints: [m33, a55_cluster]
    carve_out_kb: 64
    name: alp_test_rpmsg
"""


# AEN701/AEN801 resolve their mailbox controller (alif_mhuv2), so they
# sail past the controller-TBD guard -- but their memory map is derived
# from the SoC variant JSON, which carries no per-region `base` yet.
# Before the region.get("base") fix this crashed resolve_carve_outs
# with `KeyError: 'base'`; it MUST instead land a clean blocked
# carve-out.  Regression guard for that crash.
AEN701_UNMAPPED = """
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


AEN801_UNMAPPED = """
som:
  sku: E1M-AEN801

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
    assert m33.board == "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"      # inherited
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


def test_load_board_yaml_rejects_unknown_features_key(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features:
  ipc:
    framing: nanopb
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "features" in msg
    assert "ipc" in msg


def test_load_board_yaml_rejects_empty_features_block(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features: {}
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "features" in str(excinfo.value)


def test_load_board_yaml_rejects_board_preset_family_mismatch(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
        som:
          sku: E1M-V2N101

        preset: e1m-evk

        cores:
          m33_sm:
            app: ./src
    """)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "hosts SoM families" in str(excinfo.value)


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
    path = _write_board(tmp_path, NX_TBD)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "TBD" in entry.reason
    assert "E1M-NX9101" in entry.reason


@pytest.mark.parametrize(
    "body, sku",
    [
        (AEN701_UNMAPPED, "E1M-AEN701"),
        (AEN801_UNMAPPED, "E1M-AEN801"),
    ],
)
def test_resolve_carve_outs_blocks_on_unmapped_base(
    tmp_path: Path, body: str, sku: str
) -> None:
    """AEN presets have a RESOLVED mailbox controller (alif_mhuv2), so
    they proceed past the controller-TBD guard into the region allocator.
    Their stock memory maps are still base-unmapped, so resolve_carve_outs
    MUST emit a blocked carve-out rather than crash with `KeyError:
    'base'`."""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)        # must not raise
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert sku in entry.reason
    assert "HW-mapped" in entry.reason


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
    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
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
        "firmware/gd32-bridge/build/gd32/gd32-bridge.bin"
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
    eeprom_24c128 present; ospi_memories / hyperram storage MPNs are
    excluded (they have no chips/<part>/ driver)."""
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
    # Storage MPNs (OSPI NOR flash + HyperRAM) are NOT chip-driver slugs:
    # they have no chips/<part>/ driver, so emitting them as
    # CONFIG_ALP_SDK_CHIP_<X> would trip Zephyr's undefined-symbol guard.
    assert "MX25UM25645GXDI00" not in slugs, "OSPI flash MPN must not be a chip slug"
    assert "W958D8NBYA5I" not in slugs, "HyperRAM MPN must not be a chip slug"


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
        firmware_path: firmware/gd32-bridge/build/gd32/gd32-bridge.bin
        flash_method:  swd_probe
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


def test_slice_alp_conf_hw_info_eeprom_feature_overrides_defaults(
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
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m33_sm"])

    assert "features.hw_info.eeprom" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID=0" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT=0x54" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET=32" in conf


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


def test_slice_alp_conf_iot_aen_uses_cc3501e_provider(tmp_path: Path) -> None:
    """cores.*.iot resolves AEN Wi-Fi/BLE to the CC3501E bridge, while
    MQTT/TLS still emit the Zephyr protocol-library gates."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    libraries: [mbedtls]
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    assert "on_module.wifi_ble: cc3501e" in conf
    assert "CONFIG_ALP_SDK_CHIP_CC3501E=y" in conf
    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" in conf
    assert "CONFIG_MQTT_LIB=y" in conf
    assert "CONFIG_ALP_SDK_IOT_MQTT=y" in conf
    assert "CONFIG_TLS_CREDENTIALS=y" in conf
    assert "CONFIG_MQTT_LIB_TLS=y" in conf
    assert "CONFIG_MBEDTLS=y" in conf

    # AEN uses the exact bridge backend, not the generic Zephyr wifi_mgmt
    # or HCI paths.
    assert "CONFIG_ALP_SDK_IOT_WIFI=y" not in conf
    assert "CONFIG_ALP_SDK_BLE=y" not in conf


def test_slice_alp_conf_iot_unknown_provider_uses_generic_zephyr(
    tmp_path: Path,
) -> None:
    """A SoM whose wireless provider is still TBD emits the generic Zephyr
    networking / MQTT / TLS / BLE gates rather than a false provider."""
    body = """
som:
  sku: E1M-NX9101

cores:
  m33:
    os: zephyr
    app: ./m33
    libraries: [mbedtls]
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m33"])

    for expected in (
        "CONFIG_NETWORKING=y",
        "CONFIG_NET_IPV4=y",
        "CONFIG_NET_SOCKETS=y",
        "CONFIG_WIFI=y",
        "CONFIG_NET_MGMT=y",
        "CONFIG_NET_MGMT_EVENT=y",
        "CONFIG_NET_L2_WIFI_MGMT=y",
        "CONFIG_ALP_SDK_IOT_WIFI=y",
        "CONFIG_NET_TCP=y",
        "CONFIG_MQTT_LIB=y",
        "CONFIG_ALP_SDK_IOT_MQTT=y",
        "CONFIG_TLS_CREDENTIALS=y",
        "CONFIG_MQTT_LIB_TLS=y",
        "CONFIG_BT=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_ALP_SDK_BLE=y",
    ):
        assert expected in conf

    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" not in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" not in conf


def test_slice_local_conf_iot_v2n_murata_linux_handoff(tmp_path: Path) -> None:
    """V2N's Murata/CYW provider is Linux-owned: local.conf gets stable
    userland/runtime deps and leaves kernel/firmware to the BSP layer."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    libraries: [mbedtls]
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_local_conf(project, project.cores["a55_cluster"])

    assert "on_module.wifi_ble: murata_lbee5hy2fy" in conf
    assert "BSP/machine recipes supply kernel/firmware packages" in conf
    assert (
        'IMAGE_INSTALL:append = " wpa-supplicant iw wireless-regdb '
        'bluez5 ca-certificates"'
    ) in conf
    assert 'PACKAGECONFIG:append:pn-alp-sdk = " mqtt security"' in conf


# ---------------------------------------------------------------------
# Storage partition resolver + emitters (v0.6 schema-only gap closed).
# ---------------------------------------------------------------------


# AEN301's auto-derived memory_map includes `mram_main` (5632 KiB) from
# the Alif e3 variant.  Used as the flash device throughout the storage
# tests because MRAM is the realistic persistent-store target on the AEN
# family; V2N101 has no MRAM (DDR + OCRAM only) so it would be a
# semantically odd test fixture.
AEN_STORAGE = """
name: test-aen-storage
som:
  sku: E1M-AEN301
  hw_rev: r1

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

storage:
  - { name: settings,        size_kib: 64,  fs: littlefs, flash_device: mram_main, mount: /lfs/settings }
  - { name: app_data,        size_kib: 128, fs: littlefs, flash_device: mram_main, mount: /lfs/app }
  - { name: mcuboot_scratch, size_kib: 32,  fs: raw,      flash_device: mram_main }
"""


# Storage entry with an explicit offset override; combined with a
# bump-allocated sibling to exercise both code paths in one project.
AEN_STORAGE_OFFSET = """
name: test-aen-storage-offset
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

storage:
  - { name: bump_alloc, size_kib: 64, fs: raw, flash_device: mram_main }
  - { name: pinned_low, size_kib: 32, fs: raw, flash_device: mram_main, offset_kib: 4096 }
"""


def test_resolve_storage_partitions_happy(tmp_path: Path) -> None:
    """Three littlefs+raw partitions on mram_main allocate bottom-up,
    page-aligned, name-sorted for determinism."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 3
    # All three resolved OK against mram_main (auto-derived from
    # Alif e3 variant's mram_mb -> "mram_main" region).
    for p in parts:
        assert p.status == "ok", (p.name, p.reason)
        assert p.flash_device == "mram_main"
        assert p.dt_label == "mram_main"  # no dt_label override declared
    # Sorted alphabetically: app_data (0), mcuboot_scratch (128), settings (160).
    by_name = {p.name: p for p in parts}
    assert by_name["app_data"].base_kib == 0
    assert by_name["app_data"].size_kib == 128
    # Page-aligned: 128 KiB stays 128 KiB.
    assert by_name["mcuboot_scratch"].base_kib == 128
    assert by_name["mcuboot_scratch"].size_kib == 32
    assert by_name["settings"].base_kib == 160
    assert by_name["settings"].size_kib == 64


def test_resolve_storage_partitions_unknown_flash_device(
    tmp_path: Path,
) -> None:
    """A typoed flash_device: must surface as a loader error with the
    list of known devices in the message."""
    body = AEN_STORAGE.replace(
        "flash_device: mram_main, mount: /lfs/settings",
        "flash_device: not_a_real_region, mount: /lfs/settings",
        1,
    )
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="not_a_real_region"):
        load_board_yaml(path)


def test_resolve_storage_partitions_duplicate_name(
    tmp_path: Path,
) -> None:
    """Duplicate partition names within `storage:` must error eagerly."""
    body = """
    name: test-dup
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: dup, size_kib: 64, fs: raw, flash_device: mram_main }
      - { name: dup, size_kib: 64, fs: raw, flash_device: mram_main }
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="more than once"):
        load_board_yaml(path)


def test_resolve_storage_partitions_explicit_offset(
    tmp_path: Path,
) -> None:
    """offset_kib: must be honoured verbatim and not shift the bump
    allocator (so byte-stable layouts stay byte-stable)."""
    path = _write_board(tmp_path, AEN_STORAGE_OFFSET)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 2
    by_name = {p.name: p for p in parts}
    # Sort order is alphabetical: bump_alloc allocates first at 0;
    # pinned_low honours its explicit offset of 4096 KiB.
    assert by_name["bump_alloc"].status == "ok"
    assert by_name["bump_alloc"].base_kib == 0
    assert by_name["bump_alloc"].size_kib == 64
    assert by_name["pinned_low"].status == "ok"
    assert by_name["pinned_low"].base_kib == 4096
    assert by_name["pinned_low"].size_kib == 32


def test_resolve_storage_partitions_misaligned_offset(
    tmp_path: Path,
) -> None:
    """A page-misaligned offset_kib: must produce a `blocked` partition
    with a clear reason, not raise."""
    body = """
    name: test-misalign
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: bad, size_kib: 64, fs: raw, flash_device: mram_main, offset_kib: 5 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    assert "page-aligned" in (parts[0].reason or "")


def test_resolve_storage_partitions_overflow(tmp_path: Path) -> None:
    """A partition larger than the device's capacity must block."""
    body = """
    name: test-overflow
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: huge, size_kib: 1048576, fs: raw, flash_device: mram_main }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    assert "overruns" in (parts[0].reason or "")


def test_resolve_storage_partitions_overlap(tmp_path: Path) -> None:
    """Two explicit offsets that overlap must surface the offending pair."""
    body = """
    name: test-overlap
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: a, size_kib: 64, fs: raw, flash_device: mram_main, offset_kib: 0 }
      - { name: b, size_kib: 64, fs: raw, flash_device: mram_main, offset_kib: 32 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    # `a` allocates at offset 0 (0..64KiB), `b` overlaps via its
    # explicit offset 32 KiB; the resolver blocks `b` (alphabetically
    # later, processed after `a` is already in the allocated set).
    by_name = {p.name: p for p in parts}
    assert by_name["a"].status == "ok"
    assert by_name["b"].status == "blocked"
    assert "overlaps" in (by_name["b"].reason or "")


def test_resolve_storage_partitions_deterministic(tmp_path: Path) -> None:
    """Two identical projects must produce byte-identical resolutions."""
    p1 = _write_board(tmp_path, AEN_STORAGE, name="b1.yaml")
    p2 = _write_board(tmp_path, AEN_STORAGE, name="b2.yaml")
    r1 = resolve_storage_partitions(load_board_yaml(p1))
    r2 = resolve_storage_partitions(load_board_yaml(p2))
    assert [(p.name, p.base_kib, p.size_kib) for p in r1] == \
           [(p.name, p.base_kib, p.size_kib) for p in r2]


def test_emit_dts_partitions_shape(tmp_path: Path) -> None:
    path = _write_board(tmp_path, AEN_STORAGE)
    out = emit_dts_partitions(load_board_yaml(path))
    # Decorates the flash device's DT label.
    assert "&mram_main {" in out
    # Standard fixed-partitions binding.
    assert 'compatible = "fixed-partitions";' in out
    assert "#address-cells = <1>;" in out
    assert "#size-cells = <1>;" in out
    # One partition node per entry, with label + reg.
    for name in ("settings", "app_data", "mcuboot_scratch"):
        assert f"{name}_partition: partition@" in out
        assert f'label = "{name}";' in out
    # Address determinism: app_data is at offset 0 bytes; settings is
    # at 160 KiB = 0x28000 bytes; size 64 KiB = 0x10000 bytes.
    assert "app_data_partition: partition@0 {" in out
    assert "settings_partition: partition@28000 {" in out
    assert "reg = <0x28000 0x10000>;" in out


def test_emit_dts_partitions_no_storage(tmp_path: Path) -> None:
    """A board with no `storage:` block must emit the stub-only file
    (so downstream `#include` still resolves) without raising."""
    body = """
    name: test-no-storage
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    """
    path = _write_board(tmp_path, body)
    out = emit_dts_partitions(load_board_yaml(path))
    assert "No `storage:` entries" in out
    assert "fixed-partitions" not in out


def test_emit_storage_mounts_c_littlefs(tmp_path: Path) -> None:
    """The optional C mount table must declare an fs_mount_t per
    mountable partition + the global alp_storage_mounts[] array."""
    path = _write_board(tmp_path, AEN_STORAGE)
    out = emit_storage_mounts_c(load_board_yaml(path))
    assert "/* clang-format off */" in out
    assert "/* clang-format on */" in out
    # littlefs declarations for the two littlefs-formatted partitions.
    assert "FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(alp_lfs_settings)" in out
    assert "FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(alp_lfs_app_data)" in out
    # raw partition has no mount: it is omitted from the table.
    assert "alp_mnt_mcuboot_scratch" not in out
    # The aggregate array carries the two mountable partitions.
    assert "alp_storage_mount_count = 2;" in out


def test_slice_alp_conf_storage_kconfig(tmp_path: Path) -> None:
    """The Kconfig fragment must enable CONFIG_FILE_SYSTEM_LITTLEFS for
    every littlefs entry, plus a documentation comment per littlefs
    partition so the customer knows how to reach the partition at
    runtime (FIXED_PARTITION_ID(<name>_partition)).  The previous
    `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y` per-partition emit was a
    fictional Kconfig -- modern Zephyr's per-partition LFS wiring is
    DT-driven (`fixed-partitions` node + chosen
    `zephyr,storage-partition`), and setting the undefined symbol
    aborted twister with "assignment to undefined symbol"."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_FILE_SYSTEM=y" in conf
    assert "CONFIG_FILE_SYSTEM_LITTLEFS=y" in conf
    # Per-partition hint comments (no live CONFIG_* line -- see comment block
    # above for why).
    assert "partition[settings]" in conf
    assert "FIXED_PARTITION_ID(settings_partition)" in conf
    assert "partition[app_data]" in conf
    assert "FIXED_PARTITION_ID(app_data_partition)" in conf
    # Raw partition does NOT get a littlefs comment.
    assert "partition[mcuboot_scratch]" not in conf
    # Live CONFIG_FS_LITTLEFS_PARTITION_* lines must never come back --
    # they are the undefined-symbol form that aborts the build.
    for stem in ("SETTINGS", "APP_DATA", "MCUBOOT_SCRATCH"):
        assert f"\nCONFIG_FS_LITTLEFS_PARTITION_{stem}=" not in conf
    # FAT / EXT2 not pulled in (no entries declared them).
    assert "CONFIG_FAT_FILESYSTEM_ELM" not in conf
    assert "CONFIG_FILE_SYSTEM_EXT2" not in conf


def test_resolve_storage_partitions_blocks_on_tbd_ospi(
    tmp_path: Path,
) -> None:
    """A partition pointing at an ospi_memories: entry with capacity_mbit
    TBD must block with a clear reason -- AEN301 declares ospi1 with
    capacity_mbit: TBD (ospi0 carries the known 256-Mbit NOR flash on
    CS0), so the storage emitter should not silently allocate against the
    unknown ospi1 capacity."""
    # AEN301 declares on_module.ospi_memories.ospi1 with capacity_mbit: TBD.
    body = """
    name: test-aen-ospi-tbd
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: app_data, size_kib: 64, fs: littlefs, flash_device: ospi1 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    reason = parts[0].reason or ""
    assert "TBD" in reason
    assert "ospi1" in reason


def test_emit_system_manifest_carries_storage(tmp_path: Path) -> None:
    """`storage:` must round-trip through the system manifest so the
    flash-allocation map ships into build/system-manifest.yaml."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    out = emit_system_manifest(project)
    parsed = yaml.safe_load(out)
    assert "storage" in parsed
    names = sorted(p["name"] for p in parsed["storage"])
    assert names == ["app_data", "mcuboot_scratch", "settings"]
    # Each ok entry carries its resolved offset + size.
    for p in parsed["storage"]:
        assert "offset_kib" in p
        assert "size_kib" in p
        assert "dt_label" in p

# ---------------------------------------------------------------------
# security.psa: emit + cross-field validation (v0.6)
# ---------------------------------------------------------------------


_AEN301_SECURITY_HAPPY = """
name: test-aen301-security
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: zephyr
    app: ./m55_he

storage:
  - name: psa_its
    size_kib: 64
    fs: raw
    flash_device: mram_main
  - name: psa_ps
    size_kib: 64
    fs: raw
    flash_device: mram_main

security:
  psa:
    persistent_slots: 32
    its_storage: psa_its
    ps_storage: psa_ps
    tfm: true
    attestation_root: optiga_trust_m
"""


def test_emit_tfm_sysbuild_conf_happy_path(tmp_path: Path) -> None:
    """Happy path: AEN301 + security.psa.tfm: true emits a TF-M
    sysbuild overlay with the expected SB_/CONFIG_ lines."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)

    # Core sysbuild knobs.
    assert "SB_CONFIG_TFM=y" in out
    assert "SB_CONFIG_TFM_BUILD_TYPE=Release" in out

    # PSA slot count + ITS/PS backing stores.
    assert "CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT=32" in out
    assert 'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="psa_its"' in out
    assert 'CONFIG_PSA_CRYPTO_PS_BACKING_STORE="psa_ps"' in out

    # OPTIGA attestation root surfaced.
    assert "CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y" in out
    # And the bridge-driver comment is present.
    assert "optiga_trust_m_bridge" in out


def test_emit_tfm_sysbuild_conf_absent_emits_nothing(tmp_path: Path) -> None:
    """No security.psa: block -> empty string + no file written."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    assert emit_tfm_sysbuild_conf(project) == ""


def test_emit_tfm_sysbuild_conf_tfm_false_emits_nothing(
    tmp_path: Path,
) -> None:
    """`security.psa:` block present but `tfm: false` -> empty.

    PSA Crypto runs non-secure-only (mbedTLS) -- no child image.
    """
    body = """
        name: test-aen301-no-tfm
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            persistent_slots: 8
            tfm: false
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert emit_tfm_sysbuild_conf(project) == ""


def test_security_psa_schema_round_trip(tmp_path: Path) -> None:
    """Schema-level: every documented security.psa.* field survives
    the loader and lands in BoardProject.security."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    psa = project.security["psa"]
    assert psa["persistent_slots"] == 32
    assert psa["its_storage"] == "psa_its"
    assert psa["ps_storage"] == "psa_ps"
    assert psa["tfm"] is True
    assert psa["attestation_root"] == "optiga_trust_m"


def test_security_psa_its_storage_resolves_to_memory_map(
    tmp_path: Path,
) -> None:
    """ITS backing store may reference a SoM memory_map region directly,
    not just a storage[] partition."""
    body = """
        name: test-aen301-its-memmap
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: mram_main
            tfm: true
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)
    assert 'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="mram_main"' in out


def test_security_psa_its_storage_unknown_reference_rejected(
    tmp_path: Path,
) -> None:
    """Unknown ITS backing-store ref -> OrchestratorError pointing at
    the offending YAML path."""
    body = """
        name: test-aen301-its-bad
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: nope_not_a_region
            tfm: true
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "security.psa.its_storage" in msg
    assert "nope_not_a_region" in msg


def test_security_psa_ps_storage_unknown_reference_rejected(
    tmp_path: Path,
) -> None:
    """Same rule for ps_storage when set."""
    body = """
        name: test-aen301-ps-bad
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: mram_main
            ps_storage: definitely_missing
            tfm: true
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "security.psa.ps_storage" in str(excinfo.value)


def test_security_psa_attestation_optiga_rejected_when_som_lacks_it(
    tmp_path: Path,
) -> None:
    """SoM without OPTIGA Trust M -> attestation_root: optiga_trust_m
    must reject with a helpful message.  Every shipping SKU carries
    OPTIGA, so this test stubs a synthetic E1M-AEN301 preset with the
    secure_element field DNI'd to exercise the rejection path."""
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
    real_soc_dir = real_meta / "socs" / "alif" / "ensemble"
    for soc_f in real_soc_dir.iterdir():
        shutil.copy(soc_f, socs_alif / soc_f.name)

    # Synthetic AEN301 variant with OPTIGA explicitly DNI'd.
    preset = yaml.safe_load(
        (real_meta / "e1m_modules" / "E1M-AEN301.yaml").read_text())
    preset["on_module"].pop("secure_element", None)
    preset["capabilities"].pop("optiga_trust_m", None)
    (e1m / "E1M-AEN301.yaml").write_text(yaml.safe_dump(preset),
                                          encoding="utf-8")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        name: test-no-optiga
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            attestation_root: optiga_trust_m
            tfm: true
    """).lstrip("\n"), encoding="utf-8")

    with pytest.raises(OrchestratorError) as excinfo:
        alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    msg = str(excinfo.value)
    assert "attestation_root" in msg
    assert "optiga_trust_m" in msg
    assert "E1M-AEN301" in msg


def test_security_psa_materialise_writes_tfm_conf(tmp_path: Path) -> None:
    """Orchestrator._materialise_shared() writes build/sysbuild/tfm/tfm.conf
    when security.psa.tfm: true, and skips it otherwise."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root)
    orch._materialise_shared()

    tfm_conf = build_root / "sysbuild" / "tfm" / "tfm.conf"
    assert tfm_conf.is_file(), "TF-M overlay not written"
    text = tfm_conf.read_text(encoding="utf-8")
    assert "SB_CONFIG_TFM=y" in text
    assert "CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT=32" in text


def test_security_psa_materialise_skips_when_absent(tmp_path: Path) -> None:
    """No security.psa: -> build/sysbuild/tfm/ is not created."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root)
    orch._materialise_shared()

    assert not (build_root / "sysbuild" / "tfm").exists()


def test_security_psa_build_type_inherits_from_boot(tmp_path: Path) -> None:
    """TF-M build type follows boot.build_type when set."""
    body = """
        name: test-aen301-debug
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        boot:
          method: mcuboot
          build_type: Debug
          signing:
            algorithm: ecdsa_p256
            key_file: keys/dev.pem
        security:
          psa:
            tfm: true
            its_storage: mram_main
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)
    assert "SB_CONFIG_TFM_BUILD_TYPE=Debug" in out

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
    # V2N101 has optiga_trust_m + cau capabilities, but those mbedTLS
    # accelerator entries are `status: planned`; the profile walker
    # must skip them and emit only the sw_fallback line.
    assert "CONFIG_ALP_MBEDTLS_CAU=y" not in conf
    assert "CONFIG_ALP_MBEDTLS_OPTIGA=y" not in conf
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


def test_consistency_mender_on_zephyr_only_ok(tmp_path: Path) -> None:
    """Rule 1 (post-ADR-0009): ota.provider: mender on an all-Zephyr
    project is now valid -- Mender-MCU-client is the Zephyr-side
    dispatch.  Was rejected pre-ADR-0009; the v0.6 provider-driven
    dispatch flips this to ok.  The mender-mcu-client west group is
    not yet active (v0.7 follow-up), so the slice alp.conf emits the
    Kconfig settings as hint comments -- live CONFIG_MENDER_*=y lines
    would resolve to undefined-symbol warnings under Zephyr Kconfig
    today.  The validator still accepts the provider; only the emit
    shape is gated."""
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
  server:
    url:    "https://hosted.mender.io"
    tenant: "${MENDER_TENANT_TOKEN}"
  poll_interval_s: 1800
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    # Zephyr Mender-MCU-client Kconfig must show up on the m55_hp slice
    # as hint comments while the west group is dormant.
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "# CONFIG_MENDER_MCU_CLIENT=y" in conf
    assert '# CONFIG_MENDER_SERVER_URL="https://hosted.mender.io"' in conf
    assert "# CONFIG_MENDER_UPDATE_POLL_INTERVAL=1800" in conf
    # And not as live settings -- the undefined-symbol form aborts the
    # twister build until the mender west group activates.
    for stem in ("CONFIG_MENDER_MCU_CLIENT", "CONFIG_MENDER_SERVER_URL",
                 "CONFIG_MENDER_UPDATE_POLL_INTERVAL"):
        assert f"\n{stem}=" not in conf, (
            f"{stem} must stay commented until mender-mcu-client lands "
            "in west.yml (otherwise Zephyr aborts on undefined symbol)"
        )


def test_consistency_mender_without_any_target_rejected(tmp_path: Path) -> None:
    """Rule 1: ota.provider: mender with NO yocto AND NO zephyr core
    (e.g. a project where every core is `off`) must still fail."""
    body = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: "off"
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
    assert "yocto" in msg.lower() or "zephyr" in msg.lower()


def test_consistency_hawkbit_requires_zephyr(tmp_path: Path) -> None:
    """Rule 1 (new in v0.6 dispatch): ota.provider: hawkbit requires
    at least one Zephyr core."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: "off"

ota:
  provider: hawkbit
  server:
    url: "https://hawkbit.example.com"
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="hawkbit"):
        load_board_yaml(path)


def test_consistency_hawkbit_on_zephyr_ok_with_kconfig(tmp_path: Path) -> None:
    """ota.provider: hawkbit on a Zephyr-targeted board.yaml validates
    and emits the Hawkbit DDI Kconfig on the Zephyr slice."""
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
  provider: hawkbit
  server:
    url: "https://hawkbit.example.com"
  poll_interval_s: 600
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_HAWKBIT=y" in conf
    assert 'CONFIG_HAWKBIT_SERVER="https://hawkbit.example.com"' in conf
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=600" in conf
    # Negative: mender Kconfig must NOT bleed in.
    assert "CONFIG_MENDER_MCU_CLIENT" not in conf


def test_consistency_mcumgr_requires_zephyr(tmp_path: Path) -> None:
    """Rule 1 (new): ota.provider: mcumgr requires at least one Zephyr core."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: "off"

ota:
  provider: mcumgr
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="mcumgr"):
        load_board_yaml(path)


def test_consistency_mcumgr_on_zephyr_emits_smp_kconfig(tmp_path: Path) -> None:
    """ota.provider: mcumgr enables the upstream SMP/MCUmgr Kconfig
    on every Zephyr slice; transport selection stays the app's call."""
    body = """
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

ota:
  provider: mcumgr
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_MCUMGR=y" in conf
    assert "CONFIG_MCUMGR_GRP_IMG=y" in conf
    assert "CONFIG_MCUMGR_GRP_OS=y" in conf


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


# ---------------------------------------------------------------------
# --emit build-plan (the Wave C consumer contract: the `alp` CLI
# consumes this JSON instead of re-implementing the planner).
# Settled 2026-06-04 with the alp-sdk-vscode team:
#   * camelCase keys, schemaVersion'd independently of board.yaml;
#   * GeneratedFile entries MUST carry `contents` (CLI materialise
#     stays pure IO);
#   * no `inputHash` (the CLI recomputes its cache key) and no
#     `sequential` (parallelism policy belongs to the CLI scheduler);
#   * one slice per non-`off` core; command-less slices carry
#     `command: null` + a warning instead of being dropped.
# ---------------------------------------------------------------------


V2N_BOOT_MCUBOOT = """
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


V2N_OFF_AND_COMMANDLESS = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: 'off'
  m33_sm:
    os: zephyr
"""


def test_emit_build_plan_happy(tmp_path: Path) -> None:
    """The plan carries the settled top-level shape, one slice per
    non-off core (sorted), the exact tool command per slice, and
    contents-bearing artefacts."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_build_plan(project, board_yaml=path,
                          build_root=Path("build"))
    plan = _json.loads(out)

    assert plan["schemaVersion"] == 1
    assert plan["sku"] == "E1M-V2N101"
    assert plan["boardYaml"] == path.as_posix()
    assert plan["buildRoot"] == "build"
    assert isinstance(plan["warnings"], list)

    # Settled schema decisions: the CLI owns cache keys + parallelism.
    assert "sequential" not in plan
    for s in plan["slices"]:
        assert "inputHash" not in s

    # Slices sorted by coreId, one per non-off core.
    assert [s["coreId"] for s in plan["slices"]] == \
        ["a55_cluster", "m33_sm"]

    m33 = next(s for s in plan["slices"] if s["coreId"] == "m33_sm")
    assert m33["backend"] == "zephyr"
    assert m33["buildDir"] == "build/m33_sm-zephyr"
    assert m33["command"]["tool"] == "west"
    assert m33["command"]["args"][:2] == ["build", "-b"]
    assert m33["command"]["cwd"] == m33["buildDir"]
    assert m33["env"]["ALP_SDK_ROOT"]
    confs = {a["path"]: a["contents"] for a in m33["configArtefacts"]}
    assert "build/m33_sm-zephyr/alp.conf" in confs
    assert "CONFIG_ALP_SDK=y" in confs["build/m33_sm-zephyr/alp.conf"]

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["backend"] == "yocto"
    assert a55["command"]["tool"] == "bitbake"
    assert a55["command"]["args"] == ["alp-image-edge"]
    confs = {a["path"]: a["contents"] for a in a55["configArtefacts"]}
    assert "build/a55_cluster-yocto/local.conf" in confs
    assert confs["build/a55_cluster-yocto/local.conf"].strip()

    # Shared artefacts carry contents (the CLI byte-writes them).
    shared = {a["path"]: a["contents"] for a in plan["sharedArtefacts"]}
    assert "build/generated/alp/system_ipc.h" in shared
    assert "build/generated/dts-reservations.dtsi" in shared
    assert "build/generated/dts-partitions.dtsi" in shared
    for contents in shared.values():
        assert contents.strip()


def test_emit_build_plan_stock_shim_resolves_to_sdk_app(tmp_path: Path) -> None:
    """A core left on the stock M-core shim (app: alp-stock-shim) gets a
    normal west command pointed at the SDK-owned shim app."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    board = """
name: stock-shim-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  m33_sm:
    os: zephyr
    app: alp-stock-shim
"""
    path = _write_board(tmp_path, board)
    project = load_board_yaml(path)
    out = emit_build_plan(project, board_yaml=path, build_root=Path("build"))
    plan = _json.loads(out)

    m33 = next(s for s in plan["slices"] if s["coreId"] == "m33_sm")
    assert m33["command"]["tool"] == "west"
    assert m33["command"]["args"][:3] == [
        "build",
        "-b",
        "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33",
    ]
    assert m33["command"]["args"][3] == str(
        REPO / "firmware" / "alp-stock-shim")
    assert m33["command"]["cwd"] == "build/m33_sm-zephyr"

    stock_warns = [w for w in plan["warnings"]
                   if w["code"] == "stock-shim-unimplemented"]
    assert stock_warns == []

    # Carried, not dropped: the slice still ships its alp.conf artefact.
    assert any(a["path"].endswith("alp.conf")
               for a in m33["configArtefacts"])


def test_emit_build_plan_deterministic(tmp_path: Path) -> None:
    """Spec parity with the other emits: byte-identical re-runs."""
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    out_a = emit_build_plan(load_board_yaml(path), board_yaml=path,
                            build_root=Path("build"))
    out_b = emit_build_plan(load_board_yaml(path), board_yaml=path,
                            build_root=Path("build"))
    assert out_a == out_b


def test_emit_build_plan_writes_nothing(
    tmp_path: Path, monkeypatch
) -> None:
    """The emit is pure -- no build dirs, no config files, nothing."""
    from alp_orchestrate import emit_build_plan

    monkeypatch.chdir(tmp_path)
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    emit_build_plan(project, board_yaml=path, build_root=Path("build"))
    assert [p.name for p in tmp_path.iterdir()] == ["board.yaml"]


def test_emit_build_plan_matches_materialiser(
    tmp_path: Path, monkeypatch
) -> None:
    """By-construction parity: every artefact the plan carries is
    byte-identical to what the Orchestrator's materialise step writes
    to disk (the contract promised to the CLI side)."""
    import json as _json
    from alp_orchestrate import emit_build_plan
    import alp_orchestrate

    path = _write_board(tmp_path, V2N_HAPPY)
    build_root = tmp_path / "build"

    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=build_root))

    # Materialise via the real fan_out (dispatch skipped: no tools).
    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    orch = Orchestrator(load_board_yaml(path), build_root)
    orch.fan_out(parallel=False)

    artefacts = list(plan["sharedArtefacts"])
    for s in plan["slices"]:
        artefacts.extend(s["configArtefacts"])
    assert artefacts
    for entry in artefacts:
        on_disk = Path(entry["path"]).read_text(encoding="utf-8")
        assert on_disk == entry["contents"], \
            f"{entry['path']} diverges from the materialiser"


def test_emit_build_plan_off_core_excluded_commandless_warns(
    tmp_path: Path,
) -> None:
    """`off` cores never enter the plan; a slice the script cannot
    yet build is carried with `command: null` plus a machine-readable
    warning, so the CLI can still report the core."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_OFF_AND_COMMANDLESS)
    project = load_board_yaml(path)
    # Simulate a SoM whose topology hasn't pinned this core's Zephyr
    # board target yet (the pending-HW-config TBD case): the loader
    # permits `board: None` for zephyr -- only `app:` is enforced --
    # and _slice_command then has nothing to hand `west build -b`.
    project.cores["m33_sm"].board = None
    # Use a real app so this isolates the board-missing -> no-command path;
    # the SoM preset would otherwise default this M-core to the stock shim,
    # which has its own warning code (see test_emit_build_plan_stock_shim_skipped).
    project.cores["m33_sm"].app = "./m33"
    plan = _json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    assert [s["coreId"] for s in plan["slices"]] == ["m33_sm"]
    m33 = plan["slices"][0]
    assert m33["command"] is None
    codes = [(w["code"], w.get("coreId")) for w in plan["warnings"]]
    assert ("no-command", "m33_sm") in codes


def test_emit_build_plan_carries_boot_sysbuild_conf(
    tmp_path: Path, monkeypatch
) -> None:
    """A `boot:` block surfaces as the build/alp_sysbuild.conf shared
    artefact -- and the materialiser writes the same file (this also
    pins the fix for emit_sysbuild_conf never being wired into
    _materialise_shared)."""
    import json as _json
    from alp_orchestrate import emit_build_plan
    import alp_orchestrate

    path = _write_board(tmp_path, V2N_BOOT_MCUBOOT)
    build_root = tmp_path / "build"
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=build_root))

    shared = {a["path"]: a["contents"] for a in plan["sharedArtefacts"]}
    sysbuild_path = (build_root / "alp_sysbuild.conf").as_posix()
    assert sysbuild_path in shared
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=y" in shared[sysbuild_path]

    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    orch = Orchestrator(load_board_yaml(path), build_root)
    orch.fan_out(parallel=False)
    assert (build_root / "alp_sysbuild.conf").read_text(
        encoding="utf-8") == shared[sysbuild_path]


def test_zephyr_slice_command_wires_sysbuild_overlay(tmp_path: Path) -> None:
    """ADR 0014 Phase-3: a `boot:` block (-> build/alp_sysbuild.conf) makes
    the Zephyr slice command pass `--sysbuild --sysbuild-config
    ../alp_sysbuild.conf` (the overlay sits one dir up from the slice's
    cwd=build/<core>-<os>); a project without one adds no flag and keeps
    the bare `west build -b <board> <app>` shape."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    # With boot: -> sysbuild overlay emitted -> command carries the flags.
    path = _write_board(tmp_path, V2N_BOOT_MCUBOOT)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))
    z = next(s for s in plan["slices"]
             if s["backend"] == "zephyr" and s["command"])
    args = z["command"]["args"]
    assert args[:2] == ["build", "-b"]
    assert "--sysbuild" in args
    assert args[args.index("--sysbuild-config") + 1] == "../alp_sysbuild.conf"

    # Without boot: -> no sysbuild overlay -> no flag, bare command.
    path2 = _write_board(tmp_path, V2N_HAPPY, name="board-noboot.yaml")
    plan2 = _json.loads(emit_build_plan(
        load_board_yaml(path2), board_yaml=path2, build_root=Path("build")))
    z2 = next(s for s in plan2["slices"]
              if s["backend"] == "zephyr" and s["command"])
    assert "--sysbuild" not in z2["command"]["args"]
    assert "--sysbuild-config" not in z2["command"]["args"]


def test_cli_emit_build_plan(tmp_path: Path, capsys, monkeypatch) -> None:
    """`--emit build-plan` prints the JSON to stdout, rc 0, writes
    nothing to disk."""
    import json as _json
    from alp_orchestrate import main as orchestrate_main

    monkeypatch.chdir(tmp_path)
    path = _write_board(tmp_path, V2N_HAPPY)
    rc = orchestrate_main(["--input", str(path), "--emit", "build-plan"])
    out = capsys.readouterr().out
    assert rc == 0
    plan = _json.loads(out)
    assert plan["schemaVersion"] == 1
    assert [p.name for p in tmp_path.iterdir()] == ["board.yaml"]


# ---------------------------------------------------------------------
# Issue #596 -- app paths anchor on board.yaml, never the process CWD
# ---------------------------------------------------------------------


def test_emit_build_plan_app_paths_independent_of_cwd(
    tmp_path: Path, monkeypatch
) -> None:
    """The plan's slice `command` + `appDir` resolve relative to
    board.yaml's own directory, byte-identical no matter which
    directory the emitting process happens to be running from --
    the #596 repro (`west build`'s target used to fall back to the
    repo root because the CWD-anchored resolve missed the app dir and
    the parent CMakeLists.txt fallback silently matched the root)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    project_dir = tmp_path / "project"
    project_dir.mkdir()
    (project_dir / "m33").mkdir()
    (project_dir / "m33" / "CMakeLists.txt").write_text("", encoding="utf-8")
    path = _write_board(project_dir, V2N_HAPPY)

    elsewhere = tmp_path / "somewhere-else-entirely"
    elsewhere.mkdir()

    # Same board.yaml, same build_root; only the CWD differs.
    monkeypatch.chdir(project_dir)
    plan_same_dir = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    monkeypatch.chdir(elsewhere)
    plan_other_dir = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    assert plan_same_dir == plan_other_dir

    m33 = next(s for s in plan_other_dir["slices"] if s["coreId"] == "m33_sm")
    # Correctly anchored on the project dir -- NOT the unrelated CWD, and
    # NOT the repo root (the historical parent-CMakeLists.txt fallback trap).
    assert m33["command"]["args"][-1] == str(project_dir / "m33")
    assert m33["appDir"] == (project_dir / "m33").as_posix()


def test_slice_command_helpers_take_explicit_base_dir(tmp_path: Path) -> None:
    """`_resolve_app_path` / `_zephyr_app_dir` / `_slice_command` require
    an explicit `base_dir` and never fall back to `Path.cwd()` -- the
    exact seam issue #596 flagged (`orchestrator.py` used to call
    `Path.cwd()` directly)."""
    from alp_orchestrate.orchestrator import _resolve_app_path

    project_dir = tmp_path / "myproj"
    (project_dir / "src").mkdir(parents=True)

    resolved = _resolve_app_path("./src", project_dir)
    assert resolved == (project_dir / "src").resolve()

    # Absolute paths pass through untouched regardless of base_dir.
    abs_dir = tmp_path / "abs-app"
    abs_dir.mkdir()
    assert _resolve_app_path(str(abs_dir), project_dir) == abs_dir


def test_orchestrator_dispatch_anchors_on_board_yaml_not_cwd(
    tmp_path: Path, monkeypatch
) -> None:
    """The real (non-emit) dispatch path -- `Orchestrator(..., board_yaml=)`
    -- resolves the same way `emit_build_plan` does, so `west
    alp-build`/`west alp-build --emit build-plan` never disagree on the
    app path depending on which directory the shell happens to be in."""
    import alp_orchestrate

    project_dir = tmp_path / "project"
    project_dir.mkdir()
    (project_dir / "m33").mkdir()
    path = _write_board(project_dir, V2N_HAPPY)

    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)

    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    assert orch.base_dir == project_dir.resolve()


# ---------------------------------------------------------------------
# Issue #597 -- Yocto app-only slices need a valid bitbake target
# ---------------------------------------------------------------------


YOCTO_APP_ONLY_NO_RECIPE = """
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    app: ./src
  m33_sm:
    os: "off"
"""


YOCTO_APP_ONLY_WITH_RECIPE = """
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    app: ./src
    recipe: alp-my-app
  m33_sm:
    os: "off"
"""


def test_yocto_app_only_without_recipe_blocks_not_bitbake_path(
    tmp_path: Path,
) -> None:
    """An app-only Yocto slice (`app:` set, no `image:`, no `recipe:`)
    must never emit `bitbake <path>` -- `app:` is a source directory, not
    a recipe name.  The plan blocks it explicitly instead (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, YOCTO_APP_ONLY_NO_RECIPE)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] is None
    assert a55["appDir"] == str((tmp_path / "src").resolve()).replace(
        "\\", "/")
    codes = [(w["code"], w.get("coreId")) for w in plan["warnings"]]
    assert ("yocto-recipe-missing", "a55_cluster") in codes
    # Never the historical bug shape.
    for s in plan["slices"]:
        if s["command"]:
            assert s["command"]["args"] != ["./src"]


def test_yocto_app_only_with_recipe_emits_valid_bitbake_target(
    tmp_path: Path,
) -> None:
    """An app-only Yocto slice that names its `recipe:` gets a real
    bitbake target; the app source dir is retained separately as
    `appDir` for tooling that wants it (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, YOCTO_APP_ONLY_WITH_RECIPE)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] == {
        "tool": "bitbake",
        "args": ["alp-my-app"],
        "cwd":  "build/a55_cluster-yocto",
    }
    assert a55["appDir"] == (tmp_path / "src").resolve().as_posix()
    assert not [w for w in plan["warnings"]
                if w["coreId"] == "a55_cluster"]


def test_yocto_image_and_app_both_set_image_wins_app_dir_retained(
    tmp_path: Path,
) -> None:
    """When both `image:` and `app:` are set, the recipe-valued `image:`
    is the actual bitbake target (never the app path); the app source
    dir is still carried as `appDir` for tooling, matching the
    acceptance criterion that image+app slices stay covered (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"]["args"] == ["alp-image-edge"]
    assert a55["appDir"] == (tmp_path / "linux").resolve().as_posix()


def test_yocto_stock_image_app_token_still_resolves_without_recipe(
    tmp_path: Path,
) -> None:
    """The SoM preset's stock A-core default (`app: alp-image-edge`,
    no `image:`) is already a real bitbake recipe name, not a project
    source path -- it must keep building without requiring a `recipe:`
    override (regression guard: the #597 fix must not block the
    default/uncustomised topology every V2N/AEN A-core inherits)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    board = """
    som:
      sku: E1M-V2N101
      hw_rev: r1

    cores:
      m33_sm:
        os: "off"
    """
    path = _write_board(tmp_path, board)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] == {
        "tool": "bitbake",
        "args": ["alp-image-edge"],
        "cwd":  "build/a55_cluster-yocto",
    }
    # Stock image token isn't a source path -- nothing to report.
    assert a55["appDir"] is None
    assert not [w for w in plan["warnings"] if w["coreId"] == "a55_cluster"]

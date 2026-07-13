# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- IPC carve-out (arena) memory
resolution (resolve_carve_outs()).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_memory.py -v
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    emit_dts_reservations,
    load_board_yaml,
    resolve_carve_outs,
)


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


# AEN801's mailbox controller (alif_mhuv2) is resolved -- and, unlike
# AEN701, its E8 SoC JSON now carries a grounded per-region `base` for
# sram0 (0x02000000, from the Alif kernel fork DTS ensemble-ex.dtsi
# memory1@2000000, checked 2026-06-26).  So the M55-HP<->M55-HE rpmsg
# channel RESOLVES rather than blocking (see
# test_resolve_carve_outs_aen801_m55_ipc_resolves below).
AEN801_M55_IPC = """
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


def test_resolve_carve_outs_blocks_on_unmapped_base(tmp_path: Path) -> None:
    """AEN701 has a RESOLVED mailbox controller (alif_mhuv2), so it
    proceeds past the controller-TBD guard into the region allocator.
    Its E7 SoC JSON carries no `memory_regions` (unlike AEN801's
    grounded sram0 base), so resolve_carve_outs MUST emit a blocked
    carve-out rather than crash with `KeyError: 'base'`."""
    path = _write_board(tmp_path, AEN701_UNMAPPED)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)        # must not raise
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "E1M-AEN701" in entry.reason
    assert "HW-mapped" in entry.reason


def test_resolve_carve_outs_aen801_m55_ipc_resolves(tmp_path: Path) -> None:
    """AEN801's E8 SoC JSON now carries a grounded per-region `base`
    for sram0 (0x02000000, from the Alif kernel fork DTS).  The
    M55-HP<->M55-HE rpmsg channel must RESOLVE (status='ok', base
    allocated from sram0) rather than crash or block.

    Renamed/split out of test_resolve_carve_outs_blocks_on_unmapped_base
    when the E8 memory map was grounded; the no-crash guarantee is still
    provided by the `resolved = resolve_carve_outs(project)` call."""
    path = _write_board(tmp_path, AEN801_M55_IPC)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)        # must not raise
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "ok", (
        f"AEN801 M55-HP<->M55-HE carveout should resolve after grounding "
        f"the E8 memory bases; got status={entry.status!r}, "
        f"reason={entry.reason!r}"
    )
    assert entry.base is not None
    # Exact base: sram0 top (0x02400000) minus 64 KiB (0x10000),
    # verified against resolve_carve_outs output for this fixture.
    assert entry.base == 0x023F0000, (
        f"expected sram0 top-down base 0x023F0000 (37683200); got {entry.base:#010x}"
    )
    assert entry.size == 64 * 1024


def test_emit_dts_reservations_aarch32_cells(tmp_path: Path) -> None:
    """E1M-AEN801 is AArch32 (linux_phys_addr_bits=32) so the
    reserved-memory node must use single-cell addressing (a two-cell base+size
    reg), matching the base ensemble-ex.dtsi #address-cells = <1>."""
    path = _write_board(tmp_path, AEN801_M55_IPC)
    out = emit_dts_reservations(load_board_yaml(path))
    assert "#address-cells = <1>;" in out
    assert "#size-cells = <1>;" in out
    # 1-cell reg: base + size as two 32-bit words (base 0x023f0000, 64 KiB).
    assert "reg = <0x023f0000 0x00010000>;" in out
    # The 2-cell 4-word form must NOT appear for this AArch32 target.
    assert "#address-cells = <2>;" not in out


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
# Phase 3 follow-up: resolve_carve_outs() strict mailbox-reservation
# guard (spec §6.4).
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


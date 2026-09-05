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

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import (              # noqa: E402
    REPO,
    V2N_HAPPY,
    _synthetic_nx9101_root,
    _write_board,
)

from alp_orchestrate import (                       # noqa: E402
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


# AEN701 resolves its mailbox controller (alif_mhuv2), so it sails past
# the controller-TBD guard -- but its own `memory_map:` (metadata/
# e1m_modules/E1M-AEN701.yaml) still keeps `mram_main`'s `base: "TBD"`.
# Before the region.get("base") fix this crashed resolve_carve_outs with
# `KeyError: 'base'`; it MUST instead land a clean blocked carve-out.
# Regression guard for that crash.  (E1M-AEN801 used to share this fixture
# -- #1069 gave it a real `memory_map:`, so it now RESOLVES instead; see
# test_resolve_carve_outs_aen801_resolves_after_1069_memory_map below.)
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


AEN801_MAPPED = """
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
    `#error` directive.

    This is the mailbox-controller-TBD placeholder (a genuinely
    different `TBD` than #1025's hw_rev `status:` gate -- E1M-NX9101's
    real `metadata/e1m_modules/E1M-NX9101.yaml` still carries both).
    #1025 refuses the real E1M-NX9101 preset outright before
    `resolve_carve_outs` is ever reached (its only hw_rev, imx93 r1,
    is `status: tbd`), and there is no second hw_rev to pick and no
    other SoM with this same mailbox-TBD placeholder to swap to -- so
    this runs against `_synthetic_nx9101_root`'s scratch metadata root
    (see its docstring for why the #1025 gate is a no-op there without
    this being a claim about the real E1M-NX9101's buildability)."""
    import alp_orchestrate

    meta = _synthetic_nx9101_root(tmp_path)
    path = _write_board(tmp_path, NX_TBD)
    project = alp_orchestrate.load_board_yaml(path, metadata_root=meta)
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
    ],
)
def test_resolve_carve_outs_blocks_on_unmapped_base(
    tmp_path: Path, body: str, sku: str
) -> None:
    """AEN presets have a RESOLVED mailbox controller (alif_mhuv2), so
    they proceed past the controller-TBD guard into the region allocator.
    E1M-AEN701's `memory_map:` authors six flash-class regions plus
    `mram_main` (still `base: "TBD"`), so resolve_carve_outs MUST emit a
    blocked carve-out rather than crash with `KeyError: 'base'`.

    #1365 split B changed the reason text this asserts. Pre-split-B,
    `mram_main` was the sole candidate (no `carveout:` key excluded it), so
    the block came from the base-unmapped check ("... hasn't been HW-mapped
    yet") in `_region_top_init()`. Split B derives eligibility against the
    SoC's declared MRAM aperture first: the six fine regions are excluded
    as flash-class (contained in the aperture), and `mram_main` is ALSO
    excluded on its own terms -- its base is still unresolved, and its
    authored `write_authority: composite` (not `customer_runtime`)
    disqualifies it -- so every candidate is excluded before the
    base-unmapped check ever runs, and the reason names that instead."""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)        # must not raise
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert sku in entry.reason
    assert "ineligible for an IPC carve-out" in entry.reason
    assert "mram_main" in entry.reason
    assert "write_authority is 'composite'" in entry.reason


def test_resolve_carve_outs_aen801_stays_blocked_after_1069_memory_map(
    tmp_path: Path,
) -> None:
    """#1069 gave E1M-AEN801 a real `memory_map:` (mcuboot/he_slot0/
    hp_slot0/reserved/storage/mram_main), but an m55_hp+m55_he rpmsg
    carve-out MUST still land `status: blocked`: the five fine-grained
    regions are MRAM (flash-class), not RAM, and are marked
    `carveout: false` so resolve_carve_outs() can't silently place a
    shared-memory ring inside non-volatile flash just because the region
    has a real `base` (needed only for the board generator's DTS
    partition table, see metadata/e1m_modules/E1M-AEN801.yaml). The only
    other both-core-accessible region, `mram_main`, deliberately keeps
    `base: TBD`, so the entry blocks there instead."""
    path = _write_board(tmp_path, AEN801_MAPPED)
    project = load_board_yaml(path)
    resolved = resolve_carve_outs(project)
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "mram_main" in entry.reason


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
    tmp_path: Path,
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

    # `load_board_yaml(..., metadata_root=meta)` below is the only knob that
    # matters -- every alp_orchestrate resolver reads
    # `BoardProject.effective_metadata_root()`, not a module-level
    # `METADATA_ROOT` global, so monkeypatching `alp_orchestrate.METADATA_ROOT`
    # is inert (#1485).

    path = _write_board(tmp_path, V2N_HAPPY)
    project = alp_orchestrate.load_board_yaml(path, metadata_root=meta)
    resolved = resolve_carve_outs(project)
    assert len(resolved) == 1
    entry = resolved[0]
    assert entry.status == "blocked"
    assert entry.reason is not None
    assert "alp_default_rpmsg" in entry.reason
    assert "E1M-V2N101" in entry.reason


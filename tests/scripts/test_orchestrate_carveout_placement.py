# SPDX-License-Identifier: Apache-2.0
"""
IPC carve-out PLACEMENT must be collision-free, in-bounds and reachable --
alplabai/tan-cli#552 + alplabai/tan-cli#553.

Two carve-outs could resolve onto ONE physical address and both report
`status: ok` (#552), an `address:` outside every declared region was accepted
and still labelled with a real `region:` (#552), and the top-down allocator
placed a DDR carve-out at the top of `ddr_main` -- a 33-bit address the
Cortex-M33 endpoint cannot form (#553).

Every test here was run against the PRE-fix resolver first and failed there in
the wrong direction (the measured pre-fix output is quoted per test), so this
is regression coverage rather than tests that merely agree with the new
implementation.

E1M-V2N101 is used throughout: its SoC JSON (metadata/socs/renesas/rzv2n/
n44.json) is the one that declares both a small `ocram_low` (base 0x00010000,
512 KiB) and a 4 GiB `ddr_main` (base 0x48000000) reachable from both
`a55_cluster` and `m33_sm`, which is exactly the shape all three defects need.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_carveout_placement.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import (  # noqa: E402
    load_board_yaml,
    resolve_carve_outs,
)


def _v2n101(ipc_body: str) -> str:
    return f"""
    name: test-v2n101-carveout-placement
    som:
      sku: E1M-V2N101
      hw_rev: r1

    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: zephyr
        app: ./m33

    ipc:
    {ipc_body}
    """


def _resolve(tmp_path, ipc_body):
    path = _write_board(tmp_path, _v2n101(ipc_body))
    return resolve_carve_outs(load_board_yaml(path))


def _by_name(carve_outs):
    return {c.name: c for c in carve_outs}


class TestExplicitAddressOverlap:
    """tan-cli#552 -- an explicit `address:` was honoured after a 4 KiB
    alignment check and nothing else."""

    def test_two_carve_outs_never_share_one_base(self, tmp_path):
        """Measured pre-fix on this exact board.yaml:

            a_chan    status= ok region= ocram_low base= 0x80000
            b_chan    status= ok region= ocram_low base= 0x80000

        `a_chan` bump-allocated top-down to 0x80000 and `b_chan` took its
        pinned 0x80000; both reported ok. Two cores handed the same 64 KiB
        shared-memory window overwrite each other's rings on silicon.
        """
        parts = _by_name(_resolve(tmp_path, """
      - {name: a_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64}
      - {name: b_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x80000}
    """))
        a, b = parts["a_chan"], parts["b_chan"]
        assert a.status == "ok", a.reason
        assert b.status == "ok", b.reason
        # The pin is honoured verbatim; the allocator steps around it.
        assert b.base == 0x80000
        assert a.base != b.base
        assert a.base + a.size <= b.base or b.base + b.size <= a.base

    def test_pin_colliding_with_a_pin_is_refused(self, tmp_path):
        """Two pins on one address cannot both be satisfied -- the second
        must block, not silently share the window."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: a_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x80000}
      - {name: b_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x80000}
    """))
        assert parts["a_chan"].status == "ok", parts["a_chan"].reason
        assert parts["b_chan"].status == "blocked"
        assert "overlaps carve-out 'a_chan'" in (parts["b_chan"].reason or "")
        assert "0x80000" in (parts["b_chan"].reason or "")


class TestExplicitAddressBounds:
    """tan-cli#552 -- an address in NO declared region was accepted and
    reported under a region it is nowhere near."""

    def test_out_of_region_address_is_refused(self, tmp_path):
        """Measured pre-fix:

            wild_chan status= ok region= ocram_low base= 0xdeadb000

        0xdeadb000 is outside `ocram_low` (0x10000..0x90000) AND outside
        `ddr_main` (0x48000000..0x148000000); it was tagged `ocram_low`
        purely because that region ranked first.
        """
        parts = _by_name(_resolve(tmp_path, """
      - {name: wild_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0xDEADB000}
    """))
        wild = parts["wild_chan"]
        assert wild.status == "blocked"
        assert wild.region == ""
        reason = wild.reason or ""
        assert "0xdeadb000" in reason
        assert "lies outside every memory_map region" in reason
        # The refusal must SAY where the entry could legally go.
        assert "ocram_low 0x10000..0x90000" in reason

    def test_pin_is_labelled_with_the_region_that_contains_it(self, tmp_path):
        """A pin inside `ddr_main` must come back tagged `ddr_main`, even
        though `ocram_low` ranks first for a non-cacheable entry."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: ddr_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x48100000}
    """))
        ddr = parts["ddr_chan"]
        assert ddr.status == "ok", ddr.reason
        assert ddr.region == "ddr_main"
        assert ddr.base == 0x48100000


class TestEndpointReachability:
    """tan-cli#553 -- the allocator only ever bounds-checked DOWNWARD, so a
    DDR carve-out landed at the top of a 4 GiB region."""

    def test_ddr_carve_out_stays_inside_the_cm33_256_mib_window(self, tmp_path):
        """Measured pre-fix on this exact board.yaml:

            alp_default_rpmsg status= ok region= ddr_main base= 0x147f80000

        0x147f80000 is a 33-bit address; cast to a pointer on the M33 it
        truncates to 0x47f80000, below the DDR base. Per Renesas FSP
        bsp_slave_address.h the CM33's DDR window is 256 MiB -- CM33-secure
        0x80000000 / CM33-non-secure 0x90000000 -> A55 0x40000000 -- so the
        reachable slice of `ddr_main` (base 0x48000000) ends at 0x50000000.
        """
        parts = _by_name(_resolve(tmp_path, """
      - {name: alp_default_rpmsg, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 512, cacheable: true}
    """))
        rpmsg = parts["alp_default_rpmsg"]
        assert rpmsg.status == "ok", rpmsg.reason
        assert rpmsg.region == "ddr_main"
        assert rpmsg.base >= 0x48000000
        assert rpmsg.base + rpmsg.size <= 0x50000000
        # It must also still be a 32-bit address the M33 can hold at all.
        assert rpmsg.base < 1 << 32

    def test_pin_outside_the_cm33_window_is_refused(self, tmp_path):
        """0x100000000 is inside `ddr_main` (which runs to 0x148000000) and
        `accessible_from:` lists m33_sm across the whole region -- but the
        CM33 cannot form the address, so the pin must block."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: high_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x100000000}
    """))
        high = parts["high_chan"]
        assert high.status == "blocked"
        assert "ddr_main 0x48000000..0x50000000" in (high.reason or "")

    def test_a_region_with_no_window_is_still_used_to_its_top(self, tmp_path):
        """`access_windows:` is declared on `ddr_main` only.  `ocram_low`
        declares none, so a small entry must still land at its very top
        (0x90000 - 0x10000 = 0x80000) -- the fix must not shrink regions
        that carry no window."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: small_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64}
    """))
        small = parts["small_chan"]
        assert small.status == "ok", small.reason
        assert small.region == "ocram_low"
        assert small.base == 0x80000
        assert small.base + small.size == 0x90000


class TestUnchangedBehaviour:
    """The no-`address:` path every shipped board.yaml uses must not move."""

    def test_plain_non_cacheable_entry_still_lands_at_ocram_low_top(self, tmp_path):
        """examples/multicore/rpmsg-v2n's shape: a 512 KiB non-cacheable
        rpmsg carve-out fills `ocram_low` exactly, base 0x10000."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: alp_default_rpmsg, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 512}
    """))
        rpmsg = parts["alp_default_rpmsg"]
        assert rpmsg.status == "ok", rpmsg.reason
        assert rpmsg.region == "ocram_low"
        assert rpmsg.base == 0x10000
        assert rpmsg.size == 512 * 1024

    def test_misaligned_pin_still_names_page_alignment(self, tmp_path):
        parts = _by_name(_resolve(tmp_path, """
      - {name: odd_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0x80001}
    """))
        odd = parts["odd_chan"]
        assert odd.status == "blocked"
        assert "not page-aligned (4 KiB)" in (odd.reason or "")

    def test_region_exhaustion_still_blocks(self, tmp_path):
        """`ocram_low` is 512 KiB; the second 512 KiB entry has nowhere to
        go and must block rather than wrap into `ddr_main`."""
        parts = _by_name(_resolve(tmp_path, """
      - {name: chan_a, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 512}
      - {name: chan_b, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 512}
    """))
        assert parts["chan_a"].status == "ok", parts["chan_a"].reason
        assert parts["chan_b"].status == "blocked"
        assert "doesn't fit in region 'ocram_low'" in (parts["chan_b"].reason or "")

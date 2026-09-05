# SPDX-License-Identifier: Apache-2.0
"""
The ordering guard for #1365 split B -- P1's flash-class exclusion must hold
EVEN IF `mram_main`'s `base: "TBD"` (metadata/e1m_modules/E1M-AEN801.yaml)
were filled in tomorrow.  Split B deliberately does NOT fill that field (a
separate, later step); this test proves the field staying `"TBD"` is not
secretly load-bearing for safety.

The hazard this closes (metadata/e1m_modules/E1M-AEN801.yaml, verbatim):

    #   5552 B   bench-observed 2026-08-08 -- ATOC magic `ckBS` (0x53426B63)
    #            read at 0x8057EA50, intact, while an app erased 0x80560000
    #            inside what was then the SAME `storage` partition

`mram_main` is the only region on E1M-AEN801 (and its AEN siblings) that
lists an `a32_cluster`/`m55_*` endpoint AND carries no `carveout` key at
all -- `scripts/alp_orchestrate/carveout.py` pre-split-B read
`if region.get("carveout") is False`, so an absent key meant ELIGIBLE.  The
allocator is top-down and seeds `region_top` from `base + size` alone with
no knowledge that mcuboot/he_slot0/hp_slot0/reserved/storage/atoc tile the
same window.  Today the ONLY thing keeping an `a32_cluster` IPC entry out of
the live `atoc` band (0x80578000..0x80580000) is `mram_main`'s unresolved
`base: "TBD"`.

This test synthetically resolves that `base` (0x80000000, matching the
declared aperture floor -- `metadata/socs/alif/ensemble/e8.json`'s
`soc_flash_base`) in an in-memory copy of the loaded project, WITHOUT
touching the tracked YAML, and asserts the `ipc:` entry still blocks --
naming the DERIVED flash class, not an address inside `atoc`.  A green run
here that predates #1365 split B's `_region_ipc_eligibility()` would go RED
(this is exactly what `resolve_carve_outs()` used to do: place the entry at
the top of `mram_main`, i.e. inside `atoc`, and report `status: ok`).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_carveout_aperture_ordering.py -v
"""

from __future__ import annotations

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO  # noqa: E402

from alp_orchestrate import load_board_yaml, resolve_carve_outs  # noqa: E402

RPMSG_AEN_BOARD = REPO / "examples" / "multicore" / "rpmsg-aen" / "board.yaml"
MPROC_MAILBOX_BOARD = REPO / "examples" / "multicore" / "mproc-mailbox" / "board.yaml"

# metadata/socs/alif/ensemble/e8.json's `soc_flash_base` (#1365 split A).
_E8_APERTURE_BASE = 0x80000000


def _with_mram_main_resolved(project):
    """Return `project` with an in-memory-only `mram_main.base` fill-in.

    Deep-copies `som_preset` first so this never mutates the tracked
    `metadata/e1m_modules/E1M-AEN801.yaml` -- split B leaves that file's
    `base: "TBD"` untouched; this is purely a synthetic what-if.
    """
    project.som_preset = copy.deepcopy(project.som_preset)
    found = False
    for region in project.som_preset["memory_map"]:
        if region.get("name") == "mram_main":
            region["base"] = _E8_APERTURE_BASE
            found = True
    assert found, "fixture drift: E1M-AEN801.yaml no longer declares mram_main"
    return project


def _by_name(carve_outs):
    return {c.name: c for c in carve_outs}


class TestMramMainOrderingGuard:
    """#1365 split B: resolving `mram_main`'s base must NOT resurrect the
    ATOC-overwrite hazard -- the flash-class exclusion has to hold on its
    own, independent of the TBD placeholder."""

    def test_a32_cluster_rpmsg_entry_stays_blocked_once_mram_main_resolves(self):
        project = _with_mram_main_resolved(load_board_yaml(RPMSG_AEN_BOARD))
        resolved = _by_name(resolve_carve_outs(project))
        entry = resolved["alp_default_rpmsg"]

        assert entry.status == "blocked", (
            f"a32_cluster ipc entry resolved {entry.status!r} once "
            f"mram_main's base was filled in -- the flash-class exclusion "
            f"did not hold on its own; base={entry.base:#x}")
        # The whole point: it must not merely happen to land somewhere
        # harmless -- it must be refused with a reason naming the DERIVED
        # flash class, not silently re-blocked for an unrelated cause
        # (e.g. a stale mailbox-metadata check tripping first).
        assert "flash-class" in entry.reason, (
            f"blocked for the wrong reason: {entry.reason!r}")
        assert "mram_main" in entry.reason

    def test_never_allocates_inside_the_atoc_band(self):
        """Even if some future change loosened the exclusion, the base
        must never land in [0x8057_8000, 0x8058_0000) -- the live ATOC
        band -- while still reporting `status: ok`."""
        project = _with_mram_main_resolved(load_board_yaml(RPMSG_AEN_BOARD))
        entry = _by_name(resolve_carve_outs(project))["alp_default_rpmsg"]
        if entry.status == "ok":
            assert not (0x80578000 <= entry.base < 0x80580000), (
                f"carve-out placed at 0x{entry.base:x}, inside the live "
                f"ATOC band -- exactly the #1365 hazard")

    def test_raw_shmem_entry_on_aen801_also_stays_blocked(self):
        """`mproc-mailbox`'s raw_shmem entry (m55_hp/m55_he, not
        a32_cluster) exercises the same SoM/aperture with a different
        `ipc.kind` -- the exclusion must not be accidentally scoped to
        `rpmsg` alone."""
        project = _with_mram_main_resolved(load_board_yaml(MPROC_MAILBOX_BOARD))
        entry = _by_name(resolve_carve_outs(project))["alp_shmem0"]
        assert entry.status == "blocked"
        assert "flash-class" in entry.reason

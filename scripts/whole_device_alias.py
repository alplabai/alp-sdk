#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared "extent == aperture exactly" whole-device-alias predicate (#2073).

A `memory_map:` region whose resolved `[base, base + size)` extent equals
a SoC's declared on-die MRAM aperture exactly (e.g. `mram_main`, once its
`base` stops being the `"TBD"` sentinel) is the device itself, not a
partition inside it -- `atoc`/`mcuboot`/`he_slot0`/`hp_slot0`/`reserved`/
`storage` all subdivide the SAME window `mram_main` aliases.

Two independent callers need this exact predicate and must never drift
on it: `alp_orchestrate.aperture.classify_region()` /
`is_partition_inside_aperture()` (the aperture-tiling / flash-class
checks, #1365 split B), and `scripts/gen_zephyr_board.py`'s
`_aen_check_map_overlaps()` (the disjoint-slot0 overlap check, #2073).
`check_atoc_reservation.py`'s own 4b/4c aperture checks still hand-write
this same comparison independently (`lo == full_lo and hi == full_hi`,
not imported from here) -- a known, separate follow-up, not something
this module claims to have closed.

Zero dependencies on purpose, same reason as `sentinels.py`: importing
the `alp_orchestrate` PACKAGE from `gen_zephyr_board.py` for one
predicate pulled in the whole orchestrator (jsonschema, alp_project,
alp_cli, ~4x the import graph -- measured 53 -> 218 modules) and created
a package-to-generator import loop (`alp_orchestrate/loader.py` and
`secure.py` already work around a REAL one -- their own need for
`gen_zephyr_board`'s helpers -- with a deferred, function-local import;
a second, module-level edge in the opposite direction added a second
loop rather than reusing that pattern). Keeping the predicate in a flat,
package-free module lets both `aperture.py` and `gen_zephyr_board.py`
import it directly, with neither depending on the other's package.
"""

from __future__ import annotations


def is_whole_device_alias(
    ext: tuple[int, int], aperture: tuple[int, int],
) -> bool:
    """True when *ext* equals *aperture* exactly -- the whole-device alias
    case, not a partition inside the device.

    `classify_region()` and `is_partition_inside_aperture()` both test
    this FIRST, before their subset-containment check, because the
    alias's own extent also satisfies `lo >= full_lo and hi <= full_hi`
    -- order matters, not just the predicate. Comparing BOTH edges is
    the whole point: a region merely flush with the aperture's low edge
    (e.g. `mcuboot`, same `base` as `mram_main`, far smaller `size_kib`)
    must NOT match here, or a genuine overlap at that edge would be
    silently excluded from the overlap comparison instead of refused.
    """
    lo, hi = ext
    full_lo, full_hi = aperture
    return lo == full_lo and hi == full_hi

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Memory-region math shared across the resolvers -- a leaf.

The page granularity and the memory_map size-field -> bytes conversion, used by
both the carve-out resolver and the storage-partition resolver. A dependency-
free leaf (typing only) so each resolver imports it directly instead of one
reaching into the other (or back through the package __init__). Extracted as a
#285 leaf seam -- the same move as paths.py -- so the partition resolver lands
cleanly.
"""

from __future__ import annotations

from typing import Any, Optional

# Page granularity for carve-out / partition base + size alignment.
_PAGE = 4096


def _region_size_bytes(region: dict[str, Any]) -> Optional[int]:
    """Convert a memory_map entry's size_mib / size_kib to bytes.

    Reads each field by its VALUE, not by key presence: the
    `memory_region` schema lets `size_kib` and `size_mib` each
    independently be an int or the literal `"TBD"`, so a `"TBD"` in one
    field must never mask a usable integer sitting in the other (e.g.
    `{size_mib: "TBD", size_kib: 64}` must resolve via `size_kib`, not
    bail out because `size_mib` is present but unresolved). `size_mib`
    is checked first -- matching this module's real prior behaviour: on
    `origin/dev` before #1365 split B, this function's only two callers
    (`carveout.py` and `partition.py`) both went through a
    key-PRESENCE check that looked at `size_mib` first
    (`if "size_mib" in region: ...`). `check_atoc_reservation.py` was
    NOT a caller of this module then -- it carried its own private,
    unrelated `_region_size_kib` helper (kib-first), which split B
    retired in favour of importing this module instead. That gate
    becoming a caller here is what split B changed; it is not evidence
    of this module's own prior precedence, which was mib-first.

    The schema permits both fields on one region (neither `som-preset-
    v1.schema.json`'s `memory_region` nor `soc-spec-v1.schema.json`'s
    forbids it), and no region in `metadata/e1m_modules/*.yaml` today
    sets both, so this precedence has no real-preset evidence either
    way -- pinned here by test only, matching the module's actual
    history rather than a caller that did not exist yet.

    Returns None only when NEITHER field resolves to a usable integer.
    """
    size_mib = region.get("size_mib")
    if isinstance(size_mib, int) and not isinstance(size_mib, bool):
        return size_mib * 1024 * 1024
    size_kib = region.get("size_kib")
    if isinstance(size_kib, int) and not isinstance(size_kib, bool):
        return size_kib * 1024
    return None

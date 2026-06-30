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

    Returns None if the size field is unset OR is the literal 'TBD'.
    """
    if "size_mib" in region:
        v = region["size_mib"]
        if isinstance(v, int):
            return v * 1024 * 1024
        return None
    if "size_kib" in region:
        v = region["size_kib"]
        if isinstance(v, int):
            return v * 1024
        return None
    return None

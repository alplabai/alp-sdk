#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared metadata registry loaders for build-config emitters."""

from __future__ import annotations

import functools
import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT = REPO / "metadata"


@functools.lru_cache(maxsize=None)
def peripheral_kconfig(metadata_root: Path) -> dict[str, tuple[str, ...]]:
    """Return board.yaml peripheral tokens -> Zephyr Kconfig symbol bundles.

    *metadata_root* is REQUIRED -- every caller must pass the project's own
    ``project.effective_metadata_root()`` (or the SDK's own in-tree
    ``METADATA_ROOT`` for a repo self-check). A module-level default here
    is exactly the shape that let this registry silently ignore a project's
    `--metadata-root` override (#1485): the cache is keyed on
    *metadata_root* so a second root in the same process doesn't reuse the
    first root's table.
    """
    registry = Path(metadata_root) / "registries" / "peripheral-kconfig.json"
    data = json.loads(registry.read_text(encoding="utf-8"))
    return {
        token: tuple(symbols)
        for token, symbols in data["peripherals"].items()
    }

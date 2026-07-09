#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared metadata registry loaders for build-config emitters."""

from __future__ import annotations

import functools
import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT = REPO / "metadata"

PERIPHERAL_KCONFIG_REGISTRY = METADATA_ROOT / "registries" / "peripheral-kconfig.json"


@functools.lru_cache(maxsize=1)
def peripheral_kconfig() -> dict[str, tuple[str, ...]]:
    """Return board.yaml peripheral tokens -> Zephyr Kconfig symbol bundles."""
    data = json.loads(PERIPHERAL_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    return {
        token: tuple(symbols)
        for token, symbols in data["peripherals"].items()
    }

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
def peripheral_kconfig() -> dict[str, str]:
    """Return the board.yaml peripheral token -> Zephyr Kconfig symbol map."""
    data = json.loads(PERIPHERAL_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    return dict(data["peripherals"])

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
    """Return board.yaml peripheral tokens -> Zephyr Kconfig symbol bundles.

    Both callers of this function (`alp_orchestrate/slugs.py`,
    `alp_project_emit/__init__.py`) invoke it at IMPORT time, at module
    scope -- reached the moment anything imports `alp_orchestrate` or
    `alp_project_emit`, which includes `scripts/validate_metadata.py`
    itself via `from alp_orchestrate.sdk_compat import ...` ->
    `alp_orchestrate/__init__.py` -> `.loader` -> `alp_project` ->
    `alp_project_emit`. That is well before
    `validate_metadata.py::_check_peripheral_kconfig()`'s own schema pass
    ever runs, so a malformed on-disk registry (bad JSON, a non-object top
    level, or a `peripherals` value that isn't itself an object) used to
    surface as a raw `JSONDecodeError`/`KeyError`/`AttributeError`
    traceback several import-frames away from anything the caller wrote.
    Raise a `ValueError` naming the real problem instead.
    """
    rel = PERIPHERAL_KCONFIG_REGISTRY.relative_to(REPO).as_posix()
    try:
        data = json.loads(PERIPHERAL_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise ValueError(f"{rel}: invalid JSON ({e})") from e
    peripherals = data.get("peripherals") if isinstance(data, dict) else None
    if not isinstance(peripherals, dict):
        raise ValueError(
            f"{rel}: expected a top-level object with a `peripherals` "
            f"object (got {type(data).__name__})")
    result: dict[str, tuple[str, ...]] = {}
    for token, symbols in peripherals.items():
        if not isinstance(symbols, list):
            raise ValueError(
                f"{rel}: peripherals[{token!r}] must be an array of "
                f"Zephyr Kconfig symbols (got {type(symbols).__name__})")
        result[token] = tuple(symbols)
    return result

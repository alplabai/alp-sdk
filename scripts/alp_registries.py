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
    `--metadata-root` override (#1485); the cache is keyed on
    *metadata_root* so a second root in the same process doesn't reuse the
    first root's table.

    The validation below is #1545's, re-rooted at *metadata_root*. Both
    callers (`alp_orchestrate/slugs.py`, `alp_project_emit/__init__.py`)
    reach this at import time, well before
    `validate_metadata.py::_check_peripheral_kconfig()`'s own schema pass,
    so a malformed on-disk registry used to surface as a raw
    `JSONDecodeError`/`KeyError`/`AttributeError` several import-frames
    away from anything the caller wrote. Raise a `ValueError` naming the
    real problem instead.
    """
    registry = Path(metadata_root) / "registries" / "peripheral-kconfig.json"
    try:
        rel = registry.relative_to(REPO).as_posix()
    except ValueError:
        # `metadata_root` legitimately points outside REPO (a scratch root
        # under `--metadata-root`, or a test fixture) -- `relative_to()`
        # raises its own confusing "is not in the subpath of" there, which
        # would mask whatever the caller actually did wrong. Fall back to
        # the raw path string.
        rel = str(registry)
    try:
        text = registry.read_text(encoding="utf-8")
    except OSError as e:
        # A missing/unreadable file is not "invalid JSON" -- report the
        # real failure instead of mislabeling every OSError that way.
        raise ValueError(f"{rel}: cannot read registry ({e})") from e
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        raise ValueError(f"{rel}: invalid JSON ({e})") from e
    if not isinstance(data, dict):
        raise ValueError(
            f"{rel}: expected a top-level object (got {type(data).__name__})")
    if "peripherals" not in data:
        raise ValueError(f"{rel}: missing top-level `peripherals` key")
    peripherals = data["peripherals"]
    if not isinstance(peripherals, dict):
        raise ValueError(
            f"{rel}: `peripherals` must be an object "
            f"(got {type(peripherals).__name__})")
    result: dict[str, tuple[str, ...]] = {}
    for token, symbols in peripherals.items():
        if not isinstance(symbols, list):
            raise ValueError(
                f"{rel}: peripherals[{token!r}] must be an array of "
                f"Zephyr Kconfig symbols (got {type(symbols).__name__})")
        result[token] = tuple(symbols)
    return result

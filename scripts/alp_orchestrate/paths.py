#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Filesystem roots for the orchestrator package -- the bottom leaf.

`REPO`, `METADATA_ROOT`, and the schema paths, derived from this file's
location. A dependency-free leaf (only `pathlib`) so any sibling -- topology,
the loader, the future carve-out / partition resolvers -- can import a path
root directly instead of reaching back into the package __init__ (which would
cycle). Extracting these is the #285 paths seam that lets topology.py drop its
lazy `BOARD_SCHEMA` import.
"""

from __future__ import annotations

from pathlib import Path

# paths.py sits at scripts/alp_orchestrate/paths.py, so the repo root is three
# parents up -- the same derivation the package __init__ used.
REPO = Path(__file__).resolve().parent.parent.parent
METADATA_ROOT = REPO / "metadata"
BOARD_SCHEMA = METADATA_ROOT / "schemas" / "board.schema.json"
BOARD_PRESET_SCHEMA = METADATA_ROOT / "schemas" / "board-preset.schema.json"

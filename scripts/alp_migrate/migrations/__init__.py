# SPDX-License-Identifier: Apache-2.0
"""Ordered board.yaml migration registry (epic #610 WS6-b).

Each entry is `(FROM, TO, apply_text_fn)`: a step migrating a board.yaml from
schema version FROM to TO, ordered by TO ascending. `apply_text_fn(lines,
report)` edits the file's lines in place (byte-faithful) and writes its own
`schemaVersion: TO` bump via `alp_migrate.set_schema_version`.

Empty until the first real schema change: with lazy versioning a board.yaml
with no `schemaVersion` already IS v1, so no adoption/stamp step exists. Add a
v1->v2 as a new `m001_to_v2.py` module here + one line below, and bump
`alp_migrate.LATEST` to 2 in the same change.
"""
from __future__ import annotations

# (FROM, TO, apply_text_fn), ordered by TO ascending.
STEPS: list = []

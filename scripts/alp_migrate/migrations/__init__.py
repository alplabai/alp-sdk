# SPDX-License-Identifier: Apache-2.0
"""Ordered board.yaml migration registry (epic #610 WS6-b).

Each entry is `(FROM, TO, apply_text_fn)`: a step migrating a board.yaml from
schema version FROM to TO, ordered by TO ascending. `apply_text_fn(lines,
report)` edits the file's lines in place (byte-faithful) and writes its own
`schemaVersion: TO` bump via `alp_migrate.set_schema_version`.

The first real schema change is v1->v2 (WS6-c #610 §6): `m001_to_v2` unifies the
`libraries:` declaration.  `alp_migrate.LATEST` is bumped to 2 in lockstep.
"""
from __future__ import annotations

from . import m001_to_v2

# (FROM, TO, apply_text_fn), ordered by TO ascending.
STEPS: list = [
    (1, 2, m001_to_v2.apply),
]

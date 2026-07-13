# SPDX-License-Identifier: Apache-2.0
"""Ordered board.yaml migration registry (epic #610 WS6-b).

Each entry is a (FROM, TO, transform) step. FROM is None for the adoption
step (unversioned -> 1); an integer for a version bump. Ordered by TO.
Add a future v1->v2 as a new mN_to_vN1 module + one line here.
"""
from __future__ import annotations

from . import m000_to_v1

# (FROM, TO, transform_callable), ordered by TO ascending.
STEPS = [
    (m000_to_v1.FROM, m000_to_v1.TO, m000_to_v1.transform),
]

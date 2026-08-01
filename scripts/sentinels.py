#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared `TBD` placeholder-sentinel detection (issue #1048).

`TBD` is a deliberate repo convention: where exact hardware configuration
isn't known yet, the maintainer hand-writes `TBD` rather than inventing a
value. Because it's hand-typed, `tbd`, `Tbd` and `" TBD "` are all natural
variants -- `is_tbd()` is the one place that normalises before comparing,
so every call site catches them the same way instead of each repeating its
own (previously inconsistent) `== "TBD"` guard.

Zero dependencies on purpose: this is imported from both the top-level
`scripts/` modules and `alp_orchestrate/`, so it must not create an import
cycle with either.
"""

from __future__ import annotations

from typing import Any


def is_tbd(value: Any) -> bool:
    """True iff @p value is the `TBD` placeholder sentinel.

    Compares case- and surrounding-whitespace-insensitively (`"tbd"`,
    `"Tbd"`, `" TBD "` all match) but only on an exact match after that
    normalisation -- `"TBD_RESERVED"` is a real value, not a placeholder,
    and must not match.
    """
    return isinstance(value, str) and value.strip().upper() == "TBD"

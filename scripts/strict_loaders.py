#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Duplicate-key-rejecting YAML/JSON loaders (issue #1127).

`yaml.safe_load` and `json.loads` silently keep only the LAST value of a
duplicated mapping key -- `yaml.safe_load("som: a\\nsom: b\\n")` returns
`{'som': 'b'}` with no error or warning. In a hand-edited board.yaml,
SoM preset, or chip/SoC manifest, a duplicated key is invisible in a
diff and silently drops hardware configuration. `strict_yaml_load()`
and `strict_json_loads()` are the one place every metadata ingestion
boundary should route through instead of the raw stdlib loaders.

`fast_safe_load()` is the lenient sibling: exactly `yaml.safe_load`
semantics (duplicate keys keep the last value) for hot readers that do not
opt into rejection. Both YAML paths parse with libyaml's C parser whenever
PyYAML was built with it, and fall back to the pure-Python one otherwise
(#2328).

Zero dependencies beyond PyYAML on purpose: this is imported from both
the top-level `scripts/` modules and `alp_orchestrate/`/`alp_cli/`, so
it must not create an import cycle with either (mirrors `sentinels.py`).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import yaml


class DuplicateKeyError(ValueError):
    """Raised when a YAML/JSON mapping repeats a key."""


def _no_duplicates_mapping_constructor(
    loader: yaml.constructor.SafeConstructor, node: yaml.MappingNode
) -> dict[str, Any]:
    # Duplicate-key detection must run on the node's OWN explicit pairs
    # BEFORE `flatten_mapping()` splices in `<<: *anchor` merge-key
    # pairs -- otherwise a spec-legal merge override (the anchor
    # supplies a default, a sibling explicit key overrides it) reads as
    # the same key twice and gets rejected as a duplicate, when the
    # explicit value should just win, exactly like the stdlib loader
    # does. Scanning only the node's own pairs also leaves two merge
    # SOURCES quietly disagreeing on a key to the normal YAML
    # first-wins merge semantics, not a hard error -- this loader's job
    # is to catch a literal duplicate key typed twice in one mapping.
    seen: set[Any] = set()
    for key_node, _value_node in node.value:
        if key_node.tag == "tag:yaml.org,2002:merge":
            continue
        key = loader.construct_object(key_node, deep=True)
        if key in seen:
            mark = key_node.start_mark
            raise DuplicateKeyError(
                f"duplicate key {key!r} at line {mark.line + 1}, "
                f"column {mark.column + 1}"
            )
        seen.add(key)

    loader.flatten_mapping(node)
    mapping: dict[str, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        mapping[key] = loader.construct_object(value_node, deep=True)
    return mapping


# libyaml's C parser when PyYAML was built with it (its wheels are), the
# pure-Python one otherwise. Same SafeConstructor either way, so the same
# values and the same duplicate-key rejection -- only ~10x faster to parse.
# YAML parsing was ~45% of tests/scripts' CPU time, most of it through the
# loaders in this module (#2328).
_SAFE_LOADER = getattr(yaml, "CSafeLoader", yaml.SafeLoader)


class _StrictLoader(_SAFE_LOADER):
    pass


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _no_duplicates_mapping_constructor
)


def strict_yaml_load(text: str, source: str | Path = "<string>") -> Any:
    """`yaml.safe_load`, but raise `DuplicateKeyError` on a repeated mapping key."""
    try:
        return yaml.load(text, Loader=_StrictLoader)
    except DuplicateKeyError as e:
        raise DuplicateKeyError(f"{source}: {e}") from e


def fast_safe_load(text: str) -> Any:
    """Exactly `yaml.safe_load(text)`, parsed by libyaml when available.

    For the hot metadata readers that want stdlib semantics (duplicate keys
    keep the last value) rather than `strict_yaml_load`'s rejection.
    """
    return yaml.load(text, Loader=_SAFE_LOADER)


def _no_duplicates_object_pairs_hook(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    mapping: dict[str, Any] = {}
    for key, value in pairs:
        if key in mapping:
            raise DuplicateKeyError(f"duplicate key {key!r}")
        mapping[key] = value
    return mapping


def strict_json_loads(text: str, source: str | Path = "<string>") -> Any:
    """`json.loads`, but raise `DuplicateKeyError` on a repeated object key."""
    try:
        return json.loads(text, object_pairs_hook=_no_duplicates_object_pairs_hook)
    except DuplicateKeyError as e:
        raise DuplicateKeyError(f"{source}: {e}") from e

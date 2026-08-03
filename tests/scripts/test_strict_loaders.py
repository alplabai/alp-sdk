# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/strict_loaders.py (issue #1127).

`yaml.safe_load`/`json.loads` silently keep only the last value of a
duplicated mapping key. These tests prove the shared strict loaders
reject that instead of the underlying stdlib functions accepting it.
"""

import pytest

from strict_loaders import DuplicateKeyError, strict_json_loads, strict_yaml_load


def test_strict_yaml_load_accepts_normal_document():
    assert strict_yaml_load("som: a\npreset: b\n") == {"som": "a", "preset": "b"}


def test_strict_yaml_load_rejects_duplicate_top_level_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'som'"):
        strict_yaml_load("som: a\nsom: b\n")


def test_strict_yaml_load_rejects_duplicate_nested_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'sku'"):
        strict_yaml_load("som:\n  sku: a\n  sku: b\n")


def test_strict_yaml_load_error_includes_source():
    with pytest.raises(DuplicateKeyError, match=r"board\.yaml:"):
        strict_yaml_load("som: a\nsom: b\n", source="board.yaml")


def test_strict_yaml_load_accepts_merge_key_override():
    """A `<<: *anchor` merge key's value merged with an explicit sibling
    key of the same name is spec-legal YAML -- the explicit key wins.
    `flatten_mapping()` splices the anchor's pairs in ahead of the node's
    own explicit pairs, so a naive "have I seen this key" scan (run
    AFTER flattening) sees the same key twice and misreports a legal
    override as a duplicate. The duplicate scan must run on the node's
    own pairs BEFORE flatten_mapping() merges the anchor in."""
    doc = strict_yaml_load("base: &b\n  a: 1\nderived:\n  <<: *b\n  a: 9\n")
    assert doc == {"base": {"a": 1}, "derived": {"a": 9}}


def test_strict_yaml_load_still_rejects_duplicate_alongside_merge_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'a'"):
        strict_yaml_load("base: &b\n  a: 1\nderived:\n  <<: *b\n  a: 9\n  a: 10\n")


def test_strict_json_loads_accepts_normal_document():
    assert strict_json_loads('{"som": "a", "preset": "b"}') == {
        "som": "a",
        "preset": "b",
    }


def test_strict_json_loads_rejects_duplicate_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'som'"):
        strict_json_loads('{"som": "a", "som": "b"}')


def test_strict_json_loads_rejects_duplicate_nested_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'sku'"):
        strict_json_loads('{"som": {"sku": "a", "sku": "b"}}')


def test_baseline_stdlib_loaders_silently_drop_the_duplicate():
    """Not a regression test -- documents WHY the strict loaders exist.

    `yaml.safe_load`/`json.loads` are the pre-#1127 baseline: both keep
    only the last value with no error, which is the exact silent-drop
    hazard `strict_yaml_load`/`strict_json_loads` close.
    """
    import json

    import yaml

    assert yaml.safe_load("som: a\nsom: b\n") == {"som": "b"}
    assert json.loads('{"som": "a", "som": "b"}') == {"som": "b"}

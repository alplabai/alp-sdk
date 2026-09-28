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


def test_loaders_use_libyaml_when_pyyaml_has_it():
    """#2328: YAML parsing was ~45% of tests/scripts' CPU time, most of it
    through these loaders. libyaml's C parser (in PyYAML's wheels) is ~10x
    faster, so both must use it whenever PyYAML was built with it."""
    import yaml

    import strict_loaders as sl

    if not yaml.__with_libyaml__:
        pytest.skip("PyYAML built without libyaml -- the pure-Python fallback applies")
    assert issubclass(sl._StrictLoader, yaml.CSafeLoader)
    assert sl.fast_safe_load("som: a\nlist: [1, 2]\n") == {"som": "a", "list": [1, 2]}


def _load_outcome(fn, text):
    try:
        return ("ok", fn(text))
    except Exception as e:  # noqa: BLE001 -- compare the failure CLASS, not text
        return ("raise", type(e).__name__)


def test_c_loaders_match_the_pure_python_ones_on_every_repo_yaml():
    """The C-backed loaders must give exactly what the pure-Python ones do --
    same value, or the same exception class -- on every YAML file the repo
    tracks. Covers both `fast_safe_load` (vs `yaml.safe_load`) and
    `strict_yaml_load` (vs the same duplicate-key constructor on the
    pure-Python `SafeLoader`)."""
    import subprocess
    from pathlib import Path

    import yaml

    import strict_loaders as sl

    class _PureStrict(yaml.SafeLoader):
        pass

    _PureStrict.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
        sl._no_duplicates_mapping_constructor,
    )

    repo = Path(__file__).resolve().parents[2]
    tracked = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "*.yaml", "*.yml"],
        capture_output=True, text=True, encoding="utf-8", check=True,
    ).stdout.split()
    assert len(tracked) > 100, tracked[:5]
    # Every tracked file parses cleanly, so add inputs that raise -- or the
    # "same exception class" half of this test would never be exercised.
    malformed = {
        "<duplicate key>": "som: a\nsom: b\n",
        "<duplicate key via merge>": "base: &b {x: 1}\nm:\n  <<: *b\n  y: 2\n  y: 3\n",
        "<tab indent>": "a:\n\tb: 1\n",
        "<unclosed flow sequence>": "a: [1, 2\nb: 3\n",
        "<two documents>": "a: 1\n---\nb: 2\n",
    }
    assert all(
        _load_outcome(sl.strict_yaml_load, t)[0] == "raise" for t in malformed.values()
    ), "a malformed fixture no longer raises -- it stopped testing the error path"
    inputs = [(rel, (repo / rel).read_text(encoding="utf-8")) for rel in tracked]
    inputs += list(malformed.items())
    mismatches = []
    for rel, text in inputs:
        if _load_outcome(sl.fast_safe_load, text) != _load_outcome(yaml.safe_load, text):
            mismatches.append(("fast_safe_load", rel))
        pure_strict = lambda t: yaml.load(t, Loader=_PureStrict)  # noqa: E731
        if _load_outcome(sl.strict_yaml_load, text) != _load_outcome(pure_strict, text):
            mismatches.append(("strict_yaml_load", rel))
    assert not mismatches, mismatches[:10]

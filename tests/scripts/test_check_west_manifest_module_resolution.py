# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_west_manifest_module_resolution.py.

A gate that only ever passes proves nothing: each failure-mode test below
mutates a TEMP COPY of west.yml (or adds a temp library manifest) to
reproduce the exact #1136 shape, asserts the gate fires, then asserts it
clears once the corpus is fixed the same way the real west.yml was.

Run locally:

    python -m pytest tests/scripts/test_check_west_manifest_module_resolution.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "scripts"))
import check_west_manifest_module_resolution as gate  # noqa: E402

_MINIMAL_WEST_YML = """\
manifest:
  defaults:
    remote: zephyrproject
  remotes:
    - name: zephyrproject
      url-base: https://github.com/zephyrproject-rtos
  projects:
    - name: zephyr
      revision: v4.4.1
      import:
        name-allowlist:
          - littlefs
"""


def _scaffold(tmp_path: Path, west_yml_text: str, libraries: dict[str, str]) -> Path:
    (tmp_path / "west.yml").write_text(west_yml_text, encoding="utf-8")
    libdir = tmp_path / "metadata" / "libraries"
    libdir.mkdir(parents=True)
    for name, text in libraries.items():
        (libdir / f"{name}.yaml").write_text(text, encoding="utf-8")
    return tmp_path


def test_real_repo_is_clean():
    """The gate must pass against the actual, fixed west.yml -- regression
    guard for the #1136 fix itself."""
    assert gate.find_problems(REPO) == []


def test_missing_module_is_flagged(tmp_path):
    """A library naming a west module that appears NOWHERE in west.yml
    (neither a top-level project nor an allowlist entry) -- the original
    #1136 shape before the fix landed."""
    root = _scaffold(
        tmp_path,
        _MINIMAL_WEST_YML,
        {
            "nanopb": (
                "schema_version: 1\n"
                "name: nanopb\n"
                "integration:\n"
                "  zephyr:\n"
                "    module: nanopb\n"
            ),
        },
    )
    problems = gate.find_problems(root)
    assert any("nanopb" in p and "no top-level project" in p for p in problems), problems


def test_allowlist_entry_shadowed_by_same_named_top_level_project(tmp_path):
    """#650's exact failure: a top-level project shares its name with an
    allowlist entry, so the allowlist entry can never win -- even though the
    module name superficially "looks" reachable."""
    west_yml = _MINIMAL_WEST_YML + """\
    - name: nanopb
      remote: zephyrproject
      revision: nanopb-0.4.9
      path: modules/lib/nanopb
      groups:
        - extras-lwrb-nanopb
  group-filter:
    - -extras-lwrb-nanopb
"""
    # Re-add nanopb to the allowlist to reproduce the exact collision.
    west_yml = west_yml.replace(
        "        name-allowlist:\n          - littlefs\n",
        "        name-allowlist:\n          - littlefs\n          - nanopb\n",
    )
    root = _scaffold(tmp_path, west_yml, {})
    problems = gate.find_problems(root)
    assert any("shares its name" in p and "'nanopb'" in p for p in problems), problems


def test_renaming_the_shadowing_project_clears_the_collision(tmp_path):
    """The actual #1136 fix shape: renaming the colliding top-level project
    (not touching the allowlist) makes the collision check pass again."""
    west_yml = _MINIMAL_WEST_YML + """\
    - name: nanopb-mproc-pin
      remote: zephyrproject
      repo-path: nanopb
      revision: nanopb-0.4.9
      path: modules/lib/nanopb-mproc-pin
      groups:
        - extras-lwrb-nanopb
  group-filter:
    - -extras-lwrb-nanopb
"""
    west_yml = west_yml.replace(
        "        name-allowlist:\n          - littlefs\n",
        "        name-allowlist:\n          - littlefs\n          - nanopb\n",
    )
    root = _scaffold(
        tmp_path,
        west_yml,
        {
            "nanopb": (
                "schema_version: 1\n"
                "name: nanopb\n"
                "integration:\n"
                "  zephyr:\n"
                "    module: nanopb\n"
            ),
        },
    )
    assert gate.find_problems(root) == []


def test_tier_a_module_reachable_only_via_disabled_group_is_flagged(tmp_path):
    """The literal ORIGINAL #1136 shape (before #650's allowlist attempt even
    existed): a top-level project named after the module sits ONLY inside a
    permanently-disabled group, and the allowlist never names it at all.
    Check 2 alone treats the top-level name as "reachable" regardless of
    group state and misses this -- check 3 exists precisely because the
    library is `tier: A` (ADR 0018: must resolve with no group opt-in)."""
    west_yml = _MINIMAL_WEST_YML + """\
    - name: nanopb
      remote: zephyrproject
      revision: nanopb-0.4.9
      path: modules/lib/nanopb
      groups:
        - extras-lwrb-nanopb
  group-filter:
    - -extras-lwrb-nanopb
"""
    root = _scaffold(
        tmp_path,
        west_yml,
        {
            "nanopb": (
                "schema_version: 1\n"
                "name: nanopb\n"
                "tier: A\n"
                "integration:\n"
                "  zephyr:\n"
                "    module: nanopb\n"
            ),
        },
    )
    problems = gate.find_problems(root)
    assert any("tier: A" in p and "'nanopb'" in p for p in problems), problems


def test_tier_b_module_reachable_only_via_disabled_group_is_not_flagged(tmp_path):
    """The same shape as above, but `tier: B` -- a documented, working
    opt-in (jsmn, Catch2, BearSSL, etc. all live here today by design).
    Check 3 must NOT fire; only tier-A libraries are held to the
    group-unconditioned bar."""
    west_yml = _MINIMAL_WEST_YML + """\
    - name: nanopb
      remote: zephyrproject
      revision: nanopb-0.4.9
      path: modules/lib/nanopb
      groups:
        - extras-lwrb-nanopb
  group-filter:
    - -extras-lwrb-nanopb
"""
    root = _scaffold(
        tmp_path,
        west_yml,
        {
            "nanopb": (
                "schema_version: 1\n"
                "name: nanopb\n"
                "tier: B\n"
                "integration:\n"
                "  zephyr:\n"
                "    module: nanopb\n"
            ),
        },
    )
    assert gate.find_problems(root) == []


def test_self_contained_west_pin_library_is_exempt(tmp_path):
    """A library carrying its own `integration.zephyr.west:` block (the ADR
    0018 Tier-B `--emit west-libraries` path) is resolved by a different
    mechanism entirely and must not be flagged just because west.yml itself
    has no matching entry."""
    root = _scaffold(
        tmp_path,
        _MINIMAL_WEST_YML,
        {
            "azure-iot": (
                "schema_version: 1\n"
                "name: azure-iot\n"
                "integration:\n"
                "  zephyr:\n"
                "    module: azure-sdk-for-c\n"
                "    west:\n"
                "      name: azure-sdk-for-c\n"
                "      url: https://github.com/Azure/azure-sdk-for-c.git\n"
                "      revision: 1.5.0\n"
                "      path: modules/lib/azure-sdk-for-c\n"
            ),
        },
    )
    assert gate.find_problems(root) == []

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Assert the curated-library registry is self-consistent (WS6-c #610 §6).

WS6-c retired the per-core `cores.<id>.libraries:` enum in favour of one
top-level, manifest-backed model: board.yaml declares curated libraries via
`libraries: [{name, cores?}]` (metadata/schemas/board.schema.json
`libraries`; folded per-core by `alp_orchestrate/loader.py`
`_normalize_libraries`).  A `cores.<id>.libraries:` key is now schema-
rejected outright (`$defs/core_entry` has no such property; only
`extra_libraries`).

An earlier version of this gate still walked board.schema.json for the
retired per-core `enum` shape -- that shape has not existed since WS6-c, so
the walk always returned an empty set and the gate passed vacuously no
matter what the real registry looked like (#1197).  This version reads the
REAL registry instead:

  - metadata/library-aliases-v1.json validates against its schema;
  - every alias value names an existing metadata/libraries/<name>.yaml
    manifest (no dangling canonical name);
  - every top-level `libraries:` entry in every board.yaml this walk finds
    resolves (directly, or via the alias table) to an existing manifest --
    the canonical top-level shape, walked for real instead of the retired
    enum;
  - `alp_orchestrate/validate.py`'s `_CURATED_LIBRARIES` set (the guard
    that rejects an `extra_libraries:` entry colliding with a curated
    library) is cross-checked against the same manifest directory, so a
    newly added curated library can't silently slip past that collision
    guard -- only when `root` is this script's own repo (see
    `find_problems`'s docstring below for why).

stdlib + jsonschema + PyYAML.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import jsonschema
import yaml

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

# Fallback-only: dirs to prune when `root` isn't a git worktree (see
# `_board_yaml_files`).  git itself needs no such list -- `--exclude-
# standard` already reads `.gitignore`, the one source of truth for
# "this is build output, not a source". Widen if a future build tool
# lands a new output dir AND git is ever genuinely unavailable here.
_FALLBACK_SKIP_DIRS = frozenset({".git", ".west", "build", "twister-out"})


def _manifest_names(root: Path) -> set[str]:
    """Canonical library names -- one per metadata/libraries/<name>.yaml."""
    libdir = root / "metadata" / "libraries"
    if not libdir.is_dir():
        return set()
    return {p.stem for p in libdir.glob("*.yaml")}


def _board_yaml_files(root: Path) -> list[Path]:
    """Every board.yaml under `root` -- tracked *or* newly-created-but-
    not-yet-`git add`ed, since `--others --exclude-standard` includes
    untracked files while still honouring `.gitignore`.

    A prior version did `root.rglob("board.yaml")`: an unbounded walk from
    repo root that also descends into build output (`twister-out/`,
    `build/`) with no notion of `.gitignore` at all -- measured at 160k+
    files of build artefacts against ~100 real board.yaml, and slow/hanging
    on some filesystems. `git ls-files` prunes whole directories
    matched by `.gitignore` (`twister-out/`, `build/`, ...) without
    descending into them, so this is fast regardless of how much build
    output happens to be sitting in the tree.

    Falls back to a directory-pruned walk only when `root` isn't a git
    worktree at all (this gate's own test scaffolds under tmp_path, which
    have no build output to prune in the first place)."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others",
             "--exclude-standard", "--", "*board.yaml"],
            check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        found: list[Path] = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in _FALLBACK_SKIP_DIRS]
            if "board.yaml" in filenames:
                found.append(Path(dirpath) / "board.yaml")
        return sorted(found)
    return sorted(root / line for line in proc.stdout.splitlines() if line)


def _top_level_library_names(board_yaml: Path) -> list[str]:
    """Names in a board.yaml's top-level `libraries:` -- a bare string
    (project-wide shorthand) or a `{name, cores?}` object.  Malformed YAML
    is some other gate's concern; return no names rather than raise."""
    try:
        doc = yaml.safe_load(board_yaml.read_text(encoding="utf-8"))
    except yaml.YAMLError:
        return []
    if not isinstance(doc, dict):
        return []
    names: list[str] = []
    for entry in doc.get("libraries") or []:
        if isinstance(entry, str):
            names.append(entry)
        elif isinstance(entry, dict) and isinstance(entry.get("name"), str):
            names.append(entry["name"])
    return names


def _registry_vs_curated(manifests: set[str], aliases: dict[str, str],
                          curated: frozenset[str]) -> list[str]:
    """Compare the real manifest registry against
    `alp_orchestrate.validate._CURATED_LIBRARIES`, the derived set the
    extra_libraries-vs-curated collision guard reads.

    Every manifest must be reachable from `curated` -- via any alias
    table legacy token that maps to it, else via the manifest's own name
    with hyphens folded to underscores (the convention every existing
    alias already follows except `libcoap` -> `coap`, a legacy rename
    that isn't a hyphen fold of its canonical name at all: `cmsis-dsp` ->
    `cmsis_dsp`, `nlohmann-json` -> `nlohmann_json`, ...).  A manifest
    that isn't reachable can be re-declared through `extra_libraries:`
    without the collision guard ever firing.  A `curated` entry that maps
    to no manifest is a stale leftover.

    `aliases` is token -> canonical, many-to-one in principle (two legacy
    tokens could name the same manifest) even though today's table has no
    such collision -- reverse_alias is therefore token-SET valued so a
    future collision can't silently drop a token from `expected`.
    """
    reverse_alias: dict[str, set[str]] = {}
    for token, canonical in aliases.items():
        reverse_alias.setdefault(canonical, set()).add(token)

    expected: set[str] = set()
    problems: list[str] = []
    for m in sorted(manifests):
        acceptable = reverse_alias.get(m) or {m.replace("-", "_")}
        expected |= acceptable
        if not acceptable & curated:
            problems.append(
                f"alp_orchestrate/validate.py _CURATED_LIBRARIES is missing "
                f"'{sorted(acceptable)[0]}' -- a real metadata/libraries "
                f"manifest the extra_libraries collision check will not "
                f"catch")
    for stale in sorted(curated - expected):
        problems.append(
            f"alp_orchestrate/validate.py _CURATED_LIBRARIES has stale "
            f"entry '{stale}' -- no metadata/libraries manifest resolves "
            f"to it")
    return problems


def find_problems(root: Path) -> list[str]:
    """Every layer below reads its data from `root`.  The one exception is
    the `_CURATED_LIBRARIES` collision-list layer near the end of this
    function, which is Python source rather than repo data and is only
    checked when `root == ROOT` (this script's own repo) -- see the
    comment at that call site."""
    problems: list[str] = []
    alias_p = root / "metadata/library-aliases-v1.json"
    schema_p = root / "metadata/schemas/library-aliases-v1.schema.json"
    if not alias_p.is_file() or not schema_p.is_file():
        return [f"missing {alias_p if not alias_p.is_file() else schema_p}"]
    schema = json.loads(schema_p.read_text(encoding="utf-8"))
    table = json.loads(alias_p.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(table)
    except jsonschema.ValidationError as e:
        return [f"alias schema: {e.message}"]
    aliases: dict[str, str] = table["aliases"]

    manifests = _manifest_names(root)
    for tok, canonical in sorted(aliases.items()):
        if canonical not in manifests:
            problems.append(
                f"alias '{tok}' -> '{canonical}' names no "
                f"metadata/libraries/{canonical}.yaml")

    # The canonical top-level `libraries:` shape -- walk every board.yaml
    # under `root` for real and confirm each declared name resolves to a
    # real manifest.  Replaces the #1197 walk of the retired per-core
    # `enum` shape, which always found nothing to check.
    boards_checked = 0
    for by in _board_yaml_files(root):
        names = _top_level_library_names(by)
        if not names:
            continue
        boards_checked += 1
        for name in names:
            canonical = aliases.get(name, name)
            if canonical not in manifests:
                try:
                    rel = by.relative_to(root)
                except ValueError:
                    rel = by
                problems.append(
                    f"{rel}: libraries entry '{name}' resolves to "
                    f"'{canonical}', which names no "
                    f"metadata/libraries/{canonical}.yaml")
    if boards_checked == 0:
        problems.append(
            "no board.yaml declares a top-level `libraries:` entry -- "
            "the registry check has nothing real to validate against")

    # #1197's other finding: the extra_libraries collision guard's set,
    # checked against the same real registry -- only when `root` IS this
    # script's own repo.  `_CURATED_LIBRARIES` lives in Python source
    # (alp_orchestrate/validate.py) that this process has already
    # imported from ROOT; there is no meaningful way to re-import a
    # DIFFERENT repo's copy of that module for an arbitrary `root`
    # (alp_orchestrate is already cached in sys.modules the moment
    # anything in this process imports it once).  Every real caller only
    # ever passes ROOT (see main() below); a test scaffold under a
    # foreign tmp_path root has no scripts/alp_orchestrate/ tree of its
    # own to check, so this layer correctly contributes nothing rather
    # than silently reporting the wrong tree's problems (the bug this
    # comment replaces).
    if root == ROOT:
        from alp_orchestrate.validate import _CURATED_LIBRARIES
        problems.extend(_registry_vs_curated(manifests, aliases, _CURATED_LIBRARIES))

    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"library-registry: {p}", file=sys.stderr)
        return 1
    boards = sum(1 for by in _board_yaml_files(ROOT) if _top_level_library_names(by))
    print(f"OK: library registry is self-consistent -- {boards} board.yaml "
          f"file(s) declare top-level `libraries:`, {len(_manifest_names(ROOT))} "
          f"curated manifests, alias table + extra_libraries collision guard "
          f"both resolve cleanly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

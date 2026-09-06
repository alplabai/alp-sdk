#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure builders for `alp.lock` (epic #610 WS6-a).

`build_lock` collects the reproducible, public-safe dependency + toolchain
inputs of an Alp SDK workspace into a schema-validated dict; `verify_lock`
recomputes and diffs.  No IO beyond reads.

`west alp-lock --check` (#1576) does NOT call `verify_lock` -- alp.lock is no
longer committed in this repo, so there is nothing in-tree to diff against;
`--check` only proves the generator still produces a schema-valid lock.
`verify_lock` remains the public entry point for a LOCK CONSUMER: given the
`alp.lock` shipped inside a release tarball (or the SBOM), a downstream build
can `build_lock`-recompute against its own checkout and `verify_lock` the two
to detect drift from the locked inputs -- the same "reproduce this release"
use case #610 was written for, just invoked by the consumer instead of by
this repo's own CI. It is exercised in-tree by `test_alp_lock.py` and by
`test_alp_lock_metadata_coverage.py`'s #1045 glob-coverage guard.
"""
from __future__ import annotations

import hashlib
import re
from pathlib import Path, PurePosixPath
from typing import Any, Optional

import yaml


class LockError(Exception):
    """Raised on an un-lockable input (e.g. a local path leaking in)."""


_LOCAL_PATH = re.compile(r"(^/)|(^[A-Za-z]:[\\/])|(^~)|(^\.{1,2}/)")


def _reject_local(value: str) -> str:
    """Return `value` unchanged, or raise if it looks like a local/abs path.
    Public URLs, version strings, SHAs, and licenses pass."""
    if isinstance(value, str) and _LOCAL_PATH.search(value):
        raise LockError(f"refusing to lock a local/abs path: {value!r}")
    return value


def _sdk_identity(root: Path) -> dict:
    # Single source of truth: metadata/sdk_version.yaml. (scripts/alp_cli's
    # __version__ derives from this same file at import time -- reading it
    # directly avoids importing/exec-ing the CLI package just to get a string.)
    txt = (root / "metadata" / "sdk_version.yaml").read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+\.\d+\.\d+(?:-[\w.]+)?)\s*$", txt, re.M)
    if not m:
        # Silently baking a wrong version would only surface later as
        # spurious --check drift -- fail loudly at generation instead.
        raise LockError("could not parse 'version:' from "
                        "metadata/sdk_version.yaml")
    # No `revision` here on purpose -- see the `sdk.revision` note above
    # `_PROVENANCE_KEYS`.
    return {"version": _reject_local(m.group(1))}


def _west_projects(root: Path) -> dict:
    doc = yaml.safe_load((root / "west.yml").read_text(encoding="utf-8")) or {}
    man = doc.get("manifest") or {}
    projects = []
    for p in man.get("projects") or []:
        if not isinstance(p, dict) or "name" not in p:
            continue
        rev = p.get("revision")
        projects.append({
            "name": _reject_local(str(p["name"])),
            "revision": _reject_local(str(rev)) if rev is not None else None,
            "groups": sorted(_reject_local(str(g)) for g in (p.get("groups") or [])),
        })
    projects.sort(key=lambda e: e["name"])
    gf = [_reject_local(str(g)) for g in (man.get("group-filter") or [])]
    return {"projects": projects, "groupFilter": gf}


def _libraries(root: Path) -> list:
    out = []
    libdir = root / "metadata" / "libraries"
    for f in sorted(libdir.glob("*.yaml")) if libdir.is_dir() else []:
        doc = yaml.safe_load(f.read_text(encoding="utf-8")) or {}
        if not isinstance(doc, dict) or "name" not in doc:
            continue
        west = (((doc.get("integration") or {}).get("zephyr") or {}).get("west") or {})
        rev = west.get("revision")
        out.append({
            "name": _reject_local(str(doc["name"])),
            "version": (_reject_local(str(doc["version"])) if doc.get("version") is not None else None),
            "license": (_reject_local(str(doc["license"])) if doc.get("license") is not None else None),
            "revision": (_reject_local(str(rev)) if rev is not None else None),
        })
    out.sort(key=lambda e: e["name"])
    return out


def _python_hashes(root: Path) -> dict:
    reqs = []
    f = root / "scripts" / "requirements.txt"
    for raw in (f.read_text(encoding="utf-8").splitlines() if f.is_file() else []):
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("-"):
            continue
        name = re.split(r"[<>=!~ \[]", line, 1)[0].strip()
        if not name:
            continue
        # version/hash come from constrained regexes (can't hold a path);
        # `name` is free text (e.g. a `-e ./pkg` egg name) -> guard it.
        vm = re.search(r"==\s*([0-9A-Za-z.\-]+)", line)
        hm = re.search(r"--hash=(sha256:[0-9a-f]{64})", line)
        reqs.append({"name": _reject_local(name), "version": vm.group(1) if vm else None,
                     "hash": hm.group(1) if hm else None})
    reqs.sort(key=lambda e: e["name"])
    return {"requirements": reqs}


# Every machine-read file type under metadata/ that a build can actually
# depend on: YAML/JSON data, TSV/CSV tables (pin/IO maps), C headers
# (library-profiles), a protobuf schema, the vendored e1m-spec lock, and
# board.yaml.example (parsed by tooling, not prose). `**/*.json`
# deliberately covers GENERATED artifacts too (metadata/catalog.json,
# metadata/error-catalog.json) -- a stale regenerated-but-uncommitted
# file is exactly the drift this lock exists to catch, same as any
# hand-written input. Deliberately OUTSIDE this tuple:
# `.md` (documentation) and `.gitkeep` (placeholder) -- neither can change
# what a build produces, so hashing them would only manufacture false drift
# on doc-only edits. `tests/scripts/test_alp_lock_metadata_coverage.py`
# asserts every tracked file under metadata/ is covered by one of these
# globs or is in its own small allowlist -- keep that test in sync with any
# addition here.
_METADATA_DIGEST_GLOBS = (
    "**/*.yaml", "**/*.json", "**/*.tsv", "**/*.csv", "**/*.h",
    "**/*.proto", "**/*.lock", "**/*.example", "**/*.tflite",
)


def _dir_digest(root: Path, rel: str, globs: str | tuple[str, ...]) -> str:
    d = root / rel
    h = hashlib.sha256()
    if isinstance(globs, str):
        globs = (globs,)
    # Gather into a set before sorting/hashing so a file matched by more
    # than one glob in `globs` is hashed exactly once. All nine suffixes in
    # `_METADATA_DIGEST_GLOBS` are disjoint today, so this never actually
    # fires there -- it's cheap insurance against a future overlapping
    # addition (e.g. a second glob that also matches `.json`), not a fix
    # for a live collision.
    matches = {p for glob in globs for p in d.glob(glob)} if d.is_dir() else set()
    # Order by the relative path's POSIX *parts*, never by the Path objects.
    # `sorted(Path)` compares pathlib's case-normalised form, which on Windows is
    # lower-cased and backslash-separated -- a different order than POSIX's, so
    # the same tree digested to a different sha on Windows than on CI.  A Windows
    # checkout thus false-reported drift, and re-locking there would have
    # committed a Windows-ordered digest that reds CI for everyone.
    # Key on `.parts` (not the joined string): pathlib orders component-wise, so
    # "a/b" sorts before "a-x/c" while a plain string compare flips them ('-' <
    # '/').  Parts therefore reproduce the existing POSIX order exactly -- the
    # committed digests stay valid and no re-lock is needed.
    for p in sorted(matches,
                    key=lambda q: PurePosixPath(q.relative_to(root).as_posix()).parts):
        h.update(p.relative_to(root).as_posix().encode())
        h.update(b"\0")
        h.update(hashlib.sha256(p.read_bytes()).hexdigest().encode())
        h.update(b"\n")
    return "sha256:" + h.hexdigest()


def _digests(root: Path) -> dict:
    return {
        # Narrower than `metadata`: kept as its own key so a schema-only
        # change points straight at `digests.schemas` instead of the wider
        # `digests.metadata` diff. `metadata/schemas/*.schema.json` is
        # therefore covered by BOTH digests below -- intentional, not a bug.
        "schemas": _dir_digest(root, "metadata/schemas", "*.schema.json"),
        "metadata": _dir_digest(root, "metadata", _METADATA_DIGEST_GLOBS),
    }


def build_lock(workspace_root: Path, board_yaml: Optional[Path] = None) -> dict:
    """Collect the workspace's reproducible inputs into an alp-lock-v1 dict."""
    root = Path(workspace_root)
    board = None
    # `resolution.groupsEnabled` is RESERVED in v1: no board-driven group
    # resolution is wired yet, so it is always emitted empty (like the
    # reserved `toolchain` object).  Populate from a real source when group
    # resolution lands; readers must not treat empty as "no groups".
    groups_enabled: list[str] = []
    if board_yaml is not None and Path(board_yaml).is_file():
        bdoc = yaml.safe_load(Path(board_yaml).read_text(encoding="utf-8")) or {}
        som = (bdoc.get("som") or {})
        raw_board = som.get("sku") if isinstance(som, dict) else None
        board = _reject_local(str(raw_board)) if raw_board is not None else None
    return {
        "lockVersion": 1,
        "generatedBy": "west alp-lock",
        "sdk": _sdk_identity(root),
        "west": _west_projects(root),
        "libraries": _libraries(root),
        "python": _python_hashes(root),
        "digests": _digests(root),
        "resolution": {"board": board, "groupsEnabled": sorted(groups_enabled)},
    }


from dataclasses import dataclass


@dataclass
class Drift:
    path: str
    locked: Any
    actual: Any


def _flatten(prefix: str, node: Any, out: dict) -> None:
    """Flatten a lock dict to {json-ish path: leaf}, keying list items that
    have a `name` by that name so drift paths are stable + human-readable.

    Name-keying is used ONLY when every item has a distinct `name`; on any
    duplicate or missing name the list falls back to index keys, so two
    same-named items can't collide into one path (which would silently mask
    drift on the shadowed item)."""
    if isinstance(node, dict):
        for k, v in node.items():
            _flatten(f"{prefix}.{k}" if prefix else k, v, out)
    elif isinstance(node, list):
        names = [v.get("name") if isinstance(v, dict) else None for v in node]
        by_name = all(n is not None for n in names) and len(set(names)) == len(names)
        for i, v in enumerate(node):
            key = str(names[i]) if by_name else str(i)
            _flatten(f"{prefix}[{key}]", v, out)
    else:
        out[prefix] = node


# Keys a lock may CARRY but that are never frozen-verified: a moved value is
# not dependency drift.
#
# `sdk.revision` is the only member, and it is no longer EMITTED (#1615) -- it
# is listed here so a lock generated before that change still verifies clean
# against a build that omits it.
#
# It recorded the git HEAD of the repo the lock was generated in, which is
# self-referential by construction: committing the lock advances HEAD past the
# value baked into it, so the field was stale the instant it landed.  Under
# squash-merge it was worse than stale -- it named the pre-squash tip of a
# feature branch, a commit the squash discarded, so the value shipped on `dev`
# was not on `dev`'s history at all.
#
# Its cost was paid by every concurrent branch: because it changed on every
# commit, `alp.lock` was rewritten by essentially every merge, so any open PR
# conflicted on it the moment an unrelated one landed -- on a file whose two
# sides never actually disagreed.  It also churned the SBOM serial (a hash over
# the whole lock) for builds that were otherwise byte-identical.
#
# There was no version of this that could have worked: any git HEAD written
# into a file that is then committed is stale by construction.  The genuine
# "which SDK commit produced this artifact" need is served by
# `scripts/build_receipt.py` (`source.sdkRevision` / `source.sdkDirty`), which
# resolves it against a real build instead of baking it into a tracked file.
#
# `sdk.version` plus the west project pins lock the SDK identity a consumer
# actually builds against; that is what reproduction needs and it is unaffected.
_PROVENANCE_KEYS = frozenset({"sdk.revision"})


def verify_lock(committed: dict, workspace_root: Path,
                board_yaml: Optional[Path] = None) -> list["Drift"]:
    """Recompute the lock from the live workspace and return field-level drift
    (empty == match).  A pre-#1615 lock may still carry `sdk.revision`; that key
    is never reported as drift (`_PROVENANCE_KEYS`).  Never writes."""
    actual = build_lock(workspace_root, board_yaml)
    a, b = {}, {}
    _flatten("", committed, a)
    _flatten("", actual, b)
    drifts: list[Drift] = []
    for key in sorted(set(a) | set(b)):
        if key in _PROVENANCE_KEYS:
            continue
        if a.get(key) != b.get(key):
            drifts.append(Drift(key, a.get(key), b.get(key)))
    return drifts

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure builders for `alp.lock` (epic #610 WS6-a).

`build_lock` collects the reproducible, public-safe dependency + toolchain
inputs of an Alp SDK workspace into a schema-validated dict; `verify_lock`
recomputes and diffs.  No IO beyond reads.
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Optional

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


def _default_rev(root: Path) -> Optional[str]:
    try:
        out = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip() or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def _sdk_identity(root: Path, rev_resolver: Callable[[Path], Optional[str]]) -> dict:
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
    return {"version": _reject_local(m.group(1)), "revision": rev_resolver(root)}


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


def _dir_digest(root: Path, rel: str, glob: str) -> str:
    d = root / rel
    h = hashlib.sha256()
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
    for p in sorted(d.glob(glob),
                    key=lambda q: PurePosixPath(q.relative_to(root).as_posix()).parts) \
            if d.is_dir() else []:
        h.update(p.relative_to(root).as_posix().encode())
        h.update(b"\0")
        h.update(hashlib.sha256(p.read_bytes()).hexdigest().encode())
        h.update(b"\n")
    return "sha256:" + h.hexdigest()


def _digests(root: Path) -> dict:
    return {
        "schemas": _dir_digest(root, "metadata/schemas", "*.schema.json"),
        "metadata": _dir_digest(root, "metadata", "**/*.yaml"),
    }


def build_lock(workspace_root: Path, board_yaml: Optional[Path] = None, *,
               rev_resolver: Callable[[Path], Optional[str]] = _default_rev) -> dict:
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
        "sdk": _sdk_identity(root, rev_resolver),
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


# Keys recorded for provenance/reproduction but NOT frozen-verified: a moved
# value is not dependency drift.  `sdk.revision` is self-referential when the
# lock lives in the repo whose HEAD it records -- committing the lock advances
# HEAD past the value baked into it, so a frozen check would fail on every
# subsequent commit.  It is kept in the lock (which SDK commit generated it)
# but excluded from the drift set.  `sdk.version` + the west pins still lock the
# SDK identity a consumer actually builds against.
_PROVENANCE_KEYS = frozenset({"sdk.revision"})


def verify_lock(committed: dict, workspace_root: Path,
                board_yaml: Optional[Path] = None, *,
                rev_resolver: Callable[[Path], Optional[str]] = _default_rev) -> list["Drift"]:
    """Recompute the lock from the live workspace and return field-level drift
    (empty == match).  Provenance-only keys (`_PROVENANCE_KEYS`, e.g.
    `sdk.revision`) are recorded but never reported as drift.  Never writes."""
    actual = build_lock(workspace_root, board_yaml, rev_resolver=rev_resolver)
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

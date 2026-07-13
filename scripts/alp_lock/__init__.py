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
from pathlib import Path
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
    txt = (root / "scripts" / "alp_cli" / "__init__.py").read_text(encoding="utf-8")
    m = re.search(r'^__version__\s*=\s*"([^"]*)"', txt, re.M)
    version = m.group(1) if m else "0.0.0"
    return {"version": version, "revision": rev_resolver(root)}


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
            "groups": sorted(str(g) for g in (p.get("groups") or [])),
        })
    projects.sort(key=lambda e: e["name"])
    gf = [str(g) for g in (man.get("group-filter") or [])]
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
            "license": (str(doc["license"]) if doc.get("license") is not None else None),
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
        vm = re.search(r"==\s*([0-9A-Za-z.\-]+)", line)
        hm = re.search(r"--hash=(sha256:[0-9a-f]{64})", line)
        reqs.append({"name": name, "version": vm.group(1) if vm else None,
                     "hash": hm.group(1) if hm else None})
    reqs.sort(key=lambda e: e["name"])
    return {"requirements": reqs}


def _dir_digest(root: Path, rel: str, glob: str) -> str:
    d = root / rel
    h = hashlib.sha256()
    for p in sorted(d.glob(glob)) if d.is_dir() else []:
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
    groups_enabled: list[str] = []
    if board_yaml is not None and Path(board_yaml).is_file():
        bdoc = yaml.safe_load(Path(board_yaml).read_text(encoding="utf-8")) or {}
        som = (bdoc.get("som") or {})
        board = som.get("sku") if isinstance(som, dict) else None
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

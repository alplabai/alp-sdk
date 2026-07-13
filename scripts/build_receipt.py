#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compose a deterministic build-receipt-v1 from existing build inputs (#610 §7).

Pure: hashes the given board.yaml / build-plan / images, reads sku + boardYaml
from the build-plan, resolves the SDK git revision, and validates the result
against metadata/schemas/build-receipt-v1.schema.json before returning. No
wall-clock field -- identical inputs yield an identical receipt.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path
from typing import Callable, Optional

import jsonschema

SCHEMA_VERSION = 1
_SCHEMA = Path(__file__).resolve().parent.parent / "metadata/schemas/build-receipt-v1.schema.json"


class MissingInputError(Exception):
    """A required build input (build-plan / image) does not exist."""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return f"sha256:{h.hexdigest()}"


def _sha256_text(text: str) -> str:
    return f"sha256:{hashlib.sha256(text.encode('utf-8')).hexdigest()}"


def digest_json(obj: dict) -> str:
    return _sha256_text(json.dumps(obj, sort_keys=True, separators=(",", ":")))


def _relpath(path: Path, root: Path) -> str:
    """Repo-relative posix path (deterministic across checkouts); bare name if
    the file lives outside the repo root."""
    p, r = Path(path).resolve(), Path(root).resolve()
    try:
        return p.relative_to(r).as_posix()
    except ValueError:
        return p.name


def _git_rev(root: Path) -> tuple[Optional[str], bool]:
    try:
        rev = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None, False
    status = subprocess.run(["git", "-C", str(root), "status", "--porcelain"],
                            capture_output=True, text=True)
    if status.returncode != 0:
        raise RuntimeError(f"git status failed in {root}: {status.stderr.strip()}")
    return (rev or None), bool(status.stdout.strip())


def build_receipt(root: Path, build_plan_path: Path, images: list,
                  board_yaml_path: Path, lock_path: Optional[Path] = None,
                  rev_resolver: Callable[[Path], tuple] = _git_rev) -> dict:
    if not build_plan_path.is_file():
        raise MissingInputError(f"build-plan not found: {build_plan_path}")
    if not board_yaml_path.is_file():
        raise MissingInputError(f"board.yaml not found: {board_yaml_path}")
    bp = json.loads(build_plan_path.read_text(encoding="utf-8"))
    rev, dirty = rev_resolver(root)
    img_out = []
    for core, path in images:
        path = Path(path)
        if not path.is_file():
            raise MissingInputError(f"image not found: {path}")
        img_out.append({"core": core, "path": _relpath(path, root),
                        "sha256": sha256_file(path), "sizeBytes": path.stat().st_size})
    receipt = {
        "schemaVersion": SCHEMA_VERSION,
        "source": {"sdkRevision": rev, "sdkDirty": dirty},
        "config": {
            "boardYaml": _relpath(board_yaml_path, root),
            "boardYamlDigest": sha256_file(board_yaml_path),
            "sku": bp.get("sku"),
            "lockDigest": sha256_file(lock_path) if lock_path and Path(lock_path).is_file() else None,
            "buildPlanDigest": sha256_file(build_plan_path),
        },
        "toolchain": {"identity": bp.get("toolchain"), "compiler": None, "flags": None},
        "images": img_out,
        "provenance": {"sbomRef": None, "attestationRef": None},
    }
    jsonschema.Draft202012Validator(
        json.loads(_SCHEMA.read_text(encoding="utf-8"))).validate(receipt)
    return receipt

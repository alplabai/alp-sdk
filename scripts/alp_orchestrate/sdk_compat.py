# SPDX-License-Identifier: Apache-2.0
"""The hw_rev <-> SDK-version compatibility gate `metadata/sdk_version.yaml`
advertises.

That file has stated since it was written that `scripts/alp_project.py`
"refuses to emit when the requested hw_rev is outside
[min_sdk_version, max_sdk_version]", and that
`scripts/validate_board_yaml.py` runs "the same check, exit code 3 on
mismatch".  Neither existed: a grep for `min_sdk_version` across
`scripts/` returned nothing, so a customer whose upgraded SDK no longer
supported their board revision got a normal, successful emit and firmware
built against the wrong hardware assumptions (issue #1019).

This module is that check.  It is deliberately pure and data-only so the
comparison can be tested without a board.yaml, a metadata tree, or a
loader.

Scope, and what is deliberately NOT here: whether the requested revision
*exists* or is `status: reserved` is a different question with a different
failure mode, tracked separately (#1025).  This module answers one thing --
given a revision that resolved, does the running SDK version fall inside
the range that revision declares.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Optional

import yaml

# A revision may bound one side, both, or neither.  `max_sdk_version: ~`
# (YAML null) is the common case in tree today and means "no upper bound
# declared yet" -- every one of the 27 ranges currently shipped is
# open-ended on the high side.  An ABSENT bound is unbounded, never zero:
# reading it as a bound would refuse every `status: reserved` revision,
# which declares no range at all.
_VERSION_RE = re.compile(r"^\s*v?(\d+)\.(\d+)\.(\d+)")


def parse_version(value: Optional[str]) -> Optional[tuple[int, int, int]]:
    """Parse `X.Y.Z` (or `vX.Y.Z`) into a comparable tuple.

    Returns None for anything unparseable, including None itself.  A
    malformed bound is treated as absent rather than as a failure: this
    gate exists to catch a real hw_rev/SDK mismatch, and turning a typo in
    metadata into a refused build would be a worse failure than the one it
    prevents.  `check_metadata` in the validator is where malformed
    metadata belongs.
    """
    if not isinstance(value, str):
        return None
    m = _VERSION_RE.match(value)
    if m is None:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def incompatibility(
    sdk_version: Optional[str],
    minimum: Optional[str],
    maximum: Optional[str],
) -> Optional[str]:
    """Return a human reason the SDK is outside the range, or None if it fits.

    Both bounds are INCLUSIVE: a revision declaring `min_sdk_version: 0.3.0`
    is supported *by* 0.3.0, not merely after it.
    """
    running = parse_version(sdk_version)
    if running is None:
        # No readable version (a wheel with no adjacent metadata/ tree, the
        # same case `buildplan._sdk_version` guards).  Provenance is
        # best-effort there and a missing version is not evidence of a
        # mismatch, so this stays quiet rather than refusing.
        return None

    lo = parse_version(minimum)
    hi = parse_version(maximum)

    if lo is not None and running < lo:
        return (f"needs SDK >= {minimum}, but this SDK is "
                f"{sdk_version}")
    if hi is not None and running > hi:
        return (f"is supported up to SDK {maximum}, but this SDK is "
                f"{sdk_version}")
    return None


def read_sdk_version(metadata_root: Path) -> Optional[str]:
    """The `version:` field out of `metadata/sdk_version.yaml`.

    Same read-and-strip idiom as `buildplan._sdk_version`, taking the
    metadata root as an argument so a test (and `--metadata-root`) can
    point it somewhere else.
    """
    try:
        text = (metadata_root / "sdk_version.yaml").read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("version:"):
            return stripped.split(":", 1)[1].split("#", 1)[0].strip()
    return None


def _revision(table: Any, hw_rev: Optional[str]) -> dict[str, Any]:
    """The entry for `hw_rev` out of a `hw_revisions:` mapping, or {}."""
    if not hw_rev or not isinstance(table, dict):
        return {}
    entry = (table.get("hw_revisions") or {}).get(hw_rev)
    return entry if isinstance(entry, dict) else {}


def family_revision(metadata_root: Path,
                    family_dir: Optional[str],
                    hw_rev: Optional[str]) -> dict[str, Any]:
    """The SoM-family `hw_revisions:` entry for `hw_rev`, or {}.

    `family_dir` is the directory name (`aen`, `v2n`, `v2n-m1`, `imx93`),
    not the SoM preset's `family:` string (`alif-ensemble`, ...) -- the two
    differ and only the former names a path.
    """
    if not family_dir:
        return {}
    path = metadata_root / "e1m_modules" / family_dir / "hw-revisions.yaml"
    if not path.is_file():
        return {}
    try:
        return _revision(yaml.safe_load(path.read_text(encoding="utf-8")), hw_rev)
    except (OSError, yaml.YAMLError):
        return {}


def board_revision(board_preset: Any, hw_rev: Optional[str]) -> dict[str, Any]:
    """The board `hw_revisions:` entry for `hw_rev`, or {}."""
    return _revision(board_preset, hw_rev)


def check(
    sdk_version: Optional[str],
    *,
    som_revision: dict[str, Any],
    som_label: str,
    board_revision_entry: dict[str, Any],
    board_label: str,
) -> Optional[str]:
    """Both halves of the claim -- "the family / board hw_revisions tables".

    Returns a single assembled message naming every side that refuses, or
    None when the SDK sits inside every declared range.  The SoM is checked
    first because its range is the one a customer changes by swapping
    modules.
    """
    reasons: list[str] = []
    for entry, label in ((som_revision, som_label),
                         (board_revision_entry, board_label)):
        if not entry:
            continue
        why = incompatibility(sdk_version,
                              entry.get("min_sdk_version"),
                              entry.get("max_sdk_version"))
        if why:
            reasons.append(f"{label} {why}")
    if not reasons:
        return None
    return "; ".join(reasons)

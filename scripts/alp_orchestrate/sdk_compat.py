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

Also here (#1025's SAFE half only): `revision_known()`, the existence
predicate -- given a resolved `hw_revisions:` table, is the requested
hw_rev even a declared key.  A revision absent from the table used to
silently resolve to base-revision routing with a clean exit code; that is
a wrong-hardware emit, not a version mismatch, so it is a distinct check
with a distinct error (`SdkRevisionUnknown`, not `SdkRevisionUnsupported`)
run BEFORE the range comparison below -- an unknown revision has no bounds
to compare against.

Deliberately NOT here: whether an EXISTING revision is `status: reserved`
(or carries no `status` at all) and whether that should refuse a build.
That is a separate, still-open question -- the metadata itself disagrees
across families today (aen r3-r8 are `status: reserved`; v2n and v2n-m1
r2-r8 carry no `status` key; imx93 r1 is `status: tbd` and an in-tree
example builds against it) -- so `revision_known()` answers existence only
and stays blind to `status:` on purpose.
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


def _hw_revisions(table: Any) -> Optional[dict[str, Any]]:
    """The raw `hw_revisions:` mapping out of `table`, or None if absent.

    None -- not {} -- distinguishes "this table declares no `hw_revisions:`
    section at all" (an inline board has no per-board table; there is
    nothing to assert existence against) from "a table exists and the key
    simply isn't in it" (a genuine unknown-revision refusal).
    """
    if not isinstance(table, dict):
        return None
    revisions = table.get("hw_revisions")
    return revisions if isinstance(revisions, dict) else None


def revision_known(table: Any, hw_rev: Optional[str]) -> Optional[bool]:
    """Whether `hw_rev` is a declared key in `table`'s `hw_revisions:` map.

    Pure existence check (#1025's safe half) -- deliberately blind to
    `status:`.  A `status: reserved` revision, or one carrying no `status`
    key at all, is still "known" here; whether it is buildable is a
    different question this predicate does not answer.

    Returns None -- not False -- when `table` has no `hw_revisions:`
    mapping at all, so a caller can tell "nothing to check existence
    against" (e.g. an inline board with no per-board table -- skip) apart
    from "checked, and it's missing" (True/False -- refuse on False).
    """
    revisions = _hw_revisions(table)
    if revisions is None:
        return None
    return bool(hw_rev) and hw_rev in revisions


def _load_family_table(metadata_root: Path, family_dir: str) -> Any:
    """The raw parsed `hw-revisions.yaml` for a SoM family, or {} if
    missing/unreadable -- same tolerant-read idiom `family_revision` used
    inline before this was factored out for `family_revision_known` to
    share."""
    path = metadata_root / "e1m_modules" / family_dir / "hw-revisions.yaml"
    if not path.is_file():
        return {}
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError):
        return {}


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
    return _revision(_load_family_table(metadata_root, family_dir), hw_rev)


def family_revision_known(metadata_root: Path,
                          family_dir: Optional[str],
                          hw_rev: Optional[str]) -> Optional[bool]:
    """`revision_known()` against the SoM-family hw-revisions.yaml table."""
    if not family_dir:
        return None
    return revision_known(_load_family_table(metadata_root, family_dir), hw_rev)


def family_available_revisions(metadata_root: Path,
                               family_dir: Optional[str]) -> list[str]:
    """Sorted `hw_revisions:` keys for a SoM family, or [] if none/missing.

    For an error message naming what a customer's SDK actually knows about
    (`family_revision_known` returning False needs somewhere to point).
    """
    if not family_dir:
        return []
    revisions = _hw_revisions(_load_family_table(metadata_root, family_dir))
    return sorted(revisions.keys()) if revisions else []


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

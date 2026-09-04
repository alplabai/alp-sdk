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

Also here: `revision_buildable()`, the maintainer's broad-reading decision
on #1025's other half -- a revision that EXISTS but is `status: reserved`,
`status: tbd`, or carries no `status` key at all is refused, distinctly
from not existing at all (`SdkRevisionNotBuildable`, not
`SdkRevisionUnknown`).  `revision_known()` itself stays deliberately blind
to `status:` -- existence and buildability are two different questions,
answered by two different predicates, so a later status change can't slip
into the wrong one silently.

All three of those gates read ONE file per SoM family,
`metadata/e1m_modules/<family>/hw-revisions.yaml`, through
`load_family_table()` below.  That reader used to swallow `OSError` and
`yaml.YAMLError` and answer `{}`, which every predicate here reads as
"nothing to judge" -- so a single tab-indented line in that file disabled
the unknown-revision gate, the not-buildable gate AND the SDK-range gate
at once and emitted a wrong-hardware artefact at exit 0.  It now refuses
instead; see `load_family_table`'s own docstring for why an ABSENT table
is still benign while a present-but-unusable one is not.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Optional

import yaml

from .models import OrchestratorError

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


# Statuses a build is permitted to proceed against.  Everything else --
# `reserved`, `tbd`, and (the broad reading) a revision that carries no
# `status` key at all -- is refused.  The schema now requires `status` on
# every entry (#1025), so a missing key means a table hand-edited or
# written before that requirement, not a deliberate declaration.
_NOT_BUILDABLE_STATUSES = frozenset({"reserved", "tbd"})


def revision_buildable(table: Any, hw_rev: Optional[str]) -> Optional[bool]:
    """Whether `hw_rev`'s declared `status:` permits a build.

    Tri-state like `revision_known()`, and answers a DIFFERENT question:
    None when there is nothing to judge -- no `hw_revisions:` table at all,
    or `hw_rev` isn't a key in it (that is `revision_known()`'s failure to
    report, not this one's -- an unknown revision has no status to have
    read).  Otherwise False for `status: reserved`, `status: tbd`, or an
    entry with no `status` key; True for every other declared status.
    """
    revisions = _hw_revisions(table)
    if revisions is None:
        return None
    entry = revisions.get(hw_rev) if hw_rev else None
    if not isinstance(entry, dict):
        # A key present but not a dict (None, a bare string, ...) is a
        # malformed entry, not an absent one -- it has no status, so it
        # is not buildable.  Only an absent key stays None (unknown).
        return False if hw_rev in revisions else None
    status = entry.get("status")
    if not isinstance(status, str) or status in _NOT_BUILDABLE_STATUSES:
        return False
    return True


def _table_unreadable(path: Path, why: str) -> OrchestratorError:
    """The refusal every present-but-unusable family table raises.

    One message shape so the three gates that read the table cannot
    disagree about what an unreadable one means.
    """
    return OrchestratorError(
        f"{path}: the SoM-family hardware-revision table is present but "
        f"unusable ({why}).  This table is the ONLY thing the hw_rev "
        "existence, buildable-status and SDK-version-range gates check "
        "against, so an unusable one is refused rather than read as "
        "\"this family declares no revisions\" -- treating it as empty "
        "would let every one of those gates pass and emit firmware for "
        "an unverified hardware revision.  Restore the file (a partial "
        "checkout, a truncated write or a hand-edit that broke the YAML "
        "are the usual causes) and re-run.")


def load_family_table(metadata_root: Path, family_dir: str) -> Any:
    """The raw parsed `hw-revisions.yaml` for a SoM family, or {} when the
    family ships no table at all.

    Public, not `_`-private, because it has a SECOND consumer outside this
    module: `alp_project_loader._hwrev_pad_route_overrides()` carries its
    own copies of the #1025 existence and buildable gates for the
    `--emit composed-route-table` / `--emit carrier-netlist` path, which
    resolves its SoM data independently of `alp_orchestrate.loader`.  That
    reader had its own tolerant `yaml.safe_load(...) or {}` and fell open
    on exactly the shapes guarded below; both readers now share this one,
    so they cannot disagree about whether a damaged table is fatal or
    about what the refusal says.

    ABSENT and UNUSABLE are two different situations and only one of them
    is benign (#563).  A family directory with no `hw-revisions.yaml`
    legitimately has nothing to check against -- the tri-state predicates
    below answer None ("nothing to judge") and the gates skip, which is
    what an in-development family and the test-isolation fixture in
    `tests/scripts/_orchestrate_support.py` both rely on.  A table that
    EXISTS but cannot be read or parsed is the opposite: there IS
    something to check against and we failed to read it, so answering
    None would silently disable all three gates on one typo and produce
    a wrong-hardware artefact at exit 0.  That is a refusal.

    Raises `OrchestratorError` (not one of the `SdkRevision*` subclasses):
    those name a defect in the customer's `board.yaml`, and
    `scripts/validate_board_yaml.py` maps each to its own exit code.  An
    unreadable table is a defect in the SDK's own metadata tree, so it
    takes the generic refusal + exit 1 instead.
    """
    path = metadata_root / "e1m_modules" / family_dir / "hw-revisions.yaml"
    if not path.is_file():
        return {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise _table_unreadable(path, f"cannot be read: {exc}") from exc
    try:
        table = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        detail = str(exc)
        first = detail.splitlines()[0] if detail else type(exc).__name__
        raise _table_unreadable(path, f"is not valid YAML: {first}") from exc
    if not isinstance(table, dict):
        # An empty file parses to None and a stray scalar to a str/int;
        # both are "present but not a table", the same fail-open hazard
        # as a parse error.  `metadata/schemas/hw-revisions-v1.schema.json`
        # requires a mapping with `family`, `display_name` and
        # `hw_revisions`, so no legitimate in-tree file lands here.
        kind = "an empty file" if table is None else type(table).__name__
        raise _table_unreadable(
            path, f"does not parse to a mapping ({kind})")
    if not isinstance(table.get("hw_revisions"), dict):
        # A file truncated above its `hw_revisions:` block still parses
        # as valid YAML, so the parse guards above do not catch it --
        # and a family table with no `hw_revisions:` mapping disables
        # the same three gates just as completely.  The schema requires
        # the key, so this is only ever a damaged file.
        raise _table_unreadable(
            path, "declares no `hw_revisions:` mapping")
    return table


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
    return _revision(load_family_table(metadata_root, family_dir), hw_rev)


def family_revision_known(metadata_root: Path,
                          family_dir: Optional[str],
                          hw_rev: Optional[str]) -> Optional[bool]:
    """`revision_known()` against the SoM-family hw-revisions.yaml table."""
    if not family_dir:
        return None
    return revision_known(load_family_table(metadata_root, family_dir), hw_rev)


def family_revision_buildable(metadata_root: Path,
                              family_dir: Optional[str],
                              hw_rev: Optional[str]) -> Optional[bool]:
    """`revision_buildable()` against the SoM-family hw-revisions.yaml table."""
    if not family_dir:
        return None
    return revision_buildable(load_family_table(metadata_root, family_dir), hw_rev)


def family_available_revisions(metadata_root: Path,
                               family_dir: Optional[str]) -> list[str]:
    """Sorted `hw_revisions:` keys for a SoM family, or [] if none/missing.

    For an error message naming what a customer's SDK actually knows about
    (`family_revision_known` returning False needs somewhere to point).
    """
    if not family_dir:
        return []
    revisions = _hw_revisions(load_family_table(metadata_root, family_dir))
    return sorted(revisions.keys()) if revisions else []


def board_revision(board_preset: Any, hw_rev: Optional[str]) -> dict[str, Any]:
    """The board `hw_revisions:` entry for `hw_rev`, or {}."""
    return _revision(board_preset, hw_rev)


def assert_exclusion_still_not_buildable(
    metadata_root: Path,
    family_dir: str,
    hw_rev: str,
    *,
    gate: str,
) -> Optional[str]:
    """The shared #1025 ratchet every `rpmsg-imx93`-shaped exclusion cites.

    `check_build_plan.py`, `check_system_manifest.py`,
    `check_emit_snapshots.py`, `check_zephyr_conf_parity.py`, and
    `tier-a-library-ci.json`'s `excludedFamilies` all carve out imx93 for
    the same reason: its only `hw_rev` (r1) is `status: tbd`, so nothing
    can `--emit` against it.  Five hand-rolled `if status == "tbd"` copies
    would drift independently the day that stops being true; this is the
    ONE assertion every site re-runs instead, so a status flip turns every
    exclusion red with the same actionable message rather than letting
    some silently keep skipping forever (round-2 review of issue #1025).

    Mirrors `check_cmake_chip_list_parity.py`'s
    `CHIP_LIST_EXCLUDED_WITH_REASON` precedent: an allowlist entry must
    prove its own reason still holds, not just exist.

    Returns None when the revision is still genuinely not buildable (the
    exclusion is honest); otherwise an actionable message naming `gate`
    and telling the reader to remove the exclusion.
    """
    buildable = family_revision_buildable(metadata_root, family_dir, hw_rev)
    if buildable is False:
        return None
    return (
        f"{gate}: the {family_dir} hw_rev {hw_rev!r} exclusion is stale -- "
        f"family_revision_buildable() now returns {buildable!r}, not "
        f"False, so this revision is no longer refused by the hw_rev-"
        f"buildable gate.  Remove the exclusion and re-add the case(s) it "
        f"was carved out for."
    )


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

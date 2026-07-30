# SPDX-License-Identifier: Apache-2.0
"""
`alp_orchestrate.sdk_compat` -- the hw_rev <-> SDK-version gate that
`metadata/sdk_version.yaml` has always documented and nothing implemented
(#1019).

The file claimed `scripts/alp_project.py` "refuses to emit when the
requested hw_rev is outside [min_sdk_version, max_sdk_version]" and that
`scripts/validate_board_yaml.py` runs "the same check, exit code 3 on
mismatch".  A grep for `min_sdk_version` across `scripts/` returned
nothing, so an upgraded SDK emitted normally for a revision it no longer
supported.

These tests pin BOTH halves: the comparison itself (pure, no metadata
tree) and the refusal reaching a caller through the loader.  The
end-to-end cases mutate a COPY of `metadata/` rather than the real tree --
every range shipped today is open-ended on the high side, so nothing in
tree can exercise the upper bound without one.

Run locally:

    python -m pytest tests/scripts/test_sdk_revision_gate.py -v
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

# `alp_orchestrate` is imported IN-BODY throughout this file, never at module
# scope. The package is deleted in a later slice, and a module-scope import
# would take the whole file's COLLECTION with it -- including
# `test_every_shipped_revision_range_admits_the_shipped_sdk_version`, the one
# test here with no counterpart in tan and the one that has to outlive the
# planner. The other ten are duplicated name-for-name in tan's
# `python/tests/core/test_sdk_revision_gate.py` and leave with it.


# A real example that names a SoM hw_rev, so the family table is consulted.
EXAMPLE = REPO / "examples" / "peripheral-io" / "gpio-button-led" / "board.yaml"


# --------------------------------------------------------------------------
# The comparison itself
# --------------------------------------------------------------------------

@pytest.mark.parametrize("sdk,lo,hi", [
    ("0.13.0", "0.3.0", None),      # every range in tree today
    ("0.13.0", None, None),         # `status: reserved` revs declare neither
    ("0.3.0", "0.3.0", None),       # lower bound is INCLUSIVE
    ("0.5.0", "0.3.0", "0.5.0"),    # upper bound is INCLUSIVE
    ("0.13.0", "v0.3.0", None),     # a `v` prefix parses
])
def test_versions_inside_the_declared_range_are_allowed(sdk, lo, hi):
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    assert sc.incompatibility(sdk, lo, hi) is None


def test_below_the_lower_bound_refuses_and_names_both_versions():
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    why = sc.incompatibility("0.2.0", "0.3.0", None)
    assert why is not None
    assert "0.3.0" in why and "0.2.0" in why


def test_above_the_upper_bound_refuses_and_names_both_versions():
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    why = sc.incompatibility("0.13.0", "0.3.0", "0.5.0")
    assert why is not None
    assert "0.5.0" in why and "0.13.0" in why


def test_the_comparison_is_numeric_not_lexicographic():
    """`0.13.0` must read as ABOVE `0.5.0`, not below it.

    String comparison puts "0.13.0" < "0.5.0" and would have silently
    allowed the exact upgrade this gate exists to catch.
    """
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    assert sc.incompatibility("0.13.0", None, "0.5.0") is not None
    assert sc.incompatibility("0.5.0", None, "0.13.0") is None


def test_an_unreadable_sdk_version_stays_quiet():
    """No version is not evidence of a mismatch.

    Mirrors `buildplan._sdk_version`, which returns None when there is no
    adjacent `metadata/` tree (a packaged wheel) rather than failing.
    """
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    assert sc.incompatibility(None, "0.3.0", "0.5.0") is None


def test_a_malformed_bound_is_treated_as_absent_not_as_a_refusal():
    """A typo in metadata must not become a refused build.

    Turning malformed metadata into a hard stop would be a worse failure
    than the one this gate prevents; malformed metadata is
    `validate_metadata`'s job.
    """
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    assert sc.incompatibility("0.13.0", "garbage", None) is None
    assert sc.incompatibility("0.13.0", None, "not-a-version") is None


def test_an_absent_range_is_unbounded_not_zero():
    """`reserved` revisions r2-r8 declare no range at all.

    Reading an absent bound as a bound would refuse every one of them.
    """
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    assert sc.incompatibility("0.13.0", None, None) is None
    assert sc.check("0.13.0",
                    som_revision={},
                    som_label="som",
                    board_revision_entry={},
                    board_label="board") is None


def test_both_sides_are_reported_when_both_refuse():
    from alp_orchestrate import sdk_compat as sc  # noqa: PLC0415
    why = sc.check(
        "0.13.0",
        som_revision={"max_sdk_version": "0.5.0"},
        som_label="SoM E1M-AEN801 hw_rev r2",
        board_revision_entry={"min_sdk_version": "9.0.0"},
        board_label="board e1m-evk hw_rev r1",
    )
    assert why is not None
    assert "E1M-AEN801" in why and "e1m-evk" in why


# --------------------------------------------------------------------------
# The refusal reaching a caller
# --------------------------------------------------------------------------

def _metadata_copy(tmp_path: Path) -> Path:
    dest = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", dest)
    return dest


def _cap_family_revision(meta: Path, family: str, rev: str, ceiling: str) -> None:
    path = meta / "e1m_modules" / family / "hw-revisions.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    doc["hw_revisions"][rev]["max_sdk_version"] = ceiling
    path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")


def test_every_shipped_revision_range_admits_the_shipped_sdk_version():
    """The gate must not refuse anything currently in tree.

    THE ONE TEST IN THIS FILE WITH NO COUNTERPART IN TAN, and the reason the
    file cannot simply be deleted when the planner leaves. tan's
    `python/tests/core/test_sdk_revision_gate.py` carries the other ten cases
    name for name, but its `test_the_shipped_tree_loads_cleanly` writes a
    SYNTHETIC board.yaml into `tmp_path`: it proves the mechanism, not that
    ALP-SDK'S OWN shipped ranges are satisfiable. That second question is
    about data in this repository, so it stays in this repository.

    Reads `metadata/**` directly instead of going through `load_board_yaml`,
    so the assertion outlives the planner: every `hw_revisions.<rev>` bound in
    every `metadata/e1m_modules/*/hw-revisions.yaml` must admit the version in
    `metadata/sdk_version.yaml`. Same fact, same failure, one fewer
    dependency. The comparison is inlined because `sdk_compat.check()` is
    itself the code being deleted.
    """
    from alp_orchestrate import load_board_yaml  # noqa: PLC0415
    sdk_version = yaml.safe_load(
        (REPO / "metadata" / "sdk_version.yaml").read_text(encoding="utf-8")
    )["version"]
    running = tuple(int(p) for p in str(sdk_version).lstrip("v").split("."))

    checked = 0
    for revisions_file in sorted(
            (REPO / "metadata" / "e1m_modules").glob("*/hw-revisions.yaml")):
        doc = yaml.safe_load(revisions_file.read_text(encoding="utf-8")) or {}
        for rev, body in (doc.get("hw_revisions") or {}).items():
            for bound in ("min_sdk_version", "max_sdk_version"):
                raw = (body or {}).get(bound)
                if raw in (None, ""):
                    continue
                limit = tuple(int(p) for p in str(raw).lstrip("v").split("."))
                where = f"{revisions_file.parent.name} {rev} {bound}={raw}"
                if bound == "min_sdk_version":
                    assert running >= limit, (
                        f"{where}: shipped SDK {sdk_version} is below the floor "
                        f"this revision declares -- the tree refuses itself")
                else:
                    assert running <= limit, (
                        f"{where}: shipped SDK {sdk_version} is above the ceiling "
                        f"this revision declares -- the tree refuses itself")
                checked += 1

    # An empty sweep would pass having proved nothing -- the same vacuous-green
    # shape this migration keeps finding behind globs.
    assert checked > 0, (
        "no hw_revisions bound found under "
        "metadata/e1m_modules/*/hw-revisions.yaml -- the sweep matched "
        "nothing, which is not the same as everything passing")


def test_an_out_of_range_som_revision_refuses_through_the_loader(tmp_path):
    from alp_orchestrate import SdkRevisionUnsupported, load_board_yaml  # noqa: PLC0415
    meta = _metadata_copy(tmp_path)
    _cap_family_revision(meta, "aen", "r2", "0.5.0")

    with pytest.raises(SdkRevisionUnsupported) as excinfo:
        load_board_yaml(EXAMPLE, metadata_root=meta)

    message = str(excinfo.value)
    assert "0.5.0" in message          # the ceiling that was exceeded
    assert "hw_rev r2" in message      # which revision
    assert "E1M-AEN801" in message     # which SoM


def test_the_refusal_is_an_orchestrator_error_subclass(tmp_path):
    """Existing `except OrchestratorError` handlers must keep catching it."""
    from alp_orchestrate import load_board_yaml  # noqa: PLC0415
    from alp_orchestrate import OrchestratorError

    meta = _metadata_copy(tmp_path)
    _cap_family_revision(meta, "aen", "r2", "0.5.0")

    with pytest.raises(OrchestratorError):
        load_board_yaml(EXAMPLE, metadata_root=meta)

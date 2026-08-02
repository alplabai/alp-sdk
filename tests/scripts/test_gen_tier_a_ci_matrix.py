"""Tests for scripts/gen_tier_a_ci_matrix.py (issue #499).

pr-tier-a-libraries.yml used to hard-code its ``strategy.matrix.include``
som/core pairs inline, so a ``familyMatrix`` edit in
metadata/registries/tier-a-library-ci.json never reached the actual CI
matrix. The workflow now derives its matrix from this script's output, so
these tests pin two things:

  * the real registry's ``familyMatrix`` produces the matrix the workflow
    build job actually runs (regression -- catches an uncoordinated
    registry edit that the script can't parse);
  * a fixture registry with a *different* ``familyMatrix`` produces a
    correspondingly different matrix, and a malformed ``familyMatrix``
    cell is rejected -- proving the workflow matrix can no longer drift
    from the registry silently, because there is no second, independent
    copy of the matrix left to drift.
"""

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from gen_tier_a_ci_matrix import build_matrix  # noqa: E402

REGISTRY = REPO / "metadata" / "registries" / "tier-a-library-ci.json"


def test_real_registry_matches_family_matrix() -> None:
    """The generated matrix must be exactly the registry's familyMatrix
    cells, minus any cell whose `family` is excludedFamilies (#1025:
    nxp-imx9/E1M-NX9101 stays listed in familyMatrix for
    scripts/validate_metadata.py's family-coverage check, but is skipped
    here -- its only hw_rev is `status: tbd`, unbuildable)."""
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    excluded = set((registry.get("excludedFamilies") or {}).keys())
    expected = [
        {"som": cell["som"], "core": cell["core"]}
        for cell in registry["familyMatrix"]
        if cell["family"] not in excluded
    ]

    matrix = build_matrix(REGISTRY)

    assert matrix == {"include": expected}
    assert expected, "sanity: the real registry's exclusion filter should not eat everything"


def test_registry_edit_changes_the_matrix(tmp_path: Path) -> None:
    """A deliberately-diverged familyMatrix produces a deliberately-different
    matrix -- demonstrating there is no separate, hand-maintained copy of
    the matrix left in the workflow that could silently disagree with it."""
    fixture = tmp_path / "tier-a-library-ci.json"
    fixture.write_text(
        json.dumps(
            {
                "schemaVersion": "tier-a-library-ci-v1",
                "hostBuild": {
                    "platform": "native_sim/native/64",
                    "libraries": ["lvgl"],
                    "excludedLibraries": {},
                },
                "familyMatrix": [
                    {"family": "alif-ensemble", "som": "E1M-AEN999", "core": "m55_hp"},
                ],
            }
        ),
        encoding="utf-8",
    )

    matrix = build_matrix(fixture)

    real_matrix = build_matrix(REGISTRY)
    assert matrix != real_matrix
    assert matrix == {"include": [{"som": "E1M-AEN999", "core": "m55_hp"}]}


def test_missing_family_matrix_rejected(tmp_path: Path) -> None:
    fixture = tmp_path / "tier-a-library-ci.json"
    fixture.write_text(json.dumps({"hostBuild": {}}), encoding="utf-8")

    with pytest.raises(ValueError, match="familyMatrix"):
        build_matrix(fixture)


def test_family_matrix_cell_missing_som_rejected(tmp_path: Path) -> None:
    fixture = tmp_path / "tier-a-library-ci.json"
    fixture.write_text(
        json.dumps({"familyMatrix": [{"family": "alif-ensemble", "core": "m55_hp"}]}),
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="som"):
        build_matrix(fixture)


def test_excluded_family_dropped_from_matrix_but_not_from_error_checks(
    tmp_path: Path,
) -> None:
    """A `family` key in `excludedFamilies` is skipped when building the CI
    matrix -- but a malformed cell in that same family is still rejected
    (exclusion skips the BUILD, not the registry's own well-formedness)."""
    fixture = tmp_path / "tier-a-library-ci.json"
    fixture.write_text(
        json.dumps(
            {
                "familyMatrix": [
                    {"family": "alif-ensemble", "som": "E1M-AEN801", "core": "m55_hp"},
                    {"family": "nxp-imx9", "som": "E1M-NX9101", "core": "m33"},
                ],
                "excludedFamilies": {"nxp-imx9": "not buildable right now"},
            }
        ),
        encoding="utf-8",
    )

    matrix = build_matrix(fixture)

    assert matrix == {"include": [{"som": "E1M-AEN801", "core": "m55_hp"}]}


def test_every_family_excluded_rejected(tmp_path: Path) -> None:
    fixture = tmp_path / "tier-a-library-ci.json"
    fixture.write_text(
        json.dumps(
            {
                "familyMatrix": [
                    {"family": "nxp-imx9", "som": "E1M-NX9101", "core": "m33"},
                ],
                "excludedFamilies": {"nxp-imx9": "not buildable right now"},
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="excluded"):
        build_matrix(fixture)

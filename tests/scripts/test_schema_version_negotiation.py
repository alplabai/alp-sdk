# SPDX-License-Identifier: Apache-2.0
"""Schema-version negotiation contract test (epic #610 WS6-b).

The board.yaml migration path (`scripts/alp_migrate/`) has never been
exercised end to end: the registry is empty and `LATEST` is 1, so a real
`schemaVersion` migration has never actually run. That leaves two easy
regressions invisible until the first v1->v2 migration ships: (1) the
camelCase `schemaVersion` key silently stops being read (e.g. someone
"fixes" it to `schema_version`) and every doc quietly falls back to the v1
floor, or (2) `plan()`/`apply_text()` stop refusing a doc newer than
`LATEST` and instead misbehave. This test pins both against a real fixture
file rather than an inline string, so it also stands in as a vendorable
example of a "future-versioned" board.yaml.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_migrate  # noqa: E402

FIXTURE = REPO / "tests/fixtures/board_schema_version_2.yaml"


def test_fixture_detected_as_schema_version_2():
    doc = alp_migrate.load(FIXTURE.read_text(encoding="utf-8"))
    assert alp_migrate.current_version(doc) == 2  # not the v1 default


def test_fixture_ahead_of_latest_refuses_to_plan():
    # LATEST is 1 today (empty registry); a v2 doc is ahead of it, so `plan()`
    # must raise rather than silently no-op or downgrade.
    assert alp_migrate.LATEST == 1
    doc = alp_migrate.load(FIXTURE.read_text(encoding="utf-8"))
    with pytest.raises(alp_migrate.MigrateError):
        alp_migrate.plan(doc)


def test_fixture_ahead_of_latest_refuses_to_apply():
    text = FIXTURE.read_text(encoding="utf-8")
    with pytest.raises(alp_migrate.MigrateError):
        alp_migrate.apply_text(text)

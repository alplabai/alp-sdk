# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_migrate  # noqa: E402


def test_current_version_absent_is_none():
    doc = alp_migrate.load("som:\n  sku: E1M-AEN801\n")
    assert alp_migrate.current_version(doc) is None


def test_current_version_explicit():
    doc = alp_migrate.load("schemaVersion: 1\nsom:\n  sku: X\n")
    assert alp_migrate.current_version(doc) == 1


def test_plan_absent_needs_adoption_step():
    doc = alp_migrate.load("som:\n  sku: X\n")
    assert alp_migrate.plan(doc) == [(None, 1)]


def test_plan_current_is_empty():
    doc = alp_migrate.load("schemaVersion: 1\nsom:\n  sku: X\n")
    assert alp_migrate.plan(doc) == []


def test_apply_stamps_and_preserves_comments():
    text = "# top comment\nsom:\n  sku: E1M-AEN801  # inline\n"
    doc = alp_migrate.load(text)
    new_doc, report = alp_migrate.apply(doc)
    out = alp_migrate.dump(new_doc)
    assert out.startswith("schemaVersion: 1")
    assert "# top comment" in out
    assert "# inline" in out
    assert "m000_to_v1" in report.steps[0]


def test_apply_is_idempotent():
    doc = alp_migrate.load("som:\n  sku: X\n")
    once, _ = alp_migrate.apply(doc)
    once_text = alp_migrate.dump(once)
    twice, report = alp_migrate.apply(alp_migrate.load(once_text))
    assert alp_migrate.dump(twice) == once_text
    assert report.steps == []


def test_downgrade_refused():
    doc = alp_migrate.load("schemaVersion: 99\nsom:\n  sku: X\n")
    with pytest.raises(alp_migrate.MigrateError):
        alp_migrate.plan(doc)


def test_report_to_diagnostics_shape():
    doc = alp_migrate.load("som:\n  sku: X\n")
    _, report = alp_migrate.apply(doc)
    d = alp_migrate.report_to_diagnostics(report, "file:///board.yaml")
    assert d["schemaVersion"] == 1
    assert d["tool"] == "alp-migrate"
    assert d["diagnostics"][0]["code"].startswith("alp.migrate.")


def test_apply_text_adds_exactly_one_line_on_real_board():
    path = REPO / "examples/peripheral-io/uart-hello-world/board.yaml"
    text = path.read_text(encoding="utf-8")
    new_text, report = alp_migrate.apply_text(text)
    old_lines = text.splitlines()
    new_lines = new_text.splitlines()
    added = [l for l in new_lines if l not in old_lines]
    removed = [l for l in old_lines if l not in new_lines]
    assert added == ["schemaVersion: 1"]
    assert removed == []          # nothing reformatted
    assert "schemaVersion: 1" in report.steps[0]


def test_apply_text_idempotent_real_board():
    path = REPO / "examples/peripheral-io/uart-hello-world/board.yaml"
    once, _ = alp_migrate.apply_text(path.read_text(encoding="utf-8"))
    twice, report = alp_migrate.apply_text(once)
    assert twice == once
    assert report.steps == []

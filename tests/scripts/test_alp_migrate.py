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
    stamped = path.read_text(encoding="utf-8")
    # The tracked file is itself canonical (schemaVersion: 1) post-migration
    # (#610 WS6-b migration #001 applied repo-wide). Strip the stamp so this
    # test still exercises apply_text against real comments/structure as if
    # unmigrated, and assert it round-trips back to the canonical file.
    text = stamped.replace("schemaVersion: 1\n", "", 1)
    new_text, report = alp_migrate.apply_text(text)
    old_lines = text.splitlines()
    new_lines = new_text.splitlines()
    added = [l for l in new_lines if l not in old_lines]
    removed = [l for l in old_lines if l not in new_lines]
    assert added == ["schemaVersion: 1"]
    assert removed == []          # nothing reformatted
    assert "schemaVersion: 1" in report.steps[0]
    assert new_text == stamped


def test_apply_text_idempotent_real_board():
    path = REPO / "examples/peripheral-io/uart-hello-world/board.yaml"
    once, _ = alp_migrate.apply_text(path.read_text(encoding="utf-8"))
    twice, report = alp_migrate.apply_text(once)
    assert twice == once
    assert report.steps == []


import importlib.util


def _load_cli():
    spec = importlib.util.spec_from_file_location(
        "alp_migrate_cli", REPO / "scripts/west_commands/alp_migrate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_cli_apply_stamps_file(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("# hi\nsom:\n  sku: X\n")
    rc = cli.main(["--apply", "--board", str(b), "--no-verify"])
    assert rc == 0
    out = b.read_text()
    # apply_text (Task 2's byte-faithful writer) preserves a leading whole-line
    # comment above the stamp, so schemaVersion lands right after it, not at
    # byte 0 -- this matches its comment-preserving contract (verified by
    # test_apply_text_adds_exactly_one_line_on_real_board above).
    assert "schemaVersion: 1" in out
    assert "# hi" in out


def test_cli_check_nonzero_on_drift(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("som:\n  sku: X\n")
    assert cli.main(["--check", "--board", str(b)]) == 1


def test_cli_requires_a_mode(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("som:\n  sku: X\n")
    with pytest.raises(SystemExit):        # argparse: a mode flag is required
        cli.main(["--board", str(b)])
    assert b.read_text() == "som:\n  sku: X\n"   # bare invocation never writes


def test_cli_migrate_error_is_clean(tmp_path, capsys):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("schemaVersion: 999\nsom:\n  sku: X\n")
    rc = cli.main(["--check", "--board", str(b)])
    assert rc == 1
    err = capsys.readouterr().err
    assert "alp-migrate:" in err            # clean message, not a traceback
    assert "Traceback" not in err

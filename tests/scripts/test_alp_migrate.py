# SPDX-License-Identifier: Apache-2.0
import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_migrate  # noqa: E402

REAL_BOARD = REPO / "examples/peripheral-io/uart-hello-world/board.yaml"


def _as_v1(text: str) -> str:
    """Drop the `schemaVersion:` stamp so a (now-migrated) repo board.yaml reads
    back as a v1 file for the machinery tests."""
    return "".join(l for l in text.splitlines(keepends=True)
                   if not l.lstrip().startswith("schemaVersion:"))


# ── version reads ───────────────────────────────────────────────────────────

def test_current_version_absent_is_one():
    doc = alp_migrate.load("som:\n  sku: E1M-AEN801\n")
    assert alp_migrate.current_version(doc) == 1  # lazy floor


def test_current_version_explicit():
    doc = alp_migrate.load("schemaVersion: 3\nsom:\n  sku: X\n")
    assert alp_migrate.current_version(doc) == 3


# ── planning ────────────────────────────────────────────────────────────────

def test_plan_absent_migrates_to_latest():
    doc = alp_migrate.load("som:\n  sku: X\n")  # absent == v1
    assert alp_migrate.plan(doc) == [(1, alp_migrate.LATEST)]


def test_plan_empty_when_explicit_latest():
    doc = alp_migrate.load(f"schemaVersion: {alp_migrate.LATEST}\nsom:\n  sku: X\n")
    assert alp_migrate.plan(doc) == []


def test_plan_downgrade_refused():
    doc = alp_migrate.load("schemaVersion: 99\nsom:\n  sku: X\n")
    with pytest.raises(alp_migrate.MigrateError):
        alp_migrate.plan(doc)


# ── apply at LATEST is a no-op ──────────────────────────────────────────────

def test_apply_text_noop_at_latest():
    text = REAL_BOARD.read_text(encoding="utf-8")  # migrated to LATEST
    new_text, report = alp_migrate.apply_text(text)
    assert new_text == text          # already current: nothing to do
    assert report.steps == []


# ── set_schema_version helper ───────────────────────────────────────────────

def test_set_schema_version_inserts_below_banner():
    lines = "# banner\nsom:\n  sku: X\n".splitlines(keepends=True)
    alp_migrate.set_schema_version(lines, 2)
    assert "".join(lines) == "# banner\nschemaVersion: 2\nsom:\n  sku: X\n"


def test_set_schema_version_updates_existing():
    lines = "schemaVersion: 1\nsom:\n  sku: X\n".splitlines(keepends=True)
    alp_migrate.set_schema_version(lines, 4)
    assert "".join(lines) == "schemaVersion: 4\nsom:\n  sku: X\n"


# ── engine machinery via a synthetic v1->v2 step ────────────────────────────

def _bump_to_v2(lines, report):
    alp_migrate.set_schema_version(lines, 2)
    report.steps.append("synthetic m001_to_v2")


@pytest.fixture
def synthetic_v2(monkeypatch):
    """Register a fake v1->v2 step so the engine's chaining + byte-faithful
    apply can be exercised in isolation from the real migration content."""
    monkeypatch.setattr(alp_migrate, "STEPS", [(1, 2, _bump_to_v2)])
    monkeypatch.setattr(alp_migrate, "LATEST", 2)


def test_synthetic_plan_chains_v1_to_v2(synthetic_v2):
    doc = alp_migrate.load("som:\n  sku: X\n")  # absent == v1
    assert alp_migrate.plan(doc) == [(1, 2)]


def test_synthetic_apply_is_byte_faithful_on_real_board(synthetic_v2):
    text = _as_v1(REAL_BOARD.read_text(encoding="utf-8"))
    new_text, report = alp_migrate.apply_text(text)
    old_lines, new_lines = text.splitlines(), new_text.splitlines()
    added = [l for l in new_lines if l not in old_lines]
    removed = [l for l in old_lines if l not in new_lines]
    assert added == ["schemaVersion: 2"]   # only the bump line
    assert removed == []                    # nothing reformatted
    assert "synthetic m001_to_v2" in report.steps[0]


def test_synthetic_apply_is_idempotent(synthetic_v2):
    text = _as_v1(REAL_BOARD.read_text(encoding="utf-8"))
    once, _ = alp_migrate.apply_text(text)
    twice, report = alp_migrate.apply_text(once)   # now at v2 == LATEST
    assert twice == once
    assert report.steps == []


# ── real m001_to_v2 migration ───────────────────────────────────────────────

def test_m001_migrates_per_core_libraries_to_unified_v2():
    text = (
        "# banner\n"
        "som:\n  sku: E1M-AEN801\n\n"
        "cores:\n"
        "  m55_hp:\n"
        "    app: ./src\n"
        "    libraries:\n"
        "      - cmsis_dsp\n"
        "      - nanopb\n"
        "    peripherals: []\n"
    )
    new_text, report = alp_migrate.apply_text(text)
    doc = alp_migrate.load(new_text)
    assert doc["schemaVersion"] == 2
    # tokens canonicalised via the alias table, scoped to the declaring core.
    assert doc["libraries"] == [
        {"name": "cmsis-dsp", "cores": ["m55_hp"]},
        {"name": "nanopb", "cores": ["m55_hp"]},
    ]
    assert "libraries" not in doc["cores"]["m55_hp"]  # moved out of the core
    assert report.steps


def test_m001_migrates_project_wide_libraries_without_cores():
    text = ("som:\n  sku: E1M-AEN801\n\n"
            "libraries:\n  - coap\n\n"
            "cores:\n  m55_hp:\n    app: ./src\n")
    new_text, _ = alp_migrate.apply_text(text)
    doc = alp_migrate.load(new_text)
    assert doc["schemaVersion"] == 2
    assert doc["libraries"] == [{"name": "coap"}]  # cores omitted == project-wide


def test_m001_is_byte_faithful_outside_libraries():
    text = ("# keep me\nsom:\n  sku: X\n\n"
            "cores:\n  m55_hp:\n    app: ./src\n"
            "    libraries:\n      - nanopb\n    peripherals: []\n")
    new_text, _ = alp_migrate.apply_text(text)
    survivors = new_text.splitlines()
    for line in ["# keep me", "  sku: X", "    app: ./src", "    peripherals: []"]:
        assert line in survivors
    assert "      - nanopb" not in survivors  # the per-core block is removed


def test_m001_no_libraries_only_stamps_version():
    text = "som:\n  sku: X\ncores:\n  m55_hp:\n    app: ./src\n"
    new_text, _ = alp_migrate.apply_text(text)
    doc = alp_migrate.load(new_text)
    assert doc["schemaVersion"] == 2
    assert "libraries" not in doc


def test_m001_is_idempotent():
    text = ("som:\n  sku: X\ncores:\n  m55_hp:\n    app: ./src\n"
            "    libraries:\n      - nanopb\n")
    once, _ = alp_migrate.apply_text(text)
    twice, report = alp_migrate.apply_text(once)   # already v2 == LATEST
    assert twice == once
    assert report.steps == []


# ── diagnostic-v1 report shape ──────────────────────────────────────────────

def test_report_to_diagnostics_shape():
    report = alp_migrate.Report(steps=["did a thing"],
                                needs_manual=[("board.yaml:3", "check this")])
    d = alp_migrate.report_to_diagnostics(report, "file:///board.yaml")
    assert d["schemaVersion"] == 1
    assert d["tool"] == "alp-migrate"
    codes = [x["code"] for x in d["diagnostics"]]
    assert "alp.migrate.applied" in codes
    assert "alp.migrate.needs-manual" in codes
    assert d["diagnostics"][0]["uri"] == "file:///board.yaml"


# ── CLI (west alp-migrate) ──────────────────────────────────────────────────

def _load_cli():
    spec = importlib.util.spec_from_file_location(
        "alp_migrate_cli", REPO / "scripts/west_commands/alp_migrate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_cli_requires_a_mode(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("som:\n  sku: X\n")
    with pytest.raises(SystemExit):        # argparse: a mode flag is required
        cli.main(["--board", str(b)])
    assert b.read_text() == "som:\n  sku: X\n"   # bare invocation never writes


def test_cli_check_clean_when_current(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text(f"schemaVersion: {alp_migrate.LATEST}\nsom:\n  sku: X\n")
    assert cli.main(["--check", "--board", str(b)]) == 0


def test_cli_apply_is_noop_when_current(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    stamped = f"# banner\nschemaVersion: {alp_migrate.LATEST}\nsom:\n  sku: X\n"
    b.write_text(stamped)
    assert cli.main(["--apply", "--board", str(b), "--no-verify"]) == 0
    assert b.read_text() == stamped   # already current: no change


def test_cli_migrate_error_is_clean(tmp_path, capsys):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("schemaVersion: 999\nsom:\n  sku: X\n")
    rc = cli.main(["--check", "--board", str(b)])
    assert rc == 1
    err = capsys.readouterr().err
    assert "alp-migrate:" in err            # clean message, not a traceback
    assert "Traceback" not in err

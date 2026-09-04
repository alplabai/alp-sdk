# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/resolve_generated_conflicts.py.

The load-bearing behaviours: only GENERATED paths are touched, a non-generated
conflict blocks regeneration rather than being silently worked around, and
conflicted paths come from git's index -- never from grepping for markers.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "resolve_generated_conflicts.py"


def _load():
    spec = importlib.util.spec_from_file_location("rgc", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rgc = _load()


@pytest.mark.parametrize(
    "path",
    [
        "metadata/catalog.json",
        "docs/abi/v0.15-snapshot.json",
        "docs/abi/v0.16-snapshot.json",          # survives the next bump
        "tests/fixtures/emit-snapshots/coap-client-get.build-plan.snap",
        "tests/fixtures/emit-snapshots/rpmsg-aen.system-manifest.snap",
    ],
)
def test_generated_paths_are_recognised(path):
    pattern, factory = rgc.regenerator_for(path)
    assert factory is not None, f"{path} should be recognised as generated"
    assert pattern


@pytest.mark.parametrize(
    "path",
    [
        "src/yocto/inference_yocto.c",
        "docs/recommended-libraries.md",
        "VERSIONS.md",
        "CHANGELOG.md",                             # changelog.d/ handles this
        "metadata/boards/e1m-evk.yaml",
        "tests/fixtures/emit-snapshots/README.md",  # not a .snap
        # #1576: alp.lock is generated on demand and no longer committed, so
        # it can never appear in a merge conflict -- REGENERATORS has no
        # entry for it any more.
        "alp.lock",
    ],
)
def test_source_paths_are_not_touched(path):
    _, factory = rgc.regenerator_for(path)
    assert factory is None, f"{path} must NOT be auto-regenerated"


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True, check=True
    ).stdout


def _repo_with_conflict(tmp_path: Path) -> Path:
    repo = tmp_path / "r"
    repo.mkdir()
    _git(repo, "init", "-q", "-b", "main")
    _git(repo, "config", "user.email", "t@example.com")
    _git(repo, "config", "user.name", "t")

    # A file whose CONTENT looks like a conflict but never is -- alp-sdk's own
    # CHANGELOG.md documents a past incident by quoting marker lines, indented
    # so a repo-wide grep does not match them.
    (repo / "doc.md").write_text(
        "Describing a past incident:\n\n"
        "```\n  <<<<<<< HEAD\n  =======\n  >>>>>>> other\n```\n",
        encoding="utf-8",
    )
    (repo / "shared.txt").write_text("base\n", encoding="utf-8")
    (repo / "gen.json").write_text("base\n", encoding="utf-8")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")

    _git(repo, "checkout", "-q", "-b", "side")
    (repo / "shared.txt").write_text("side\n", encoding="utf-8")
    (repo / "gen.json").write_text("side\n", encoding="utf-8")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "side")

    _git(repo, "checkout", "-q", "main")
    (repo / "shared.txt").write_text("main\n", encoding="utf-8")
    (repo / "gen.json").write_text("main\n", encoding="utf-8")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "main")

    subprocess.run(["git", "merge", "side"], cwd=repo,
                   capture_output=True, text=True)  # conflicts, non-zero
    return repo


def test_conflicted_paths_reads_the_index_not_markers(tmp_path, monkeypatch):
    repo = _repo_with_conflict(tmp_path)
    monkeypatch.setattr(rgc, "REPO", repo)

    paths = rgc.conflicted_paths()

    assert sorted(paths) == ["gen.json", "shared.txt"], paths
    assert "doc.md" not in paths, "a file containing marker TEXT is not conflicted"


def test_refuses_to_regenerate_while_a_source_conflict_remains(
    tmp_path, monkeypatch, capsys
):
    """The whole point: metadata/catalog.json digests the tree, so
    regenerating while shared.txt still carries markers would hash them into
    a wrong value that LOOKS resolved."""
    repo = _repo_with_conflict(tmp_path)
    monkeypatch.setattr(rgc, "REPO", repo)
    monkeypatch.setattr(sys, "argv", ["rgc"])

    ran = []
    monkeypatch.setattr(rgc.subprocess, "run",
                        lambda *a, **k: ran.append(a) or (_ for _ in ()).throw(
                            AssertionError("no regenerator may run")))

    # conflicted_paths uses subprocess too, so stub it directly instead.
    monkeypatch.setattr(rgc, "conflicted_paths",
                        lambda: ["metadata/catalog.json", "shared.txt"])

    rc = rgc.main()

    assert rc == 1
    err = capsys.readouterr().err
    assert "refusing to regenerate" in err
    assert "shared.txt" in err
    assert ran == [], "nothing may be executed while a source conflict remains"


def test_dry_run_changes_nothing_and_flags_source_conflicts(
    monkeypatch, capsys
):
    monkeypatch.setattr(rgc, "conflicted_paths",
                        lambda: ["metadata/catalog.json", "src/x.c"])
    monkeypatch.setattr(sys, "argv", ["rgc", "--dry-run"])
    monkeypatch.setattr(rgc.subprocess, "run",
                        lambda *a, **k: (_ for _ in ()).throw(
                            AssertionError("dry-run must not execute anything")))

    rc = rgc.main()
    out = capsys.readouterr().out

    assert rc == 1
    assert "generated: metadata/catalog.json" in out
    assert "NOT generated (left alone): src/x.c" in out


def test_outside_a_merge_is_an_error_not_a_success(monkeypatch):
    monkeypatch.setattr(rgc, "conflicted_paths", lambda: [])
    monkeypatch.setattr(sys, "argv", ["rgc"])
    assert rgc.main() == 2


def test_abi_snapshot_command_tracks_the_current_minor(monkeypatch):
    """The snapshot filename carries the minor, so the argv must be derived
    from --print-current-version, never hardcoded."""
    calls = []

    class R:
        stdout = "v0.99\n"

    monkeypatch.setattr(rgc.subprocess, "run",
                        lambda argv, **kw: (calls.append(argv), R())[1])
    argv = rgc._abi_snapshot_cmd()

    assert "--print-current-version" in calls[0]
    assert "v0.99" in argv
    assert "docs/abi/v0.99-snapshot.json" in argv

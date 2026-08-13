# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""`scripts/verify_west_patches.py` must refuse an unpatched workspace (#1392).

`west patch apply` exits 0 on three no-op paths, so its status is not evidence
that a workspace carries the patches. These tests build real git repositories in
`tmp_path` -- a fake alp-sdk holding `zephyr/patches.yml` and a fake module
checkout -- and drive `verify()` against them, because the thing under test IS
the `git apply --reverse --check` behaviour and a mock of git would test the
mock.

`west list` is stubbed via the `--topdir` seam: `verify()` shells out to `west`,
so each test writes a `west` shim onto `PATH` that prints the fixture's own
project list. That keeps the module-name resolution under test (the
`module: alif` -> project `hal_alif` mismatch this repo actually has) instead of
assuming it.
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "verify_west_patches.py"

_spec = importlib.util.spec_from_file_location("verify_west_patches", SCRIPT)
vwp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vwp)


#: A patch that adds one line to `payload.txt`. Written as a real unified diff
#: so `git apply` -- the actual mechanism -- handles it, not a stand-in.
PATCH = """\
diff --git a/payload.txt b/payload.txt
--- a/payload.txt
+++ b/payload.txt
@@ -1,2 +1,3 @@
 first
+carried-by-the-patch
 last
"""

UNPATCHED = "first\nlast\n"
PATCHED = "first\ncarried-by-the-patch\nlast\n"
#: Neither the patched nor the unpatched text: the patch applies in neither
#: direction, which is the DRIFTED verdict.
DRIFTED_CONTENT = "first\nsomething-else-entirely\nlast\n"

_GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@e",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@e",
}


def _git(*args: str, cwd: Path) -> None:
    subprocess.run(["git", *args], cwd=cwd, check=True,
                   capture_output=True, env={**os.environ, **_GIT_ENV})


def _workspace(tmp_path: Path, module_content: str, *, module_name: str = "alif",
               declare_module_yml: bool = True,
               module_dirname: str = "alif") -> tuple[Path, Path]:
    """`(repo, topdir)` -- a fake alp-sdk plus a fake `hal_alif` checkout.

    `module_dirname` is the last path component of the checkout. It defaults to
    `alif`, matching the real workspace, where that basename ALSO happens to
    equal the `patches.yml` `module:` -- so a test that wants to exercise the
    `zephyr/module.yml` `name:` lookup specifically has to move it.
    """
    topdir = tmp_path / "ws"
    repo = topdir / "alp-sdk"
    (repo / "zephyr" / "patches" / "hal_alif").mkdir(parents=True)
    (repo / "zephyr" / "patches" / "hal_alif" / "0001.patch").write_text(PATCH)
    (repo / "zephyr" / "patches.yml").write_text(
        "patches:\n"
        "  - path: hal_alif/0001.patch\n"
        f"    module: {module_name}\n"
    )

    mod = topdir / "modules" / "hal" / module_dirname
    mod.mkdir(parents=True)
    (mod / "payload.txt").write_text(module_content)
    if declare_module_yml:
        (mod / "zephyr").mkdir()
        (mod / "zephyr" / "module.yml").write_text("name: alif\n")
    _git("init", "-q", cwd=mod)
    _git("add", "-A", cwd=mod)
    _git("commit", "-q", "-m", "fixture", cwd=mod)
    return repo, topdir


@pytest.fixture(autouse=True)
def _west_shim(tmp_path, monkeypatch):
    """A `west` on PATH that answers `list -f {name}|{abspath}` for the fixture.

    The west project name is `hal_alif` while `patches.yml` says `alif` -- the
    same mismatch as the real workspace. A resolver that keyed only on the west
    project name would pass every other test here and still fail in production.

    Written as a PYTHON script plus, on Windows, a `.bat` that invokes it. A
    `#!/usr/bin/env bash` file with the exec bit is not executable on Windows --
    `subprocess` there raises `FileNotFoundError: [WinError 2] The system cannot
    find the file specified`, which failed all 13 tests in this file on the
    `python-smoke (windows-latest)` leg while passing on ubuntu and macos.
    Skipping the module on Windows would have been the cheaper fix and the wrong
    one: `scripts/bootstrap.ps1` calls `verify_west_patches.py` on Windows, so
    this is code Windows users run.
    """
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    body = (
        "import os, sys\n"
        "if len(sys.argv) > 1 and sys.argv[1] == 'list':\n"
        "    root = os.path.join(os.getcwd(), 'modules', 'hal')\n"
        "    if os.path.isdir(root):\n"
        "        for name in sorted(os.listdir(root)):\n"
        "            print('hal_alif|' + os.path.join(root, name))\n"
        "    sys.exit(0)\n"
        "sys.exit(1)\n"
    )
    (bindir / "west.py").write_text(body, encoding="utf-8")
    if sys.platform.startswith("win"):
        # `west` with no extension is not spawnable on Windows; PATHEXT picks
        # up `.bat`.
        (bindir / "west.bat").write_text(
            f'@echo off\r\n"{sys.executable}" "%~dp0west.py" %*\r\n', encoding="utf-8"
        )
    else:
        shim = bindir / "west"
        shim.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
        shim.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}{os.pathsep}{os.environ['PATH']}")


def test_an_applied_patch_verifies(tmp_path):
    """Positive control: without it, a verifier that refuses everything passes."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert failures == []
    assert absent == []
    assert len(applied) == 1


def test_an_unapplied_patch_is_reported_absent(tmp_path):
    """The #1392 bug: `west patch apply` exits 0 having left this state."""
    repo, topdir = _workspace(tmp_path, UNPATCHED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert applied == []
    assert len(failures) == 1
    assert "ABSENT" in failures[0]
    assert "hal_alif/0001.patch" in failures[0]


def test_a_module_changed_on_top_of_the_patch_is_reported_drifted(tmp_path):
    """Applies in neither direction -- distinct from simply not being there."""
    repo, topdir = _workspace(tmp_path, DRIFTED_CONTENT)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert applied == []
    assert len(failures) == 1
    assert "DRIFTED" in failures[0]


def test_a_module_west_cannot_resolve_fails_rather_than_being_skipped(tmp_path):
    """west's own `if mod is None: continue` is the silent path being closed."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_name="no-such-module")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert applied == []
    assert len(failures) == 1
    assert "UNRESOLVED" in failures[0]


def test_module_name_resolves_through_module_yml(tmp_path):
    """`patches.yml` says `alif`; the west project is `hal_alif`.

    The checkout is moved to `modules/hal/alif_vendor` so the directory
    basename cannot resolve it either -- `zephyr/module.yml`'s `name: alif` is
    then the only thing that can, which is the lookup `west patch` itself uses.
    """
    repo, topdir = _workspace(tmp_path, PATCHED, module_dirname="alif_vendor")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert failures == []
    assert len(applied) == 1


def test_a_module_resolvable_by_none_of_the_three_names_refuses(tmp_path):
    """Same layout with `module.yml` removed: nothing maps `alif` to it."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_dirname="alif_vendor",
                              declare_module_yml=False)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert applied == []
    assert "UNRESOLVED" in failures[0]


def test_an_uncheckedout_module_is_separated_from_a_missing_patch(tmp_path):
    """Exit 3's input: nothing to have patched is not the same as unpatched."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    import shutil
    shutil.rmtree(topdir / "modules" / "hal" / "alif" / ".git")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert failures == []
    assert applied == []
    assert len(absent) == 1
    assert "NO-CHECKOUT" in absent[0]


def test_an_empty_patches_list_refuses_instead_of_passing_vacuously(tmp_path):
    """A verifier with nothing to verify must not report success."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    (repo / "zephyr" / "patches.yml").write_text("patches: []\n")
    with pytest.raises(RuntimeError, match="declares no patches"):
        vwp.verify(repo, topdir)


def test_main_refuses_when_no_workspace_resolves(tmp_path, monkeypatch, capsys):
    """`west topdir` failing is exit 2 -- a refusal, never a skip that reads green."""
    repo, _ = _workspace(tmp_path, PATCHED)
    bindir = tmp_path / "bin"
    (bindir / "west").write_text("#!/usr/bin/env bash\nexit 1\n")
    (bindir / "west").chmod(0o755)
    rc = vwp.main(["--repo", str(repo)])
    assert rc == 2
    assert "no west workspace resolvable" in capsys.readouterr().err


def test_main_exits_1_on_an_unapplied_patch(tmp_path):
    repo, topdir = _workspace(tmp_path, UNPATCHED)
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir)]) == 1


def test_main_exits_3_when_only_an_uncheckedout_module_blocked_the_check(tmp_path):
    repo, topdir = _workspace(tmp_path, PATCHED)
    import shutil
    shutil.rmtree(topdir / "modules" / "hal" / "alif" / ".git")
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir)]) == 3


def test_main_exits_0_on_a_fully_patched_workspace(tmp_path):
    repo, topdir = _workspace(tmp_path, PATCHED)
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir)]) == 0


def test_a_west_that_cannot_be_executed_is_a_refusal_not_a_verdict(tmp_path):
    """The #1426 regression: a bare `west` is not on PATH where bootstrap runs.

    `scripts/bootstrap.sh` installs west into the workspace venv and calls it by
    absolute path, so the default bare name raised `FileNotFoundError` out of
    every CI job that ran bootstrap -- and bootstrap reported that as
    "zephyr/patches.yml is not applied", which is a verdict this run never
    reached. It has to be exit 2 (could not inspect), with a message naming the
    executable.
    """
    repo, topdir = _workspace(tmp_path, PATCHED)
    with pytest.raises(RuntimeError, match="cannot execute"):
        vwp.verify(repo, topdir, west=str(tmp_path / "no-such-west"))


def test_main_exits_2_when_west_cannot_be_executed(tmp_path, capsys):
    repo, topdir = _workspace(tmp_path, PATCHED)
    rc = vwp.main(["--repo", str(repo), "--topdir", str(topdir),
                   "--west", str(tmp_path / "no-such-west")])
    assert rc == 2
    err = capsys.readouterr().err
    assert "cannot execute" in err
    # Never the "not applied" verdict -- nothing was inspected.
    assert "are not applied" not in err


#: Two patches to the SAME file, the shape `hal_alif/0001` + `0002` have in
#: this repo. `SECOND` applies on top of `FIRST`, so reversing `FIRST` alone
#: against the fully-patched tree fails on changed context.
OVERLAP_FIRST = """\
diff --git a/payload.txt b/payload.txt
--- a/payload.txt
+++ b/payload.txt
@@ -1,2 +1,3 @@
 first
+from-patch-one
 last
"""

OVERLAP_SECOND = """\
diff --git a/payload.txt b/payload.txt
--- a/payload.txt
+++ b/payload.txt
@@ -1,3 +1,4 @@
 first
 from-patch-one
+from-patch-two
 last
"""

BOTH_APPLIED = "first\nfrom-patch-one\nfrom-patch-two\nlast\n"


def _overlap_workspace(tmp_path: Path, module_content: str) -> tuple[Path, Path]:
    """A module carrying two patches that touch the same file."""
    topdir = tmp_path / "ws"
    repo = topdir / "alp-sdk"
    (repo / "zephyr" / "patches" / "hal_alif").mkdir(parents=True)
    (repo / "zephyr" / "patches" / "hal_alif" / "0001.patch").write_text(OVERLAP_FIRST)
    (repo / "zephyr" / "patches" / "hal_alif" / "0002.patch").write_text(OVERLAP_SECOND)
    (repo / "zephyr" / "patches.yml").write_text(
        "patches:\n"
        "  - path: hal_alif/0001.patch\n    module: alif\n"
        "  - path: hal_alif/0002.patch\n    module: alif\n"
    )
    mod = topdir / "modules" / "hal" / "alif"
    mod.mkdir(parents=True)
    (mod / "payload.txt").write_text(module_content)
    (mod / "zephyr").mkdir()
    (mod / "zephyr" / "module.yml").write_text("name: alif\n")
    _git("init", "-q", cwd=mod)
    _git("add", "-A", cwd=mod)
    _git("commit", "-q", "-m", "fixture", cwd=mod)
    return repo, topdir


def test_two_patches_touching_one_file_are_both_reported_applied(tmp_path):
    """The #1426 false DRIFTED.

    Per-patch `git apply --reverse --check` reports the FIRST of an overlapping
    pair as DRIFTED even though it is applied, because the second changed its
    context. Measured on a pristine `hal_alif` at `v2.3.0` with both patches
    forward-applied (rc=0 each): reversing `0001` alone gave rc=1. Every CI job
    that runs bootstrap failed on that.
    """
    repo, topdir = _overlap_workspace(tmp_path, BOTH_APPLIED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert failures == [], failures
    assert len(applied) == 2


def test_an_absent_patch_does_not_condemn_the_rest_of_its_stack(tmp_path):
    """Unwinding continues past a missing patch.

    A first cut marked everything below the failure DRIFTED, which turned one
    genuinely-missing patch into a whole module's worth of failures.
    """
    # Only the first patch applied: the second is simply not there.
    repo, topdir = _overlap_workspace(tmp_path, "first\nfrom-patch-one\nlast\n")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir)
    assert len(applied) == 1, applied
    assert len(failures) == 1, failures
    assert "0002.patch" in failures[0]
    assert "0001.patch" not in failures[0]

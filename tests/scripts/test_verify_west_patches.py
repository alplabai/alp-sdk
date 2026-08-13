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
    """
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    shim = bindir / "west"
    shim.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "list" ]; then\n'
        '  for d in "$PWD"/modules/hal/*; do echo "hal_alif|$d"; done\n'
        "  exit 0\n"
        "fi\n"
        "exit 1\n"
    )
    shim.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}{os.pathsep}{os.environ['PATH']}")


def test_an_applied_patch_verifies(tmp_path):
    """Positive control: without it, a verifier that refuses everything passes."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    failures, applied, absent = vwp.verify(repo, topdir)
    assert failures == []
    assert absent == []
    assert len(applied) == 1


def test_an_unapplied_patch_is_reported_absent(tmp_path):
    """The #1392 bug: `west patch apply` exits 0 having left this state."""
    repo, topdir = _workspace(tmp_path, UNPATCHED)
    failures, applied, absent = vwp.verify(repo, topdir)
    assert applied == []
    assert len(failures) == 1
    assert "ABSENT" in failures[0]
    assert "hal_alif/0001.patch" in failures[0]


def test_a_module_changed_on_top_of_the_patch_is_reported_drifted(tmp_path):
    """Applies in neither direction -- distinct from simply not being there."""
    repo, topdir = _workspace(tmp_path, DRIFTED_CONTENT)
    failures, applied, absent = vwp.verify(repo, topdir)
    assert applied == []
    assert len(failures) == 1
    assert "DRIFTED" in failures[0]


def test_a_module_west_cannot_resolve_fails_rather_than_being_skipped(tmp_path):
    """west's own `if mod is None: continue` is the silent path being closed."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_name="no-such-module")
    failures, applied, absent = vwp.verify(repo, topdir)
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
    failures, applied, absent = vwp.verify(repo, topdir)
    assert failures == []
    assert len(applied) == 1


def test_a_module_resolvable_by_none_of_the_three_names_refuses(tmp_path):
    """Same layout with `module.yml` removed: nothing maps `alif` to it."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_dirname="alif_vendor",
                              declare_module_yml=False)
    failures, applied, absent = vwp.verify(repo, topdir)
    assert applied == []
    assert "UNRESOLVED" in failures[0]


def test_an_uncheckedout_module_is_separated_from_a_missing_patch(tmp_path):
    """Exit 3's input: nothing to have patched is not the same as unpatched."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    import shutil
    shutil.rmtree(topdir / "modules" / "hal" / "alif" / ".git")
    failures, applied, absent = vwp.verify(repo, topdir)
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

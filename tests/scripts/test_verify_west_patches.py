# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""`scripts/verify_west_patches.py` must refuse an unpatched workspace (#1392).

`west patch apply` exits 0 on three no-op paths, so its status is not evidence
that a workspace carries the patches. These tests build real git repositories in
`tmp_path` -- a fake alp-sdk holding `zephyr/patches.yml` and a fake module
checkout -- and drive `verify()` against them, because the thing under test IS
the `git apply --reverse --check` behaviour and a mock of git would test the
mock.

`west list` is stubbed via the `west=`/`--west` seam: `verify()` shells out to
whatever executable it is given, so each test builds a `west` shim (the
`west_shim` fixture) that prints the fixture's own project list and passes its
path explicitly. That keeps the module-name resolution under test (the
`module: alif` -> project `hal_alif` mismatch this repo actually has) instead of
assuming it. The shim is passed by path rather than left on `PATH` for
`subprocess` to find by bare name -- see `west_shim`'s docstring for why that
distinction is itself the fix for a Windows-only defect this file used to
carry.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import stat
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


def _rmtree_clearing_readonly(path: Path) -> None:
    """`shutil.rmtree` that clears the read-only bit git leaves under `.git/objects/`.

    Git writes packed and loose objects read-only. `shutil.rmtree` deletes via
    `os.unlink`, which raises `PermissionError: [WinError 5] Access is denied`
    against a read-only file on Windows -- measured deleting a fixture's
    `.git`. POSIX doesn't hit this: there, deletion is governed by the
    containing directory's write permission, not the file's own mode bits, so
    the same fixture's `shutil.rmtree(... / ".git")` was never the problem
    there.

    `shutil.rmtree`'s `onexc` callback -- the replacement for the now
    deprecated `onerror` -- only exists from Python 3.12; this repo supports
    `>=3.10` (`pyproject.toml`), so this clears the bit up front with a plain
    walk instead of branching on `sys.version_info` for one callback signature
    vs. the other. One code path, correct on 3.10 through 3.12+.

    FILES ONLY, deliberately. Chmod'ing the directories too regresses POSIX,
    and did in this helper's first draft: `stat.S_IWRITE` is `0o200`, which as
    a directory mode is `d-w-------` -- no `r`, no `x`, so the directory
    becomes unlistable and untraversable. `os.walk` is topdown, so it reaches
    a directory BEFORE descending into it (measured on a `.git`-shaped tree:
    `objects` at index 0, `objects/ab` at index 3); its default `onerror=None`
    then swallows the resulting scandir failure in silence, so nothing deeper
    is ever chmod'ed; and `shutil.rmtree` -- which on POSIX walks via
    `_rmtree_safe_fd`, `os.open(name, O_RDONLY, dir_fd=...)` per subdirectory
    -- raises `PermissionError` on the first one. Net effect would be two
    ubuntu/macos tests turning red to fix a Windows-only problem.

    Files-only is right on both platforms: git's read-only bit is on the
    objects themselves; Windows' read-only ATTRIBUTE on a directory does not
    block deleting it (so WinError 5 is still cleared); and on POSIX a file's
    own mode does not govern unlinking it -- the containing directory's write
    permission does -- so the chmod really is inert there, which is precisely
    what the `dirs`-inclusive version was not.

    Both loops below tolerate a file vanishing underneath them. Git runs
    background maintenance (`gc.autoDetach`), which creates and then removes
    `.git/objects/maintenance.lock`; `os.walk` can enumerate that lock and the
    chmod land after git has already unlinked it, raising FileNotFoundError.
    Measured on macos-latest (2026-08-19): `FileNotFoundError: .../ws/modules/
    hal/alif/.git/objects/maintenance.lock` from this helper's chmod loop.
    `shutil.rmtree` has the same window on the unlink, so the ENOENT retry
    covers it too -- and only ENOENT, so a real PermissionError still surfaces.
    """
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                os.chmod(os.path.join(root, name), stat.S_IWRITE)
            except FileNotFoundError:
                continue
    try:
        shutil.rmtree(path)
    except FileNotFoundError:
        shutil.rmtree(path, ignore_errors=True)


def test_rmtree_helper_tolerates_a_file_vanishing_mid_walk(tmp_path, monkeypatch):
    """The helper must survive git's background maintenance deleting a lock.

    Regression guard for the macos-latest failure on 2026-08-19:
    `FileNotFoundError: .../.git/objects/maintenance.lock` raised from the chmod
    loop, because `os.walk` enumerated a lock file that `git maintenance` then
    unlinked before the chmod ran. Simulated deterministically here rather than
    raced: the first chmod is made to raise FileNotFoundError, exactly as the
    kernel would when the file is already gone.
    """
    victim = tmp_path / "gitdir"
    (victim / "objects").mkdir(parents=True)
    (victim / "objects" / "maintenance.lock").write_text("")
    (victim / "objects" / "keep").write_text("x")

    real_chmod = os.chmod
    calls = {"n": 0}

    def flaky_chmod(path, mode, *a, **kw):
        calls["n"] += 1
        if calls["n"] == 1:
            raise FileNotFoundError(2, "No such file or directory", str(path))
        return real_chmod(path, mode, *a, **kw)

    monkeypatch.setattr(os, "chmod", flaky_chmod)

    _rmtree_clearing_readonly(victim)

    assert calls["n"] >= 1, "the chmod loop did not run, so nothing was exercised"
    assert not victim.exists(), "the helper must still remove the tree"


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


@pytest.fixture
def west_shim(tmp_path, monkeypatch) -> str:
    """A directly-spawnable `west` command answering `list -f {name}|{abspath}`.

    Returns a path string; every call site passes it explicitly via
    `vwp.verify(..., west=west_shim)` or `["--west", west_shim]`. It is
    deliberately NOT put on PATH -- PATH is exactly the mechanism that hid the
    original defect (see below), so re-adding it here would re-hide it.

    The west project name is `hal_alif` while `patches.yml` says `alif` -- the
    same mismatch as the real workspace. A resolver that keyed only on the west
    project name would pass every other test here and still fail in production.

    MEASURED, not assumed -- this replaces three prior attempts that reasoned
    about PATH instead of running anything:

      `shutil.which("west")` DOES consult PATHEXT and finds a PATH-installed
      `west.bat`. `subprocess.run(["west", ...])`, by contrast, spawns via
      `CreateProcess`, which appends only `.exe` to a bare command and never
      consults PATHEXT at all -- so it never finds the `.bat`, no matter where
      on PATH it sits. `shutil.which` and `subprocess.run` disagree about the
      same bare name. (The docstring this replaces asserted the opposite --
      "PATHEXT picks up `.bat`" -- which is the false premise that misled
      those three attempts.)

      On this box, a real `west.exe` from `pip install west` sits on PATH
      further along than any shim directory could shadow via prepending, so a
      bare `["west", ...]` call silently runs THAT instead -- a real west with
      no workspace at the fixture's `topdir`, failing with "no west workspace
      found", not the shim's canned answer: a defect, not a crash, so it does
      not even look wrong until you diff the output. On a CI runner with no
      west installed, the identical bare-name lookup instead raises
      `FileNotFoundError: [WinError 2]`. One root cause, two symptoms, and
      neither one is the thing this fixture is trying to test.

      A SECOND, narrower Windows-only defect surfaced by actually running this
      after the PATH fix: a `.bat` handed to `subprocess.run` as a full path
      spawns fine for a plain arg list (measured: `["<west.bat>", "list"]` ->
      `rc: 0`), but `west_project_dirs()` calls with `-f "{name}|{abspath}"` --
      a literal, unescaped `|` in one argv element. A `.bat`/`.cmd` target is
      not a real Win32 executable, so Windows silently reroutes it through
      `cmd.exe`, which re-tokenizes the WHOLE reconstructed command line and
      treats that bare `|` as a shell pipe -- into a program literally named
      `{abspath}`, which does not exist. Measured effect: the `.bat` body never
      even runs (confirmed -- a debug marker file it would have written never
      appears), and the call fails with `'{abspath}' is not recognized...`.
      This is invisible to any check that only ever exercises `west list` with
      no `-f` argument, which is why the earlier "a full path IS spawnable"
      measurement missed it. A real `.exe` is immune, because `CreateProcess`
      launches it directly with no `cmd.exe` re-tokenization -- confirmed:
      `subprocess.run([sys.executable, "-c", "print(sys.argv[1:])", "list",
      "-f", "{name}|{abspath}"])` prints the pipe intact.

      So the Windows shim is `sys.executable` itself (a real `.exe`, no `.bat`
      involved) plus a `sitecustomize.py` reached via `PYTHONPATH`: `site`
      imports it during interpreter startup, before Python attempts to open
      `list` (its first argv element) as a script file, so the shim runs and
      calls `os._exit()` -- a hard exit that skips the rest of startup --
      before that open ever happens; no file named `list` need exist. Argv
      shape differs from the POSIX shim script here: Python treats its own
      first CLI argument as `sys.argv[0]` (the "script"), so this checks
      `sys.argv[0] == "list"`, not `sys.argv[1]` as the exec'd POSIX script
      below does (there, the script's OWN path is `argv[0]`).

      The non-`list` case deliberately does NOT exit. `sitecustomize` runs in
      EVERY interpreter started while this `PYTHONPATH` is set, so a bare
      `os._exit(1)` there would kill any unrelated Python child dead with rc=1
      and no output at all -- measured. It is also not needed: no test that
      uses this shim reaches `main()`'s `[west, "topdir"]` probe (they all pass
      `--topdir`), and replacing that line with `pass` was measured to leave
      all 16 tests passing. Falling through to normal start-up makes a stray
      Python child behave normally, and still leaves a non-zero exit for a
      genuine `west <something-else>` -- Python then fails to open that
      subcommand as a script, which is the right verdict anyway.

    Skipping this module on Windows is NOT an option, however tempting: it is
    `scripts/bootstrap.ps1` that invokes `verify_west_patches.py` (see its
    `--west` call sites), so this is code Windows users run. A skip would turn
    a real failure into a green run -- the exact defect class this file exists
    to catch.
    """
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    if sys.platform.startswith("win"):
        (bindir / "sitecustomize.py").write_text(
            "import os, sys\n"
            "if sys.argv and sys.argv[0] == 'list':\n"
            "    root = os.path.join(os.getcwd(), 'modules', 'hal')\n"
            "    if os.path.isdir(root):\n"
            "        for name in sorted(os.listdir(root)):\n"
            "            print('hal_alif|' + os.path.join(root, name))\n"
            "    sys.stdout.flush()\n"
            "    os._exit(0)\n",
            encoding="utf-8",
        )
        existing = os.environ.get("PYTHONPATH", "")
        monkeypatch.setenv(
            "PYTHONPATH", f"{bindir}{os.pathsep}{existing}" if existing else str(bindir)
        )
        return sys.executable
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
    script = bindir / "west"
    script.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
    script.chmod(0o755)
    return str(script)


def test_an_applied_patch_verifies(tmp_path, west_shim):
    """Positive control: without it, a verifier that refuses everything passes."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert failures == []
    assert absent == []
    assert len(applied) == 1


def test_an_unapplied_patch_is_reported_absent(tmp_path, west_shim):
    """The #1392 bug: `west patch apply` exits 0 having left this state."""
    repo, topdir = _workspace(tmp_path, UNPATCHED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert applied == []
    assert len(failures) == 1
    assert "ABSENT" in failures[0]
    assert "hal_alif/0001.patch" in failures[0]


def test_a_module_changed_on_top_of_the_patch_is_reported_drifted(tmp_path, west_shim):
    """Applies in neither direction -- distinct from simply not being there."""
    repo, topdir = _workspace(tmp_path, DRIFTED_CONTENT)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert applied == []
    assert len(failures) == 1
    assert "DRIFTED" in failures[0]


def test_a_module_west_cannot_resolve_fails_rather_than_being_skipped(tmp_path, west_shim):
    """west's own `if mod is None: continue` is the silent path being closed."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_name="no-such-module")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert applied == []
    assert len(failures) == 1
    assert "UNRESOLVED" in failures[0]


def test_module_name_resolves_through_module_yml(tmp_path, west_shim):
    """`patches.yml` says `alif`; the west project is `hal_alif`.

    The checkout is moved to `modules/hal/alif_vendor` so the directory
    basename cannot resolve it either -- `zephyr/module.yml`'s `name: alif` is
    then the only thing that can, which is the lookup `west patch` itself uses.
    """
    repo, topdir = _workspace(tmp_path, PATCHED, module_dirname="alif_vendor")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert failures == []
    assert len(applied) == 1


def test_a_module_resolvable_by_none_of_the_three_names_refuses(tmp_path, west_shim):
    """Same layout with `module.yml` removed: nothing maps `alif` to it."""
    repo, topdir = _workspace(tmp_path, PATCHED, module_dirname="alif_vendor",
                              declare_module_yml=False)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert applied == []
    assert "UNRESOLVED" in failures[0]


def test_an_uncheckedout_module_is_separated_from_a_missing_patch(tmp_path, west_shim):
    """Exit 3's input: nothing to have patched is not the same as unpatched."""
    repo, topdir = _workspace(tmp_path, PATCHED)
    _rmtree_clearing_readonly(topdir / "modules" / "hal" / "alif" / ".git")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
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


def test_main_refuses_when_no_workspace_resolves(tmp_path, capsys):
    """`west topdir` failing is exit 2 -- a refusal, never a skip that reads green.

    The always-fails `west` here must be genuinely SPAWNABLE on this platform,
    passed explicitly via `--west` -- not just present on disk and left for
    PATH to maybe find. A `#!/usr/bin/env bash` script with no `.bat`
    counterpart is inert on Windows, so a bare `["west", ...]` call there
    falls through to whatever real `west` (if any) happens to be on PATH: on
    this box, a real `west.exe` that itself also refuses (no workspace at
    `--repo`), which would pass this assertion for the wrong reason -- verified
    by running the old version of this test, which is exactly how it read
    green here while still failing on CI, where no `west` is installed at all
    and the same lookup raises `FileNotFoundError` before the assertion is
    even reached. Same `.bat`-on-Windows / exec-bit-on-POSIX technique as
    `west_shim`, built directly here rather than shared with it because
    "always fail" needs no Python interposer.
    """
    repo, _ = _workspace(tmp_path, PATCHED)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    if sys.platform.startswith("win"):
        west = bindir / "west.bat"
        west.write_text("@echo off\r\nexit /b 1\r\n", encoding="utf-8")
    else:
        west = bindir / "west"
        west.write_text("#!/usr/bin/env bash\nexit 1\n")
        west.chmod(0o755)
    rc = vwp.main(["--repo", str(repo), "--west", str(west)])
    assert rc == 2
    assert "no west workspace resolvable" in capsys.readouterr().err


def test_main_exits_1_on_an_unapplied_patch(tmp_path, west_shim):
    repo, topdir = _workspace(tmp_path, UNPATCHED)
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir),
                      "--west", west_shim]) == 1


def test_main_exits_3_when_only_an_uncheckedout_module_blocked_the_check(tmp_path, west_shim):
    repo, topdir = _workspace(tmp_path, PATCHED)
    _rmtree_clearing_readonly(topdir / "modules" / "hal" / "alif" / ".git")
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir),
                      "--west", west_shim]) == 3


def test_main_exits_0_on_a_fully_patched_workspace(tmp_path, west_shim):
    repo, topdir = _workspace(tmp_path, PATCHED)
    assert vwp.main(["--repo", str(repo), "--topdir", str(topdir),
                      "--west", west_shim]) == 0


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


def test_two_patches_touching_one_file_are_both_reported_applied(tmp_path, west_shim):
    """The #1426 false DRIFTED.

    Per-patch `git apply --reverse --check` reports the FIRST of an overlapping
    pair as DRIFTED even though it is applied, because the second changed its
    context. Measured on a pristine `hal_alif` at `v2.3.0` with both patches
    forward-applied (rc=0 each): reversing `0001` alone gave rc=1. Every CI job
    that runs bootstrap failed on that.
    """
    repo, topdir = _overlap_workspace(tmp_path, BOTH_APPLIED)
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert failures == [], failures
    assert len(applied) == 2


def test_an_absent_patch_does_not_condemn_the_rest_of_its_stack(tmp_path, west_shim):
    """Unwinding continues past a missing patch.

    A first cut marked everything below the failure DRIFTED, which turned one
    genuinely-missing patch into a whole module's worth of failures.
    """
    # Only the first patch applied: the second is simply not there.
    repo, topdir = _overlap_workspace(tmp_path, "first\nfrom-patch-one\nlast\n")
    failures, applied, absent, _unapplied = vwp.verify(repo, topdir, west=west_shim)
    assert len(applied) == 1, applied
    assert len(failures) == 1, failures
    assert "0002.patch" in failures[0]
    assert "0001.patch" not in failures[0]

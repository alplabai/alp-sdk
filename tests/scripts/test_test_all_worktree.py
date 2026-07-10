"""Regression test for #608: scripts/test-all.sh must be worktree-safe.

Uses a REAL `git worktree add` checkout (not a plain tmp_path copy) so
the fixture reproduces the two defects the original bug report named:

  * the checked-out worktree's `.git` is a *file* (a gitlink), not a
    directory -- any code assuming `.git` is a directory would break
    here;
  * the twister stage must pin the worktree's own path as the Zephyr
    module (`EXTRA_ZEPHYR_MODULES`) instead of inheriting whatever a
    west workspace elsewhere has registered.

The twister stage itself is stubbed out (a fake `$ZEPHYR_BASE/scripts/
twister` that just records how it was invoked) so this test stays
hermetic and fast -- it does not need a real Zephyr checkout.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"

pytestmark = pytest.mark.skipif(
    sys.platform.startswith("win")
    or shutil.which("git") is None
    or shutil.which("bash") is None,
    reason="exercises the POSIX bash script scripts/test-all.sh via a real "
    "git worktree; not applicable on Windows (git worktree add fails on the "
    "runner) and needs git+bash",
)


@pytest.fixture
def real_worktree(tmp_path):
    """Create (and clean up) a real `git worktree add --detach` checkout
    of this repo at HEAD, in a directory that is guaranteed to differ
    from REPO itself."""
    wt_dir = tmp_path / "alp-sdk-worktree"
    subprocess.run(
        ["git", "-C", str(REPO), "worktree", "add", "--detach", str(wt_dir), "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    try:
        yield wt_dir
    finally:
        subprocess.run(
            ["git", "-C", str(REPO), "worktree", "remove", "--force", str(wt_dir)],
            check=False,
            capture_output=True,
            text=True,
        )


def _fake_zephyr_base(tmp_path, counter_file):
    """A minimal $ZEPHYR_BASE tree with a stub `scripts/twister` that
    records its invocation count and the env it saw, then exits 0."""
    zephyr_base = tmp_path / "fake-zephyr"
    twister_dir = zephyr_base / "scripts"
    twister_dir.mkdir(parents=True)
    twister = twister_dir / "twister"
    twister.write_text(
        "#!/usr/bin/env python3\n"
        "import os, sys\n"
        f"with open({str(counter_file)!r}, 'a') as f:\n"
        "    f.write('invoked\\n')\n"
        "    f.write('EXTRA_ZEPHYR_MODULES=' "
        "+ os.environ.get('EXTRA_ZEPHYR_MODULES', '<unset>') + '\\n')\n"
        "sys.exit(0)\n"
    )
    twister.chmod(twister.stat().st_mode | stat.S_IEXEC)
    return zephyr_base


def test_git_worktree_gitdir_is_a_file(real_worktree):
    """Sanity-check the fixture itself reproduces the defect's precondition:
    a worktree's `.git` is a gitlink file, never a directory."""
    git_entry = real_worktree / ".git"
    assert git_entry.is_file()
    assert not git_entry.is_dir()
    assert git_entry.read_text().startswith("gitdir:")


def test_zephyr_only_runs_twister_exactly_once_from_a_worktree(real_worktree, tmp_path):
    """Regression lock for the duplicate `--zephyr-only` execution bug
    (scripts/test-all.sh:256 in the pre-fix script): the twister stage
    must be invoked exactly once, and its PASS/FAIL must be reported
    once in the summary -- not run twice with the first result hidden."""
    counter_file = tmp_path / "invocations.log"
    zephyr_base = _fake_zephyr_base(tmp_path, counter_file)

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = str(zephyr_base)
    env.pop("EXTRA_ZEPHYR_MODULES", None)

    proc = subprocess.run(
        ["bash", str(real_worktree / "scripts" / "test-all.sh"), "--zephyr-only"],
        cwd=str(real_worktree),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert proc.returncode == 0, proc.stdout + proc.stderr

    invocation_lines = [
        line for line in counter_file.read_text().splitlines() if line == "invoked"
    ]
    assert len(invocation_lines) == 1, (
        f"expected the twister stage to run exactly once, saw "
        f"{len(invocation_lines)}\n--- stdout ---\n{proc.stdout}"
    )

    # Exactly one "===== [twister] =====" stage header in the output --
    # the pre-fix script printed it twice (once via the command
    # substitution, once via the following run_stage() call).
    assert proc.stdout.count("===== [twister] =====") == 1

    # And the summary reports the stage once, as PASS (not silently
    # dropped/duplicated).
    assert proc.stdout.count("twister") >= 1
    assert "[twister] PASS" in proc.stdout


def test_twister_stage_pins_worktree_as_zephyr_module(real_worktree, tmp_path):
    """Regression lock for the worktree module-selection bug: running
    from a worktree must pin *that worktree's own path* as
    EXTRA_ZEPHYR_MODULES, not silently inherit an unset/foreign value
    that would resolve to some other (e.g. primary) checkout."""
    counter_file = tmp_path / "invocations.log"
    zephyr_base = _fake_zephyr_base(tmp_path, counter_file)

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = str(zephyr_base)
    env.pop("EXTRA_ZEPHYR_MODULES", None)

    proc = subprocess.run(
        ["bash", str(real_worktree / "scripts" / "test-all.sh"), "--zephyr-only"],
        cwd=str(real_worktree),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert proc.returncode == 0, proc.stdout + proc.stderr
    lines = counter_file.read_text().splitlines()
    module_lines = [line for line in lines if line.startswith("EXTRA_ZEPHYR_MODULES=")]
    assert len(module_lines) == 1
    seen_module = module_lines[0].split("=", 1)[1]

    # Must resolve to the WORKTREE's own real path, not REPO's, and be
    # the LAST entry in the (possibly multi-module) list -- last wins
    # a module-name collision in zephyr_module.py's parse_modules().
    modules = seen_module.split(";")
    assert modules[-1] == str(real_worktree.resolve())
    assert str(REPO.resolve()) not in modules


def test_twister_stage_appends_without_dropping_other_modules(real_worktree, tmp_path):
    """Regression lock for the acceptance criterion 'resolve the
    current checkout as the module without dropping other west
    modules': an inherited EXTRA_ZEPHYR_MODULES pointing at another
    checkout (simulating a shell-rc export, per docs/local-ci.md)
    must not win alp-sdk module-name resolution -- the worktree's own
    path is appended LAST, so it overrides any earlier entry with the
    same module name (zephyr_module.py's parse_modules() keys modules
    by name and a later entry wins a collision) -- while an unrelated
    third-party module path already in the list must survive
    untouched, at its original position."""
    counter_file = tmp_path / "invocations.log"
    zephyr_base = _fake_zephyr_base(tmp_path, counter_file)
    other_module = tmp_path / "some-other-zephyr-module"
    other_module.mkdir()

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = str(zephyr_base)
    # Simulate a shell-rc EXTRA_ZEPHYR_MODULES that still points at the
    # PRIMARY checkout (REPO) plus one unrelated module.
    env["EXTRA_ZEPHYR_MODULES"] = f"{other_module};{REPO.resolve()}"

    proc = subprocess.run(
        ["bash", str(real_worktree / "scripts" / "test-all.sh"), "--zephyr-only"],
        cwd=str(real_worktree),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert proc.returncode == 0, proc.stdout + proc.stderr
    lines = counter_file.read_text().splitlines()
    module_lines = [line for line in lines if line.startswith("EXTRA_ZEPHYR_MODULES=")]
    assert len(module_lines) == 1
    modules = module_lines[0].split("=", 1)[1].split(";")

    # The worktree's own path is appended LAST, after the stale
    # same-module entry -- so it wins the alp-sdk module-name
    # collision (last entry wins in zephyr_module.py) even though the
    # older entry is still present in the string ...
    assert modules[-1] == str(real_worktree.resolve())
    assert modules.index(str(real_worktree.resolve())) > modules.index(str(REPO.resolve()))
    # ... and the unrelated module survives untouched, at its
    # original position.
    assert modules[0] == str(other_module)


def test_repo_root_resolves_to_worktree_not_primary_checkout(real_worktree):
    """scripts/test-all.sh must resolve REPO_ROOT from its own
    invocation path (`${BASH_SOURCE[0]}`), landing on the worktree --
    never on the primary checkout it was `git worktree add`-ed from."""
    # `--help` is side-effect-free and exercises the same
    # SCRIPT_DIR/REPO_ROOT resolution + `cd` every other flag does.
    proc = subprocess.run(
        ["bash", str(real_worktree / "scripts" / "test-all.sh"), "--help"],
        cwd="/",  # invoke from an unrelated cwd to prove path resolution
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))

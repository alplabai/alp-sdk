# SPDX-License-Identifier: Apache-2.0
"""`scripts/ci/apt-bounded.sh` must bound the STEP, not each invocation.

A step calls the wrapper twice -- `update` then `install`. An earlier revision
gave each invocation its own budget, so a step could spend 2x the intended wall
clock and the step's own `timeout-minutes` fired first, killing the wrapper
before it could report an attributed failure. Measured on alp-sdk#1592
(2026-08-19, job 96013431161): `update` burned 3x300s, `install` then burned
2x300s, and the 20-minute step cap fired at 09:34:08 -- exactly the anonymous
outcome the wrapper exists to prevent.

The deadline is therefore computed once per step and persisted in
`RUNNER_TEMP`, keyed by `GITHUB_ACTION`. These tests pin the two properties
that matter, without needing a network or a real apt.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_WRAPPER = _REPO_ROOT / "scripts" / "ci" / "apt-bounded.sh"

def _why_the_wrapper_cannot_run_here() -> str:
    """Empty when this platform can execute the wrapper; else the reason.

    `python-smoke` runs this suite on three platforms and the wrapper is a
    Linux-CI artefact, so two of them could never have passed:

    - `macos-latest` HAS bash but ships no `timeout` -- it is `gtimeout`, from
      coreutils, not installed by default. Surfaced as rc=127, command not
      found, on exactly the two tests that reach the timeout call.
    - `windows-latest` failed all four with an empty stderr and no deadline
      file. NOT for want of tools: Git for Windows puts both `bash.EXE` and a
      real GNU `timeout.EXE` on PATH, so a `shutil.which` check finds them and
      would run these anyway. What is missing is POSIX semantics -- the PATH
      shims below are `#!/bin/sh` files made executable with `chmod`, and NTFS
      has no executable bit for that to set.

    So the platform test is deliberately BOTH: `os.name` for the semantics the
    shims need, and `shutil.which` for the two binaries, because a Linux runner
    that silently lost `timeout` should skip loudly rather than pass vacuously.
    Neither half is redundant.

    Nothing about the wrapper's real behaviour goes unchecked either way: it
    only ever runs on `ubuntu-latest`, which is where apt exists at all and
    where every workflow it guards runs.
    """
    if os.name != "posix":
        return (
            "not a POSIX platform -- the PATH shims are `#!/bin/sh` scripts and "
            "rely on an executable bit this filesystem does not have"
        )
    missing = [tool for tool in ("bash", "timeout") if shutil.which(tool) is None]
    if missing:
        return " + ".join(missing) + " not installed"
    return ""


_CANNOT_RUN = _why_the_wrapper_cannot_run_here()
needs_the_wrappers_own_tools = pytest.mark.skipif(
    bool(_CANNOT_RUN),
    reason=f"scripts/ci/apt-bounded.sh cannot run here: {_CANNOT_RUN}",
)


def _env(tmp_path: Path, *, step: str = "teststep") -> dict[str, str]:
    env = dict(os.environ)
    env["RUNNER_TEMP"] = str(tmp_path)
    env["GITHUB_ACTION"] = step
    env["APT_STEP_BUDGET"] = "60"
    env["APT_ATTEMPT_TIMEOUT"] = "5"
    env["APT_ATTEMPTS"] = "2"
    return env


def _fake_apt(tmp_path: Path, exit_code: int = 0) -> Path:
    """PATH shims for apt-get and sudo, so no network, no root, no password.

    The `sudo` shim matters: the wrapper prefixes `sudo` whenever it is not
    root and sudo exists, and a developer box with a password-protected sudo
    would otherwise fail at the prompt (rc=1) before ever reaching apt-get --
    masking what these tests assert. The shim just execs its arguments, which
    is what passwordless sudo does on the runners.
    """
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    apt = bindir / "apt-get"
    apt.write_text(f"#!/bin/sh\nexit {exit_code}\n", encoding="utf-8")
    apt.chmod(0o755)
    sudo = bindir / "sudo"
    sudo.write_text('#!/bin/sh\nexec "$@"\n', encoding="utf-8")
    sudo.chmod(0o755)
    return bindir


def test_the_wrapper_exists_and_is_executable() -> None:
    """Guard the guard: the tests below prove nothing if the path is wrong."""
    assert _WRAPPER.is_file(), f"{_WRAPPER} is missing"
    assert os.access(_WRAPPER, os.X_OK), f"{_WRAPPER} is not executable"


@needs_the_wrappers_own_tools
def test_a_step_whose_budget_is_spent_fails_loudly_rather_than_silently(
    tmp_path: Path,
) -> None:
    """The regression that mattered most: it must NEVER exit 0 having done nothing.

    When an earlier invocation in the same step consumed the budget, the retry
    counter is untouched, so `rc` is still 0. Exiting with it would report
    SUCCESS for an apt-get that never ran -- a silent failure strictly worse
    than the hang the wrapper bounds.
    """
    # Pre-seed a deadline that has already passed, as a first invocation would
    # leave behind after spending the whole budget.
    deadline = tmp_path / "apt-bounded.teststep.deadline"
    deadline.write_text(str(int(time.time()) - 1), encoding="utf-8")

    env = _env(tmp_path)
    env["PATH"] = f"{_fake_apt(tmp_path, exit_code=0)}:{env['PATH']}"

    proc = subprocess.run(
        ["bash", str(_WRAPPER), "install", "-y", "some-package"],
        env=env, capture_output=True, text=True, timeout=120,
    )
    assert proc.returncode != 0, (
        "budget exhausted but the wrapper exited 0 -- a step whose install never "
        f"ran would go GREEN. stderr:\n{proc.stderr}"
    )
    assert "step budget" in proc.stderr, (
        f"the failure must say why, so CI names it. stderr:\n{proc.stderr}"
    )


@needs_the_wrappers_own_tools
def test_the_deadline_is_shared_across_invocations_in_one_step(
    tmp_path: Path,
) -> None:
    """The second invocation must inherit the first one's deadline, not restart it."""
    env = _env(tmp_path)
    env["PATH"] = f"{_fake_apt(tmp_path, exit_code=0)}:{env['PATH']}"

    subprocess.run(["bash", str(_WRAPPER), "update"], env=env,
                   capture_output=True, text=True, timeout=120, check=False)
    written = list(tmp_path.glob("apt-bounded.*.deadline"))
    assert len(written) == 1, f"expected exactly one deadline file, got {written}"
    first = written[0].read_text(encoding="utf-8")

    subprocess.run(["bash", str(_WRAPPER), "install", "-y", "pkg"], env=env,
                   capture_output=True, text=True, timeout=120, check=False)
    assert written[0].read_text(encoding="utf-8") == first, (
        "the second invocation rewrote the deadline -- each call would get a "
        "full budget again, which is the #1592 overrun"
    )


@needs_the_wrappers_own_tools
def test_a_different_step_gets_its_own_budget(tmp_path: Path) -> None:
    """Scoping is per step: one step's spent budget must not starve the next."""
    env_a = _env(tmp_path, step="step-a")
    env_a["PATH"] = f"{_fake_apt(tmp_path, exit_code=0)}:{env_a['PATH']}"
    subprocess.run(["bash", str(_WRAPPER), "update"], env=env_a,
                   capture_output=True, text=True, timeout=120, check=False)

    env_b = _env(tmp_path, step="step-b")
    env_b["PATH"] = env_a["PATH"]
    proc = subprocess.run(["bash", str(_WRAPPER), "update"], env=env_b,
                          capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, (
        f"a fresh step must get a fresh budget. stderr:\n{proc.stderr}"
    )
    assert len(list(tmp_path.glob("apt-bounded.*.deadline"))) == 2


@needs_the_wrappers_own_tools
def test_a_real_apt_error_is_not_retried(tmp_path: Path) -> None:
    """Only timeout (124) and apt-transient (100) retry; a real error surfaces."""
    env = _env(tmp_path, step="step-err")
    env["PATH"] = f"{_fake_apt(tmp_path, exit_code=7)}:{env['PATH']}"
    proc = subprocess.run(["bash", str(_WRAPPER), "install", "-y", "pkg"],
                          env=env, capture_output=True, text=True, timeout=120)
    assert proc.returncode == 7, (
        f"a non-transient exit must pass through unchanged, got {proc.returncode}"
    )
    assert "not retrying" in proc.stderr


def _hanging_apt(tmp_path: Path) -> Path:
    """apt-get that never returns, so `timeout` kills it with rc=124."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    apt = bindir / "apt-get"
    apt.write_text("#!/bin/sh\nsleep 9999\n", encoding="utf-8")
    apt.chmod(0o755)
    sudo = bindir / "sudo"
    sudo.write_text('#!/bin/sh\nexec "$@"\n', encoding="utf-8")
    sudo.chmod(0o755)
    return bindir


def _run_hanging(tmp_path: Path, *, budget: str, slice_s: str, step: str):
    env = dict(os.environ)
    env["RUNNER_TEMP"] = str(tmp_path)
    env["GITHUB_ACTION"] = step
    env["APT_STEP_BUDGET"] = budget
    env["APT_ATTEMPT_TIMEOUT"] = slice_s
    env["APT_ATTEMPTS"] = "3"
    env["PATH"] = f"{_hanging_apt(tmp_path)}:{env['PATH']}"
    return subprocess.run(["bash", str(_WRAPPER), "update"], env=env,
                          capture_output=True, text=True, timeout=180)


def test_the_give_up_line_agrees_with_the_attempt_lines(tmp_path: Path) -> None:
    """#1604: the two terminal messages used to contradict the progress lines.

    The budget guard and the loop exit each printed their own summary and were
    mutually exclusive: when the budget fell below the floor before the LAST
    attempt, the guard fired and `all N attempts failed` became unreachable, so
    a reader saw `attempt 3/3` followed by a give-up naming a different attempt
    number and no total. Observed on #1570, run 32275153453.

    Budget 30s with a 12s slice spends 24s on two attempts and leaves 6s, which
    is under the 10s floor -- the exact shape that used to contradict itself.
    """
    proc = _run_hanging(tmp_path, budget="30", slice_s="12", step="short-budget")
    err = proc.stderr

    assert proc.returncode != 0, f"a hung apt must not report success:\n{err}"
    assert "attempt 2/3" in err, f"expected the second attempt to announce itself:\n{err}"
    assert "giving up after 2/3 attempt(s)" in err, (
        f"the give-up line must state how many attempts actually RAN, and it must "
        f"agree with the last 'attempt N/M' line:\n{err}"
    )
    # The contradictory phrasing must be gone entirely.
    assert "before attempt" not in err, (
        f"'before attempt N' named an attempt that never started and could follow "
        f"a HIGHER attempt number:\n{err}"
    )


def test_exhausting_every_attempt_says_so(tmp_path: Path) -> None:
    """The other exit path must produce the same shape of sentence."""
    proc = _run_hanging(tmp_path, budget="40", slice_s="12", step="full-budget")
    err = proc.stderr
    assert "attempt 3/3" in err, err
    assert "giving up after 3/3 attempt(s)" in err, (
        f"all three ran, so the total must say 3/3:\n{err}"
    )
    assert "every attempt the budget allowed has failed" in err, err


def test_an_invocation_that_never_ran_apt_says_zero_attempts(tmp_path: Path) -> None:
    """The most important case to read correctly: apt-get never executed.

    A second invocation in a step whose budget an earlier one consumed must say
    0 attempts and still fail -- reporting success there would be a silent
    green for a package that was never installed.
    """
    deadline = tmp_path / "apt-bounded.spent.deadline"
    deadline.write_text(str(int(time.time()) - 1), encoding="utf-8")
    proc = _run_hanging(tmp_path, budget="30", slice_s="12", step="spent")
    err = proc.stderr
    assert proc.returncode != 0, f"must not exit 0 having run nothing:\n{err}"
    assert "giving up after 0/3 attempt(s)" in err, (
        f"an invocation that ran no apt-get must say 0, not omit the count:\n{err}"
    )

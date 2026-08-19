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
import subprocess
import time
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_WRAPPER = _REPO_ROOT / "scripts" / "ci" / "apt-bounded.sh"


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

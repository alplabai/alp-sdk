# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/west_patch_apply.py (issue #1392, item 3).

The invariant: a `west patch apply` that applied NOTHING (or fewer patches
than `zephyr/patches.yml` carries) must fail the caller, instead of exiting 0
the way `west patch apply` itself does.

Two upstream code paths make west exit 0 while doing nothing -- both read out
of zephyr v4.4.1's `scripts/west_commands/patch.py`:

  * `load_yml()` prints `no patches to apply: {path} not found` and returns
    None when the resolved `patches.yml` is not a file (what `-b`/`-l`
    resolving against the wrong manifest repo produced in `C:\\alp\\ws1380`);
  * `apply()` does `mod = self.get_module_path(...); if mod is None: continue`
    -- an unresolvable `module:` is skipped silently and is simply absent from
    the final `{patch_count} patches applied successfully \\o/` line (the
    #1076 class, where two hal_alif patches were skipped by every run).
"""
from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "west_patch_apply.py"
WORKFLOW = REPO / ".github" / "workflows" / "pr-twister-aen.yml"


def _patches_yml(root: Path, count: int) -> Path:
    """Write a `zephyr/patches.yml` carrying `count` entries."""
    entries = "\n".join(
        textwrap.dedent(
            f"""\
              - path: zephyr/{i:04d}-fake.patch
                sha256sum: {'0' * 64}
                module: zephyr
            """
        ).rstrip()
        for i in range(1, count + 1)
    )
    yml = root / "zephyr" / "patches.yml"
    yml.parent.mkdir(parents=True, exist_ok=True)
    yml.write_text("patches:\n" + entries + "\n", encoding="utf-8")
    return yml


def _stub_west(bin_dir: Path, stdout: str, rc: int = 0) -> None:
    """Put a fake `west` on PATH that prints `stdout` and exits `rc`."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    exe = bin_dir / "west"
    exe.write_text(
        "#!/bin/sh\n"
        f"cat <<'EOF'\n{stdout}\nEOF\n"
        f"exit {rc}\n",
        encoding="utf-8",
    )
    exe.chmod(0o755)


def _run(patches_yml: Path, bin_dir: Path, *extra: str):
    env = dict(os.environ)
    env["PATH"] = f"{bin_dir}{os.pathsep}{env['PATH']}"
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--patches-yml", str(patches_yml), *extra],
        capture_output=True,
        text=True,
        env=env,
    )


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_refuses_the_silent_no_op(tmp_path):
    """`no patches to apply: ... not found` + exit 0 must NOT pass."""
    yml = _patches_yml(tmp_path, 6)
    _stub_west(
        tmp_path / "bin",
        "no patches to apply: /x/alp-sdk/alp-sdk/zephyr/patches.yml not found",
    )
    proc = _run(yml, tmp_path / "bin")
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "applied no patches" in proc.stderr


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_refuses_a_partial_apply(tmp_path):
    """A skipped `module:` (#1076) shows up as a short count, not an error."""
    yml = _patches_yml(tmp_path, 6)
    _stub_west(tmp_path / "bin", "4 patches applied successfully \\o/")
    proc = _run(yml, tmp_path / "bin")
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "applied 4 of the 6 patches" in proc.stderr


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_accepts_a_full_apply(tmp_path):
    yml = _patches_yml(tmp_path, 6)
    _stub_west(tmp_path / "bin", "6 patches applied successfully \\o/")
    proc = _run(yml, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "6 of 6" in proc.stdout


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_propagates_a_west_failure(tmp_path):
    """west's own non-zero rc must come back verbatim, not just non-zero.

    rc=3 is deliberately not 1 (the wrapper's own generic-failure rc) and not
    2 (a common shell/argparse rc): only a wrapper that actually forwards
    `proc.returncode` from the west subprocess can produce exactly 3 here. A
    wrapper whose rc-propagation branch has been deleted still returns
    non-zero (via the `applied is None` path, since the stub prints no
    success line) but returns its OWN rc, not west's -- see #1392 review
    finding 2.
    """
    yml = _patches_yml(tmp_path, 6)
    _stub_west(tmp_path / "bin", "failed to apply patch zephyr/0002-ipm.patch", rc=3)
    proc = _run(yml, tmp_path / "bin")
    assert proc.returncode == 3, proc.stdout + proc.stderr


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_passes_patches_yml_to_west(tmp_path):
    """`--patches-yml`'s value must reach west's own argv.

    `expected_patch_count()` reads `--patches-yml` to get its count, but that
    is a different code path from the one that invokes west. Before this fix
    `run_west_patch_apply()` built argv as `["west", "patch", "apply", *extra]`
    with no `-l`/`-b`, so west silently fell back to its own default
    `zephyr/patches.yml` resolution -- for a non-default `--patches-yml` that
    is a different file than the one just counted, and the wrapper could
    print a success line about a manifest west never opened (#1392, finding
    1). A stub `west` here logs its own argv so the test inspects what the
    subprocess actually received, not just the wrapper's own file reads.
    """
    yml = _patches_yml(tmp_path, 6)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    argv_log = tmp_path / "argv.txt"
    exe = bin_dir / "west"
    exe.write_text(
        "#!/bin/sh\n"
        f'echo "$@" > "{argv_log}"\n'
        "echo '6 patches applied successfully \\o/'\n",
        encoding="utf-8",
    )
    exe.chmod(0o755)
    proc = _run(yml, bin_dir)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    argv = argv_log.read_text(encoding="utf-8")
    resolved_yml = str(yml.resolve())
    assert resolved_yml in argv, (
        f"--patches-yml's resolved path never reached west's argv: {argv!r}"
    )
    assert "-l" in argv.split(), f"no -l flag in west's argv: {argv!r}"
    assert "-b" in argv.split(), f"no -b flag in west's argv: {argv!r}"
    assert str(yml.resolve().parent / "patches") in argv, (
        f"no -b patch-base directory in west's argv: {argv!r}"
    )


@pytest.mark.skipif(os.name == "nt", reason="/bin/sh stub west is POSIX-only")
def test_expect_applied_override(tmp_path):
    """`--expect-applied` lets a filtered run (`-dm`) state its own count."""
    yml = _patches_yml(tmp_path, 6)
    _stub_west(tmp_path / "bin", "2 patches applied successfully \\o/")
    proc = _run(yml, tmp_path / "bin", "--expect-applied", "2")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_missing_patches_yml_is_refused_before_west_runs(tmp_path):
    """The `-b`/`-l`-resolved-to-nowhere case, caught without invoking west."""
    missing = tmp_path / "alp-sdk" / "zephyr" / "patches.yml"
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--patches-yml", str(missing)],
        capture_output=True,
        text=True,
    )
    assert proc.returncode != 0
    assert str(missing) in proc.stderr


def test_empty_patches_list_is_refused(tmp_path):
    yml = tmp_path / "zephyr" / "patches.yml"
    yml.parent.mkdir(parents=True, exist_ok=True)
    yml.write_text("patches: []\n", encoding="utf-8")
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--patches-yml", str(yml)],
        capture_output=True,
        text=True,
    )
    assert proc.returncode != 0
    assert "carries no patches" in proc.stderr


def test_real_patches_yml_is_countable():
    """The repo's own zephyr/patches.yml parses and carries entries."""
    sys.path.insert(0, str(REPO / "scripts"))
    import west_patch_apply

    assert west_patch_apply.expected_patch_count(REPO / "zephyr" / "patches.yml") > 0


def test_ci_step_does_not_trust_west_exit_0():
    """pr-twister-aen.yml:166 must go through the guard, not bare west."""
    wf = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    # Match on the step NAME as well as its `run:` body: once the step is
    # wired through the wrapper its body no longer contains the literal
    # `west patch apply`, and a run-body-only match would then find nothing
    # and vacuously pass.
    runs = [
        step["run"]
        for job in wf["jobs"].values()
        for step in job.get("steps", [])
        if isinstance(step, dict)
        and "west patch apply" in f"{step.get('name', '')}\n{step.get('run', '')}"
    ]
    assert runs, "no step in pr-twister-aen.yml invokes `west patch apply`"
    for run in runs:
        assert "scripts/west_patch_apply.py" in run, (
            "the west patch apply step must call scripts/west_patch_apply.py; "
            "a bare `west patch apply` exits 0 having done nothing (#1392)"
        )
        bare = [
            line
            for line in run.splitlines()
            if line.strip().startswith("west patch apply")
        ]
        assert not bare, f"bare `west patch apply` invocation left in the step: {bare}"

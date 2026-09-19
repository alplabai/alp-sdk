# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2189 -- the Flow D batch runner's argument and exit-8 contract.

`scripts/bench/aen/flash-all-flowd.sh` invoked `flash-jlink.sh` with
positional arguments only, forwarding neither `--atoc-unqueryable` nor
`--replace-atoc`. Once #2029 made `--atoc-unqueryable` mandatory wherever the
resident ATOC cannot be queried, EVERY entry in the batch aborted with exit 8
on a bench slot with no `SE_UART` exported and the batch flashed nothing at
all -- and on a slot with no SE-UART wired, where Flow D is the only load
path, that left the batch runner entirely non-functional. Worse, exit 8 was
the one Flow D exit code the summary map did not explain, so the operator got
a `BATCH SUMMARY` that was 100% `FLASH-ERROR (exit 8)` with no line naming the
cause.

Everything asserted here is host-side shell behaviour: the batch runner is
driven against a STUB `flash-jlink.sh` that records its own argv and exits
with a scripted status. No probe, no module, no SETOOLS install, and no
`flash-jlink.sh` of any kind is reached.

Two properties are load-bearing enough to be worth stating plainly, because
a plausible "simplification" of either re-opens a destructive path:

  * `--atoc-unqueryable` is OPT-IN here, never hardcoded at the call site.
    Hardcoding it would make the batch replace the resident ATOC blindly on
    every run with the acknowledgement recorded nowhere -- the #2025 hazard,
    re-entered through the batch runner.
  * `--replace-atoc` is deliberately NOT forwarded. It is the
    "I checked and still want to replace it" override, and a batch runner is
    exactly where a habitual override must not live.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"
BATCH = BENCH / "flash-all-flowd.sh"


def _sanitized_env() -> dict[str, str]:
    """Same discipline as tests/scripts/test_bench_jlink_connect_guard.py: a
    unit test must never be able to reach real bench infrastructure.
    Sourcing bench-env.sh with `LG_PLACE` inherited from an operator's own
    shell resolves it LIVE against the real labgrid coordinator the moment it
    is sourced, whatever the test then does."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT",
                "SE_UART", "AEN_JLINK_RUN"):
        env.pop(var, None)
    return env


def _bash_can_run_a_script() -> bool:
    """Probe by RUNNING something, never by `shutil.which` -- on GitHub's
    windows-latest runner `bash` resolves to the WSL launcher with no
    distribution installed and exits 1 for reasons unrelated to the code
    under test."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host (Windows CI resolves the WSL "
           "launcher with no distribution installed); flash-all-flowd.sh is "
           "a shell script and cannot be exercised here",
)


def _has_timeout_binary() -> bool:
    """`flash-all-flowd.sh` wraps each child in `timeout 120`. macOS ships no
    `timeout(1)` by default, so say which host cannot run this rather than
    reporting a phantom batch-runner defect."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "command -v timeout >/dev/null 2>&1"], timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0


_NEEDS_TIMEOUT = pytest.mark.skipif(
    not _has_timeout_binary(),
    reason="no `timeout(1)` on this host; flash-all-flowd.sh wraps every "
           "flash-jlink.sh call in it",
)


#: A stub `flash-jlink.sh`. Records its full argv, one call per line, into
#: $FLASH_STUB_LOG, then exits with the next status listed in
#: $FLASH_STUB_EXITS (whitespace separated; the LAST one repeats once the
#: list is exhausted, so a one-element list means "always this status").
_FLASH_STUB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >>"$FLASH_STUB_LOG"
n=$(wc -l <"$FLASH_STUB_LOG")
read -r -a codes <<<"$FLASH_STUB_EXITS"
idx=$((n - 1))
[ "$idx" -ge "${#codes[@]}" ] && idx=$((${#codes[@]} - 1))
echo "stub flash-jlink.sh call $n, exiting ${codes[$idx]}"
exit "${codes[$idx]}"
"""

#: `flash-all-flowd.sh` resolves an `arm-zephyr-eabi-nm` up front, via
#: bench_tool_prefix, and aborts without one. It is only used by the
#: post-flash RAM-console read, which no test here reaches (that path needs a
#: real probe), but it has to resolve for the script to start at all.
_NM_STUB = """#!/usr/bin/env bash
exit 0
"""


def _make_bench(tmp_path: Path, apps: list[str], exits: list[int]) -> tuple[Path, Path]:
    """Lay out a self-contained fake bench: the real batch runner and the
    real bench-env.sh, a stub `flash-jlink.sh` beside them, a stub toolchain
    on PATH, and a `$BENCH_ROOT/build/<app>` tree for each named app.

    Returns (workdir, stub-call-log).
    """
    workdir = tmp_path / "bench"
    workdir.mkdir()
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())
    (workdir / "flash-all-flowd.sh").write_bytes(BATCH.read_bytes())

    stub = workdir / "flash-jlink.sh"
    stub.write_text(_FLASH_STUB, encoding="utf-8")
    stub.chmod(0o755)

    bindir = tmp_path / "bin"
    bindir.mkdir()
    nm = bindir / "arm-zephyr-eabi-nm"
    nm.write_text(_NM_STUB, encoding="utf-8")
    nm.chmod(0o755)

    root = tmp_path / "bench-root"
    for app in apps:
        build = root / "build" / app / "zephyr"
        build.mkdir(parents=True)
        (build / "zephyr.bin").write_bytes(b"\x00" * 16)
        (build / "zephyr.elf").write_bytes(b"\x7fELF")

    # apps.txt exists so a no-argv run has a list; every test here passes
    # apps on argv, which the script prefers.
    (workdir / "apps.txt").write_text("\n".join(apps) + "\n", encoding="utf-8")

    log = tmp_path / "stub-calls.log"
    log.write_text("", encoding="utf-8")
    return workdir, log


def _run_batch(
    tmp_path: Path, argv: list[str], apps: list[str], exits: list[int],
) -> tuple[subprocess.CompletedProcess[str], list[str]]:
    """Run flash-all-flowd.sh with `argv`, against stub children that exit
    with `exits` in order. Returns (process, one string per stub call)."""
    workdir, log = _make_bench(tmp_path, apps, exits)
    env = _sanitized_env()
    env["PATH"] = f"{tmp_path / 'bin'}{os.pathsep}{env.get('PATH', '')}"
    env["BENCH_ROOT"] = str(tmp_path / "bench-root")
    env["FLASH_STUB_LOG"] = str(log)
    env["FLASH_STUB_EXITS"] = " ".join(str(c) for c in exits)
    proc = subprocess.run(
        ["bash", "flash-all-flowd.sh", *argv],
        cwd=workdir, env=env, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=120,
    )
    calls = [ln for ln in log.read_text(encoding="utf-8").splitlines() if ln.strip()]
    return proc, calls


# --- the flag reaches the children, and only because it was asked for ------


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_atoc_unqueryable_before_the_app_list_is_forwarded_to_every_child(tmp_path):
    """The ordinary invocation. Every child must receive the flag -- not just
    the first -- because the acknowledgement is about the bench slot, and the
    slot does not change between apps."""
    proc, calls = _run_batch(
        tmp_path, ["--atoc-unqueryable", "app-one", "app-two"],
        ["app-one", "app-two"], [3],
    )
    assert len(calls) == 2, f"expected one child per app, got {calls}\n{proc.stdout}"
    for call in calls:
        assert call.startswith("--atoc-unqueryable "), (
            f"flag not forwarded to every child: {calls}"
        )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_atoc_unqueryable_after_the_app_list_is_still_honoured(tmp_path):
    """The parser shape is load-bearing (alp-sdk#2189).

    This script's positionals are a variable-length app list, so
    `flash-run.sh`'s `while`/`shift` loop -- which stops honouring flags at
    the first non-option token -- would SILENTLY ignore the flag here. And
    silently ignoring THIS flag means every entry aborts with exit 8 again,
    which is the whole bug being fixed. The whole-argv `for` scan
    `flash-jlink.sh` already uses for this same flag is what makes both
    orderings work.
    """
    proc, calls = _run_batch(
        tmp_path, ["app-one", "app-two", "--atoc-unqueryable"],
        ["app-one", "app-two"], [3],
    )
    assert len(calls) == 2, f"expected one child per app, got {calls}\n{proc.stdout}"
    for call in calls:
        assert call.startswith("--atoc-unqueryable "), (
            f"a trailing flag was dropped -- the parser stopped honouring "
            f"flags at the first app name: {calls}"
        )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_the_flag_never_becomes_an_app_name(tmp_path):
    """The flag must be consumed by the scan, not left in the positional app
    list -- otherwise the batch looks for `$BENCH_ROOT/build/--atoc-unqueryable`
    and reports it as a skipped app."""
    proc, calls = _run_batch(
        tmp_path, ["--atoc-unqueryable", "app-one"], ["app-one"], [3],
    )
    assert len(calls) == 1, f"expected exactly one child, got {calls}\n{proc.stdout}"
    assert "--atoc-unqueryable :" not in proc.stdout, (
        f"the flag was treated as an app name:\n{proc.stdout}"
    )
    assert "########## --atoc-unqueryable" not in proc.stdout, (
        f"the flag was treated as an app name:\n{proc.stdout}"
    )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_the_flag_is_opt_in_and_never_hardcoded(tmp_path):
    """Without the flag on argv, no child may receive it.

    This is the #2025 property, and the reason the fix is a forwarding scan
    rather than a hardcoded argument: a batch that always passed
    `--atoc-unqueryable` would replace the resident ATOC blindly on every
    run with the acknowledgement recorded nowhere. The operator says
    "I cannot check" once, deliberately.
    """
    proc, calls = _run_batch(tmp_path, ["app-one"], ["app-one"], [3])
    assert len(calls) == 1, f"expected exactly one child, got {calls}\n{proc.stdout}"
    assert "--atoc-unqueryable" not in calls[0], (
        f"the flag was passed without being asked for: {calls}"
    )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_replace_atoc_is_not_forwarded(tmp_path):
    """`--replace-atoc` is the "I checked and still want to replace it"
    override. It is deliberately not a batch-runner argument, and must not
    reach a child even when passed here -- see flash-jlink.sh's own header on
    why the two flags must never be merged or aliased."""
    proc, calls = _run_batch(
        tmp_path, ["--replace-atoc", "app-one"], ["app-one"], [3],
    )
    assert calls, f"no child ran at all:\n{proc.stdout}"
    assert "--replace-atoc" not in calls[0], (
        f"the override leaked into a child: {calls}"
    )


# --- exit 8: a batch-level refusal, labelled and stopped once --------------


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_exit_8_is_labelled_stops_the_batch_and_still_summarises(tmp_path):
    """Exit 8 is `bench_flowd_atoc_guard`'s refusal: no `SE_UART` and no
    `--atoc-unqueryable`.

    Unlike every other exit here it is a BATCH-level configuration refusal,
    decided before any probe or target access, so it cannot differ between
    apps -- every remaining entry would abort identically. Three things must
    hold together: it gets its own `FLASH-REFUSED` label rather than falling
    to the catch-all `FLASH-ERROR (exit N)`; the batch stops instead of
    printing the same guard text once per app; and the `BATCH SUMMARY` still
    prints, carrying the refused entry.
    """
    proc, calls = _run_batch(
        tmp_path, ["app-one", "app-two", "app-three"],
        ["app-one", "app-two", "app-three"], [8],
    )
    assert len(calls) == 1, (
        f"the batch kept going after a run-level refusal -- every remaining "
        f"app would abort identically: {calls}\n{proc.stdout}"
    )
    assert "FLASH-REFUSED" in proc.stdout, (
        f"exit 8 fell through to the catch-all label:\n{proc.stdout}"
    )
    assert "FLASH-ERROR (exit 8)" not in proc.stdout, (
        f"exit 8 is still being reported as an unexplained error:\n{proc.stdout}"
    )
    assert "ABORTING BATCH" in proc.stdout, (
        f"the batch stopped without saying why:\n{proc.stdout}"
    )
    assert "--atoc-unqueryable" in proc.stdout, (
        f"the refusal must name the way forward:\n{proc.stdout}"
    )
    assert "BATCH SUMMARY" in proc.stdout, (
        f"breaking out of the loop skipped the summary:\n{proc.stdout}"
    )
    summary = proc.stdout.split("BATCH SUMMARY")[-1]
    assert "app-one" in summary, f"the refused entry is missing from the summary:\n{summary}"
    assert "app-three" not in summary, (
        f"an app that never ran appears in the summary:\n{summary}"
    )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_earlier_entries_keep_their_own_verdicts_when_the_batch_is_refused(tmp_path):
    """The early stop must not rewrite what already happened: an app that
    genuinely failed its verify before the refusal keeps its own label."""
    proc, calls = _run_batch(
        tmp_path, ["app-one", "app-two", "app-three"],
        ["app-one", "app-two", "app-three"], [3, 8],
    )
    assert len(calls) == 2, f"expected two children, got {calls}\n{proc.stdout}"
    summary = proc.stdout.split("BATCH SUMMARY")[-1]
    assert "app-one : FLASH-UNVERIFIED" in summary, (
        f"the first app's real verdict was lost:\n{summary}"
    )
    assert "app-two : FLASH-REFUSED" in summary, (
        f"the refusing app is missing or mislabelled:\n{summary}"
    )


@_NEEDS_BASH
@_NEEDS_TIMEOUT
def test_a_per_app_failure_still_continues_the_batch(tmp_path):
    """The early stop is scoped to exit 8 alone.

    `flash-all-flowd.sh` is deliberately resilient -- "a failed app is logged
    and the batch continues" -- and exit 8 is the single documented departure
    from that. A fix that stopped on any non-zero status would quietly turn a
    one-app verify failure into a dead batch, so pin the other direction too.
    """
    proc, calls = _run_batch(
        tmp_path, ["app-one", "app-two"], ["app-one", "app-two"], [3],
    )
    assert len(calls) == 2, (
        f"a per-app failure stopped the batch: {calls}\n{proc.stdout}"
    )
    assert "ABORTING BATCH" not in proc.stdout, (
        f"a per-app failure was treated as a run-level refusal:\n{proc.stdout}"
    )
    summary = proc.stdout.split("BATCH SUMMARY")[-1]
    assert "app-one : FLASH-UNVERIFIED" in summary, summary
    assert "app-two : FLASH-UNVERIFIED" in summary, summary

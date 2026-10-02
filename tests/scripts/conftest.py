"""Pytest configuration for tests/scripts/: put scripts/ on sys.path."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make packages under scripts/ (alp_cli, alp_orchestrate, ...) importable directly.
_scripts = Path(__file__).resolve().parents[2] / "scripts"
if str(_scripts) not in sys.path:
    sys.path.insert(0, str(_scripts))

_REPO = Path(__file__).resolve().parents[2]
_CLANG_FORMAT_STYLE = _REPO / ".clang-format"

# The repo pins this exact version (pr-generated-files.yml / pr-static-
# analysis.yml, both `pip install clang-format==22.1.5`) because clang-format
# output moves between versions; the committed cap.h/cap.c/soc_caps.h are
# v22-formatted. Comparing against any other version turns a host-tool
# mismatch into a false "generator drifted" failure.
_CLANG_FORMAT_PIN = "22.1.5"

# Modules whose tests temporarily write into the real checkout, then clean up
# (#2328). Serially that is harmless; under pytest-xdist another worker that
# globs the same directory can see the transient file mid-test. It was
# observed: test_abi_snapshot's real-tree check picked up the freeze-gate
# test's fake docs/abi/v99.99-snapshot.json as the last released snapshot.
# Each module listed here gets the `repo_writes` marker. CI and test-all.sh
# run `-n auto -m "not repo_writes"` first and then `-m repo_writes`
# serially, so no parallel test ever overlaps one of these. A new test that
# writes into the checkout (instead of tmp_path) must be added here.
_REPO_WRITER_MODULES = frozenset({
    "test_abi_snapshot_freeze_gate",  # docs/abi/v99.9x-snapshot.json
    "test_validate_metadata_slot0_address",  # metadata/e1m_modules/.test-*.yaml
    "test_validate_metadata_som_memory_population",  # metadata/e1m_modules/.test-*.yaml
    "test_validate_metadata_soc_peripheral_instance_uniqueness",  # metadata/e1m_modules/.test-*.yaml
    "test_validate_metadata_duplicate_keys",  # metadata/chips/.test-dup-*.{yaml,json}
    "test_check_atoc_class_disagreement",  # metadata/e1m_modules/.test-*.yaml
    "test_check_atoc_aperture_tiling",  # metadata/e1m_modules/.test-*.yaml
    "test_test_all_worktree",  # `git worktree add` against the shared .git
})


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "repo_writes: writes into the real checkout; run serially, never "
        "alongside pytest-xdist workers (see _REPO_WRITER_MODULES)",
    )
    config.addinivalue_line(
        "markers",
        "gate_duplicate: re-runs, from pytest, exactly the live-repo check a "
        "scripts/check_*.py gate stage already runs; scripts/test-all.sh "
        "deselects these when that stage ran in the same invocation, CI's "
        "plain pytest sweep still runs them",
    )
    here = Path(__file__).resolve().parent
    stale = sorted(m for m in _REPO_WRITER_MODULES if not (here / f"{m}.py").is_file())
    if stale:
        # A renamed or split writer module would otherwise drop silently back
        # into the parallel phase.
        raise pytest.UsageError(
            f"_REPO_WRITER_MODULES names module(s) that no longer exist: {stale}")


def pytest_collection_modifyitems(config, items):
    # On an xdist worker a repo_writes test would race the other workers --
    # someone ran `-n` without `-m "not repo_writes"`. Skip it there with the
    # right command in the reason, rather than let the race back in or crash
    # the worker. (-m deselection runs after this hook, so on the proper
    # parallel phase these items are deselected anyway.)
    on_xdist_worker = hasattr(config, "workerinput")
    for item in items:
        if item.path.stem in _REPO_WRITER_MODULES:
            item.add_marker(pytest.mark.repo_writes)
            if on_xdist_worker:
                item.add_marker(pytest.mark.skip(
                    reason="writes into the checkout: not safe under pytest-xdist; "
                           "run `pytest tests/scripts/ -m repo_writes` without -n"))


def clang_format_text(tmp_path: Path, name: str, text: str) -> str:
    """Write `text` under tmp_path and run it through the repo's clang-format,
    the way a generator's own post-processing pass formats its real output --
    without ever writing into the working tree.

    Uses an explicit `file:<path>` style argument rather than plain
    `--style=file` (which walks up from the *file's own* directory): tmp_path
    lives outside the repo, so implicit lookup would silently pick up whatever
    stray .clang-format (if any) happens to sit above it on the filesystem,
    not this repo's.

    Skips (does not fail) unless the exact pinned clang-format is on PATH:
    the byte-identity check this feeds is already gated with the pinned
    toolchain by the `generated-files` CI job, so a host without it (or with
    a different version) isn't a real drift signal -- see #973 finding 1/2.
    """
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if not exe:
        pytest.skip("clang-format not found on PATH")
    version_out = subprocess.run(
        [exe, "--version"], capture_output=True, text=True, check=True,
        encoding="utf-8",
    ).stdout
    match = re.search(r"(\d+\.\d+\.\d+)", version_out)
    found = match.group(1) if match else version_out.strip()
    if found != _CLANG_FORMAT_PIN:
        pytest.skip(
            f"clang-format {found} on PATH, need pinned {_CLANG_FORMAT_PIN}"
        )
    path = tmp_path / name
    path.write_text(text, encoding="utf-8", newline="")
    subprocess.run(
        [exe, "-i", f"--style=file:{_CLANG_FORMAT_STYLE}", str(path)], check=True
    )
    return path.read_text(encoding="utf-8")


@pytest.fixture(autouse=True)
def _sandbox_tmpdir(tmp_path_factory, monkeypatch):
    """Point TMPDIR at a per-test directory under pytest's own basetemp.

    The bench scripts keep what they write under ${TMPDIR:-/tmp} on purpose
    -- bench_atoc_replace_guard()'s `<tag>-atoc-before.<random>` transcript
    is the audit record of what was resident before a destructive write, so
    it is never removed. That is right on a bench and wrong in a unit test:
    every test that sources bench-env.sh without exporting its own TMPDIR
    left its files in the host's real /tmp, and one board-farm host
    accumulated about 20,000 of them in three weeks. pytest prunes its
    basetemp to the last three runs, so a sandbox there cleans itself up.

    A test that exports TMPDIR inside the script it runs still wins: this
    only changes what an unset TMPDIR falls back to. `tempfile` in the
    pytest process itself is unaffected -- it caches gettempdir() once.
    """
    monkeypatch.setenv("TMPDIR", str(tmp_path_factory.mktemp("tmpdir")))

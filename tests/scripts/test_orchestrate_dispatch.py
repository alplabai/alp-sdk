# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- Orchestrator.fan_out()
end-to-end dispatch, including the #591 build-cache invalidation
contract.

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_dispatch.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    Orchestrator,
    OrchestratorError,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# 10. Orchestrator.fan_out end-to-end with missing tools
# ---------------------------------------------------------------------


def test_orchestrator_fan_out_skips_when_tools_absent(
    tmp_path: Path, monkeypatch
) -> None:
    """On a host where neither bitbake nor west exists, every slice
    must end up `status: skipped` (not failed) and the manifest must
    be well-formed."""
    # Stub `shutil.which` to claim neither tool is on PATH.
    import alp_orchestrate
    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    manifest = orch.fan_out(parallel=False)

    # Manifest reflects two slices, both skipped (tools missing).
    statuses = {s.core_id: s.status for s in manifest.slices}
    assert statuses["a55_cluster"] == "skipped"
    assert statuses["m33_sm"] == "skipped"

    # Reasons are descriptive (mention the missing tool).
    a55 = next(s for s in manifest.slices if s.core_id == "a55_cluster")
    m33 = next(s for s in manifest.slices if s.core_id == "m33_sm")
    assert "bitbake" in (a55.reason or "")
    assert "west" in (m33.reason or "")

    # Shared artefacts materialised.  The IPC header lands under
    # `alp/` so consumer code uses `#include <alp/system_ipc.h>`
    # canonically (build_root/generated ends up on the include path).
    assert (build_root / "generated" / "alp" / "system_ipc.h").is_file()
    assert (build_root / "generated" / "dts-reservations.dtsi").is_file()
    assert (build_root / "system-manifest.yaml").is_file()

    # State cache: no slice was ok, so nothing should be persisted as
    # `status: ok` in the cache file.  The cache file may or may not
    # exist; if it does, it should be empty / have no `ok` entries.
    if orch.state_path.is_file():
        import json
        state = json.loads(orch.state_path.read_text(encoding="utf-8"))
        for entry in state.values():
            assert entry.get("status") != "ok" or entry.get("hash") is None


def test_fan_out_rejects_unknown_only_core(tmp_path: Path) -> None:
    """#603: `--core <unknown>` used to silently skip every real slice,
    still write a manifest, and return a clean (0-slice-failed)
    manifest -- indistinguishable from a deliberately-scoped success.
    `fan_out(only_core=...)` must reject it immediately, before any
    artefact is written."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    with pytest.raises(OrchestratorError) as excinfo:
        orch.fan_out(only_core="m99_garbage", parallel=False)
    msg = str(excinfo.value)
    assert "m99_garbage" in msg
    assert "a55_cluster" in msg
    assert "m33_sm" in msg
    # No manifest / build-root artefacts get written for a rejected
    # --core -- the failure happens before materialisation.
    assert not (build_root / "system-manifest.yaml").is_file()


# ---------------------------------------------------------------------
# 10b. Cache invalidation (#591): a real change must always invoke a
# rebuild; an unchanged re-run must still hit the cache.
# ---------------------------------------------------------------------


class _FakeCompletedProcess:
    """Stand-in for subprocess.CompletedProcess -- `_dispatch_slice`
    only reads `.returncode`, so this avoids ever spawning a real
    `west`/`bitbake`/`cmake`."""

    def __init__(self, returncode: int = 0) -> None:
        self.returncode = returncode


def _fake_dispatch(monkeypatch, call_counter: list) -> None:
    """Make every slice's tool 'present' and every dispatch 'succeed'
    without a real subprocess -- lets `fan_out()` exercise the real
    cache-hit / rebuild decision end-to-end, and lets the test count
    how many times the builder was actually invoked."""
    import alp_orchestrate

    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: f"/usr/bin/{name}")

    def _run(cmd, **kwargs):
        call_counter[0] += 1
        return _FakeCompletedProcess(0)

    monkeypatch.setattr(alp_orchestrate.orchestrator.subprocess, "run", _run)


def _v2n_project_with_app(tmp_path: Path) -> Path:
    """Writes V2N_HAPPY plus a real `./m33` app dir (one source file) --
    so both the dispatch path (`m33_sm` needs a `board:` to get a real
    command) and the app-source hash have something real to work with.
    Returns the board.yaml path; callers `load_board_yaml` it fresh per
    Orchestrator instance (mirrors two separate CLI invocations)."""
    path = _write_board(tmp_path, V2N_HAPPY)
    app_dir = tmp_path / "m33"
    app_dir.mkdir()
    (app_dir / "main.c").write_text("int main(void) { return 0; }\n",
                                     encoding="utf-8")
    return path


def _fresh_orch(path: Path, build_root: Path) -> Orchestrator:
    project = load_board_yaml(path)
    project.cores["m33_sm"].board = "test_m33_board"
    return Orchestrator(project, build_root, board_yaml=path)


def test_unchanged_rerun_hits_cache(tmp_path: Path, monkeypatch) -> None:
    """#591 acceptance: a re-run with nothing changed must still report
    cache-hit and must NOT re-invoke the builder."""
    path = _v2n_project_with_app(tmp_path)
    build_root = tmp_path / "build"
    calls = [0]
    _fake_dispatch(monkeypatch, calls)

    orch_a = _fresh_orch(path, build_root)
    orch_a.fan_out(parallel=False)
    m33_a = orch_a.project.cores["m33_sm"]
    assert m33_a.status == "ok"
    assert "cache-hit" not in (m33_a.reason or "")
    first_calls = calls[0]
    assert first_calls >= 1

    orch_b = _fresh_orch(path, build_root)
    orch_b.fan_out(parallel=False)
    m33_b = orch_b.project.cores["m33_sm"]
    assert m33_b.status == "ok"
    assert "cache-hit" in (m33_b.reason or "")
    assert calls[0] == first_calls, "unchanged re-run must not re-dispatch"


def test_source_change_invalidates_cache(tmp_path: Path, monkeypatch) -> None:
    """#591's core repro: editing an app source file must invoke the
    builder again, not report a stale cache-hit that leaves the old
    artefact in place."""
    path = _v2n_project_with_app(tmp_path)
    build_root = tmp_path / "build"
    calls = [0]
    _fake_dispatch(monkeypatch, calls)

    orch_a = _fresh_orch(path, build_root)
    orch_a.fan_out(parallel=False)
    first_calls = calls[0]
    assert first_calls >= 1

    # Repro step: edit the application source.
    (tmp_path / "m33" / "main.c").write_text(
        "int main(void) { return 1; } /* changed */\n", encoding="utf-8")

    orch_b = _fresh_orch(path, build_root)
    orch_b.fan_out(parallel=False)
    m33_b = orch_b.project.cores["m33_sm"]
    assert m33_b.status == "ok"
    assert "cache-hit" not in (m33_b.reason or "")
    assert calls[0] > first_calls, "source edit must re-invoke the builder"


def test_sdk_version_change_invalidates_cache(tmp_path: Path, monkeypatch) -> None:
    """#591 acceptance: an SDK upgrade must invalidate every slice's
    cache even though board.yaml (and the app source) is untouched."""
    import alp_orchestrate

    path = _v2n_project_with_app(tmp_path)
    build_root = tmp_path / "build"
    calls = [0]
    _fake_dispatch(monkeypatch, calls)

    orch_a = _fresh_orch(path, build_root)
    orch_a.fan_out(parallel=False)
    first_calls = calls[0]
    assert first_calls >= 1

    # Simulate an SDK version bump without touching board.yaml or the
    # app source at all.
    monkeypatch.setattr(
        alp_orchestrate.orchestrator, "_sdk_version_token",
        lambda: "version: 9.9.9\nstatus: development\n")

    orch_b = _fresh_orch(path, build_root)
    orch_b.fan_out(parallel=False)
    m33_b = orch_b.project.cores["m33_sm"]
    assert m33_b.status == "ok"
    assert "cache-hit" not in (m33_b.reason or "")
    assert calls[0] > first_calls, "SDK version bump must re-invoke the builder"


def test_missing_build_log_prevents_cache_hit(tmp_path: Path, monkeypatch) -> None:
    """Acceptance: 'missing output artefacts cannot produce a cache
    hit' -- if the on-disk evidence of a prior build (build.log) is
    gone, a matching hash + `status: ok` in the state file alone must
    not fabricate a cache-hit."""
    path = _v2n_project_with_app(tmp_path)
    build_root = tmp_path / "build"
    calls = [0]
    _fake_dispatch(monkeypatch, calls)

    orch_a = _fresh_orch(path, build_root)
    orch_a.fan_out(parallel=False)
    first_calls = calls[0]
    assert first_calls >= 1

    log_path = build_root / "m33_sm-zephyr" / "build.log"
    assert log_path.is_file()
    log_path.unlink()

    orch_b = _fresh_orch(path, build_root)
    orch_b.fan_out(parallel=False)
    m33_b = orch_b.project.cores["m33_sm"]
    assert m33_b.status == "ok"
    assert "cache-hit" not in (m33_b.reason or "")
    assert calls[0] > first_calls, "missing build.log must not cache-hit"



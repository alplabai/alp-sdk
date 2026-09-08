# SPDX-License-Identifier: Apache-2.0
"""The `.snap` goldens participate in `pytest tests/scripts/` (#2023).

`tests/scripts/test_check_emit_snapshots.py` unit-tests only
`_normalize_host_paths`, so every committed golden under
`tests/fixtures/emit-snapshots/` was guarded SOLELY by
`scripts/check_emit_snapshots.py` running inside `scripts/test-all.sh`.
Anyone who ran `pytest tests/scripts -q`, read green and pushed had no
snapshot coverage at all.

That is measured, not theorised: during the mutation review of #1365
split B (#2010), mutants **A2** and **A3** survived `pytest tests/scripts/`
entirely and were killed only by the script.

The fixtures carry the exact refusal text that ships into
`build/generated/alp/system_ipc.h` and `dts-reservations.dtsi`, which is
how #2009 -- a refusal instructing an edit the same function refuses --
reached four committed goldens and customer-visible generated output.

This closes the gap the cheap way the issue prefers: invoke the gate from
pytest so there is ONE source of truth for what "in sync" means, rather
than a second fixture-diff implementation that can itself drift.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_emit_snapshots.py"
SNAP_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"


def _load():
    spec = importlib.util.spec_from_file_location("check_emit_snapshots", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_every_committed_golden_is_in_sync():
    """The gate itself, run from pytest.

    Slow by the standards of this file (it re-emits every `--emit`
    surface), and that cost IS the point: without it the suite says
    nothing about the goldens.
    """
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "byte-identical" in proc.stdout


def test_every_case_has_a_committed_golden_and_no_golden_is_orphaned():
    """A new `CASES` entry without a committed golden, or a golden whose
    case was deleted, both mean the fixture set and the emitter have
    drifted apart -- and neither shows up as a DIFF."""
    mod = _load()
    declared = {case[0] for case in mod.CASES}
    on_disk = {p.stem for p in SNAP_DIR.glob("*.snap")}

    assert declared - on_disk == set(), "CASES entries with no golden"
    assert on_disk - declared == set(), "goldens no case emits any more"


def test_a_perturbed_golden_is_detected(tmp_path, monkeypatch):
    """The refusing direction, pinned.

    Without this, `test_every_committed_golden_is_in_sync` above could
    pass for the wrong reason (a gate that never fails is not a gate).
    Perturbs a COPY -- the committed fixtures are never written to.
    `CASES` is narrowed to one entry so this costs one emit, not 37.
    """
    mod = _load()
    case = next(c for c in mod.CASES if c[0].endswith(".system-manifest"))
    snap_id = case[0]

    work = tmp_path / "emit-snapshots"
    work.mkdir()
    shutil.copyfile(SNAP_DIR / f"{snap_id}.snap", work / f"{snap_id}.snap")
    golden = work / f"{snap_id}.snap"
    golden.write_text(
        golden.read_text(encoding="utf-8") + "# perturbed\n",
        encoding="utf-8", newline="")

    monkeypatch.setattr(mod, "SNAP_DIR", work)
    monkeypatch.setattr(mod, "CASES", [case])
    monkeypatch.setattr(sys, "argv", ["check_emit_snapshots.py"])

    assert mod.main() == 1


def test_a_missing_golden_is_detected(tmp_path, monkeypatch):
    """A case whose golden was never committed must fail rather than be
    silently skipped -- the `MISS` leg of the same gate."""
    mod = _load()
    case = next(c for c in mod.CASES if c[0].endswith(".system-manifest"))

    work = tmp_path / "emit-snapshots"
    work.mkdir()

    monkeypatch.setattr(mod, "SNAP_DIR", work)
    monkeypatch.setattr(mod, "CASES", [case])
    monkeypatch.setattr(sys, "argv", ["check_emit_snapshots.py"])

    assert mod.main() == 1


@pytest.mark.parametrize("surface", ["system-manifest", "build-plan"])
def test_both_emit_surfaces_are_represented(surface):
    """The two surfaces that reach customer-visible generated output
    (`system_ipc.h` / `dts-reservations.dtsi` come off the manifest, and
    the build plan is what `tan` executes) each keep at least one golden,
    so narrowing the fixture set to one surface cannot go unnoticed."""
    mod = _load()
    assert any(case[0].endswith(f".{surface}") for case in mod.CASES)

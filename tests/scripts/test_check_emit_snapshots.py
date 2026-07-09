# SPDX-License-Identifier: Apache-2.0
"""Unit tests for `_normalise_sdk_root()` in scripts/check_emit_snapshots.py.

Regression coverage for issue #472: a plain `str(REPO)` replace only ever
catches one spelling of the checkout root inside the emitted JSON.  On
Windows the same path also shows up JSON-escaped (backslashes doubled)
and, for emit paths that pre-normalise separators, in POSIX (forward-
slash) form.  All three spellings must collapse to the `<SDK_ROOT>`
sentinel so the goldens stay host-agnostic.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path, PureWindowsPath

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_emit_snapshots.py"


@pytest.fixture(scope="module")
def check_emit_snapshots():
    spec = importlib.util.spec_from_file_location("check_emit_snapshots", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["check_emit_snapshots"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_raw_repo_path_normalises(check_emit_snapshots):
    """Unix-style raw path (the current REPO's own spelling) still works."""
    mod = check_emit_snapshots
    text = f'{{"cwd": "{mod.REPO}"}}'
    assert mod._normalise_sdk_root(text) == '{"cwd": "<SDK_ROOT>"}'


def test_windows_json_escaped_repo_path_normalises(check_emit_snapshots):
    """Synthetic Windows checkout root, JSON-escaped (backslashes doubled),
    as it would actually appear inside emitted JSON text -- must collapse
    to the sentinel even though it never matches a plain str(REPO) on a
    POSIX host."""
    mod = check_emit_snapshots
    win_repo = r"C:\Users\dev\alp-sdk"
    win_repo_escaped = win_repo.replace("\\", "\\\\")
    text = (
        '{"env": {"ALP_SDK_ROOT": "' + win_repo_escaped + '"}, '
        '"args": ["west", "build", "-b", "alp_board", "'
        + win_repo_escaped + '"]}'
    )
    # Patch the module's REPO for the duration of this assertion so the
    # Windows-style needle is the one being searched for.
    orig_repo = mod.REPO
    try:
        mod.REPO = PureWindowsPath(win_repo)
        got = mod._normalise_sdk_root(text)
    finally:
        mod.REPO = orig_repo
    assert "<SDK_ROOT>" in got
    assert win_repo not in got
    assert win_repo_escaped not in got
    assert got == (
        '{"env": {"ALP_SDK_ROOT": "<SDK_ROOT>"}, '
        '"args": ["west", "build", "-b", "alp_board", "<SDK_ROOT>"]}'
    )


def test_windows_forward_slash_repo_path_normalises(check_emit_snapshots):
    """Some emit paths pre-normalise separators to `/` before the checkout
    root is stamped in; that POSIX spelling of a Windows root must also
    collapse to the sentinel."""
    mod = check_emit_snapshots
    win_repo = r"C:\Users\dev\alp-sdk"
    win_repo_posix = "C:/Users/dev/alp-sdk"
    text = f'{{"buildRoot": "{win_repo_posix}/build"}}'
    orig_repo = mod.REPO
    try:
        mod.REPO = PureWindowsPath(win_repo)
        got = mod._normalise_sdk_root(text)
    finally:
        mod.REPO = orig_repo
    assert got == '{"buildRoot": "<SDK_ROOT>/build"}'


def test_escaped_form_is_not_partially_eaten_by_raw_replace(check_emit_snapshots):
    """The escaped needle must be tried before the raw one -- otherwise a
    naive single-pass replace order can leave stray backslashes behind."""
    mod = check_emit_snapshots
    win_repo = r"C:\Users\dev\alp-sdk"
    win_repo_escaped = win_repo.replace("\\", "\\\\")
    orig_repo = mod.REPO
    try:
        mod.REPO = PureWindowsPath(win_repo)
        got = mod._normalise_sdk_root(win_repo_escaped)
    finally:
        mod.REPO = orig_repo
    assert got == "<SDK_ROOT>"

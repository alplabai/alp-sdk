# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for `_emit_extra_library_profile`'s never-raises contract
(issue #1961).

The function's docstring contracts it to return a `#`-prefixed
diagnostic-comment line, never raise, when the `profile:` file it walks
is missing, unreadable, or malformed.  On the default branch the
`try`/`except (OSError, yaml.YAMLError)` around the read+parse was
narrower than the failure modes of the I/O it wraps:

  * `.resolve()` sat OUTSIDE the try and raises `RuntimeError` on a
    symlink loop (CPython's `pathlib.Path.resolve(strict=False)`,
    `Lib/pathlib.py`: `check_eloop` -> `raise RuntimeError("Symlink
    loop from %r" % e.filename)` on `errno == ELOOP`).
  * `read_text(encoding="utf-8")` raises `UnicodeDecodeError` on
    non-UTF-8 bytes -- a `ValueError` subclass, not an `OSError` --
    *before* the YAML parser ever runs.

This module drives the seven shapes tan-cli's sibling gate
(`python/tan/planner/kconfig.py`'s `test_never_raises_contract_holds.py`,
tan-cli#1122) exercises, table-driven, plus the two mutation-proof tests
that show each fix is load-bearing.  Symlink loops and `chmod 000`
permission denial are POSIX-only (Windows has no unprivileged
`os.symlink` and `chmod` doesn't restrict owner-read) -- those two
shapes skip cleanly on `nt` rather than fail.

Run locally:

    python -m pytest tests/scripts/test_kconfig_extra_library_profile_never_raises.py -v
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import load_board_yaml  # noqa: E402
from alp_orchestrate.kconfig import _emit_extra_library_profile  # noqa: E402

_WINDOWS = os.name == "nt"


@pytest.fixture(scope="module")
def project(tmp_path_factory):
    """A real `BoardProject` (V2N101) -- `_emit_extra_library_profile`
    only reads `project.som_preset` / `project.effective_metadata_root()`
    once past the try/except, which none of the failing shapes reach."""
    tmp_path = tmp_path_factory.mktemp("v2n-happy-board")
    path = _write_board(tmp_path, V2N_HAPPY)
    return load_board_yaml(path)


# ---------------------------------------------------------------------
# Table-driven: seven shapes, all must RETURN a list, never raise.
# ---------------------------------------------------------------------

def _make_nonutf8(tmp_path: Path) -> Path:
    p = tmp_path / "nonutf8.yaml"
    p.write_bytes(b"\xff\xfe\x00bad")
    return p


def _make_symlink_loop(tmp_path: Path) -> Path:
    a = tmp_path / "loop_a"
    b = tmp_path / "loop_b"
    try:
        os.symlink(b, a)
        os.symlink(a, b)
    except OSError:
        pytest.skip("unprivileged os.symlink not permitted on this host")
    return a


def _make_permission_denied(tmp_path: Path) -> Path:
    if _WINDOWS:
        pytest.skip("chmod 000 does not restrict owner-read on Windows")
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        pytest.skip("running as root bypasses chmod 000 (no EACCES to hit)")
    p = tmp_path / "chmod000.yaml"
    p.write_text("sw_fallback: { kconfig: CONFIG_X=y }\n", encoding="utf-8")
    os.chmod(p, 0)
    return p


def _make_directory(tmp_path: Path) -> Path:
    d = tmp_path / "a_directory.yaml"
    d.mkdir()
    return d


def _make_malformed_yaml(tmp_path: Path) -> Path:
    p = tmp_path / "malformed.yaml"
    p.write_text("key: [unterminated\n", encoding="utf-8")
    return p


def _make_missing(tmp_path: Path) -> Path:
    return tmp_path / "does-not-exist.yaml"


def _make_valid(tmp_path: Path) -> Path:
    p = tmp_path / "valid.yaml"
    p.write_text(
        "schema_version: 1\n"
        "sw_fallback:\n"
        "  kconfig: CONFIG_ALP_MYLIB_SW=y\n",
        encoding="utf-8",
    )
    return p


_SHAPES = [
    ("non_utf8_bytes", _make_nonutf8, False),
    ("symlink_loop", _make_symlink_loop, False),
    ("permission_denied", _make_permission_denied, False),
    ("directory_where_file_expected", _make_directory, False),
    ("malformed_yaml", _make_malformed_yaml, False),
    ("missing_file", _make_missing, False),
    ("valid_profile", _make_valid, True),
]


@pytest.mark.parametrize(
    "shape_name, make_path, expect_valid", _SHAPES, ids=[s[0] for s in _SHAPES]
)
def test_never_raises_contract_holds(
    tmp_path, project, shape_name, make_path, expect_valid
) -> None:
    """`_emit_extra_library_profile` returns a `list[str]` for every
    shape -- never an unhandled exception."""
    path = make_path(tmp_path)

    try:
        result = _emit_extra_library_profile("thelib", str(path), project)
    except Exception as exc:  # noqa: BLE001 -- the exact thing under test
        pytest.fail(
            f"{shape_name}: _emit_extra_library_profile raised "
            f"{type(exc).__name__}: {exc}"
        )

    assert isinstance(result, list)
    assert all(isinstance(line, str) for line in result)

    if expect_valid:
        assert any("CONFIG_ALP_MYLIB_SW=y" in line for line in result)
    else:
        assert len(result) == 1
        assert result[0].startswith("# extra_libraries[thelib] profile parse failed:")


# ---------------------------------------------------------------------
# Deterministic symlink-loop reproduction (mutation-proof #2).
#
# `_make_symlink_loop` above needs unprivileged `os.symlink`, which
# Windows refuses outside Developer Mode/admin (confirmed on this
# host: `OSError: [WinError 1314] A required privilege is not held`)
# -- it skips there instead of failing.  This test reproduces the same
# defect deterministically on every platform by monkeypatching
# `pathlib.Path.resolve` to raise the exact `RuntimeError` CPython's
# real ELOOP path raises (`Lib/pathlib.py`'s `check_eloop`), so the
# "is `.resolve()` inside the try" structural fix is always exercised,
# not just where the OS happens to grant symlink privileges.
# ---------------------------------------------------------------------

def test_symlink_loop_deterministic(project, monkeypatch) -> None:
    def _raise_eloop(self, strict=False):
        raise RuntimeError(f"Symlink loop from {str(self)!r}")

    monkeypatch.setattr(Path, "resolve", _raise_eloop)

    result = _emit_extra_library_profile("thelib", "somewhere.yaml", project)

    assert isinstance(result, list)
    assert len(result) == 1
    assert result[0].startswith("# extra_libraries[thelib] profile parse failed:")
    assert "Symlink loop" in result[0]

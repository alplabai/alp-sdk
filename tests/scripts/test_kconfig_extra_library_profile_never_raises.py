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

The fix drops `.resolve()` entirely (nothing downstream of it needs a
canonicalized path -- only `read_text` does), which removes the
`RuntimeError` failure mode at the source instead of widening the
`except` tuple to catch it; a real symlink loop still fails, but via
`read_text`'s own `OSError` (`[Errno 40] Too many levels of symbolic
links`), already inside the original `except (OSError, ...)`.  This
also reconverges this function with tan-cli's relocated copy
(`python/tan/planner/kconfig.py`), which made the same call.

A *board.yaml load-time* symlink loop / permission-denied `profile:`
is a separate, actually-reachable defect on the same field:
`scripts/alp_orchestrate/validate.py`'s `_validate_consistency` (run
by every `load_board_yaml`, i.e. every `--emit` mode including the
`--emit build-plan` tan-cli's planner fallback runs) does its own
`(REPO / prof).resolve()` + `.is_file()` on the identical
user-supplied path, BEFORE this module's emitter is ever reached --
so leaving that call unguarded left the CLI path this issue's
reachability argument rests on still crashing with an unhandled
`RuntimeError`/`PermissionError`.  That call is now wrapped too and
raises a normal `OrchestratorError` (the cli's usual exit-1 path)
instead.

This module drives the eight shapes tan-cli's sibling gate
(`python/tan/planner/kconfig.py`'s `test_never_raises_contract_holds.py`,
tan-cli#1122, seven shapes) plus `parent_is_a_file` from issue #1961's
acceptance list, table-driven, plus a deterministic reproduction of
the symlink-loop failure mode and an end-to-end check that the
reachable shape survives the full `load_board_yaml` ->
`_slice_alp_conf` path, not just a direct call into the private
function.  Symlink loops and `chmod 000` permission denial are
POSIX-only (Windows has no unprivileged `os.symlink` and `chmod`
doesn't restrict owner-read) -- those two shapes skip cleanly on `nt`
rather than fail.

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

from alp_orchestrate import _slice_alp_conf, load_board_yaml  # noqa: E402
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


def _make_parent_is_file(tmp_path: Path) -> Path:
    """Issue #1961's acceptance list also names "a parent that is a
    file": a `profile:` path whose parent component is itself a
    regular file, not a directory.  `read_text()` on the child raises
    `NotADirectoryError` on POSIX / `FileNotFoundError` on Windows --
    both `OSError` subclasses, so either way this is an existing
    catch, not a new failure mode; it was just missing from this
    table."""
    parent = tmp_path / "not_a_directory.yaml"
    parent.write_text("sw_fallback: { kconfig: CONFIG_X=y }\n",
                       encoding="utf-8")
    return parent / "child.yaml"


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
    ("parent_is_a_file", _make_parent_is_file, False),
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
# -- it skips there instead of failing.  This test reproduces the
# ORIGINAL #1961 crash deterministically on every platform by
# monkeypatching `pathlib.Path.resolve` -- the call that actually sat
# outside the pre-fix `try`, not `read_text` -- to raise the exact
# `RuntimeError` CPython's `Path.resolve(strict=False)` raises
# detecting an ELOOP cycle itself on POSIX ("Symlink loop from ...").
#
# Patching `read_text` instead (an earlier draft of this test did)
# proves NOTHING: `except (OSError, ...)` already covered `read_text`
# raising `OSError` on the ORIGINAL, pre-#1961 dev baseline too, so
# that version of this test passed unchanged on the exact code #1961
# reports as broken -- confirmed empirically two ways, a parent-commit
# overlay and an isolated `kconfig.py` revert, both leaving the old
# test green (round-12b review finding #5). `.resolve()` is the call
# that actually raised unhandled; patch that one.
#
# The current source drops `.resolve()` entirely (matching tan-cli's
# shape), so patching it here is inert against today's code -- the
# assertions below pass because `read_text()` on a nonexistent path
# raises its own (already-caught) `OSError`. The mutation proof is
# that reverting `kconfig.py` to the pre-#1961 shape (`.resolve()`
# outside the `try`, `except (OSError, yaml.YAMLError)` only) turns
# this test RED: the patched `.resolve()` then raises `RuntimeError`
# unhandled, before the `try` is ever entered.
# ---------------------------------------------------------------------

def test_symlink_loop_deterministic(project, monkeypatch) -> None:
    def _raise_symlink_loop(self, strict=False):
        raise RuntimeError(f"Symlink loop from {self!r}")

    monkeypatch.setattr(Path, "resolve", _raise_symlink_loop)

    result = _emit_extra_library_profile("thelib", "somewhere.yaml", project)

    assert isinstance(result, list)
    assert len(result) == 1
    assert result[0].startswith("# extra_libraries[thelib] profile parse failed:")


# ---------------------------------------------------------------------
# End-to-end: the one shape that survives board.yaml load-time
# validation (`validate.py`'s `_validate_consistency` rejects
# missing_file / directory_where_file_expected / parent_is_a_file /
# symlink_loop / permission_denied before this emitter is ever
# reached -- a real `.is_file()` check can't see non-UTF-8 content or
# malformed YAML) must still resolve to the diagnostic comment when
# driven through the REAL `load_board_yaml` -> `_slice_alp_conf` path,
# not just a direct call into the private function every other case
# in this module uses.
# ---------------------------------------------------------------------

def test_non_utf8_bytes_end_to_end(tmp_path: Path) -> None:
    profile = tmp_path / "nonutf8-hw-backends.yaml"
    profile.write_bytes(b"\xff\xfe\x00bad")
    # `(REPO / prof)` with an absolute `prof` yields `prof` unchanged
    # (pathlib drops the left operand for an absolute right operand),
    # so an absolute tmp_path profile stands in for a repo-relative
    # one without writing into the real repo tree.
    body = (
        "som:\n"
        "  sku: E1M-V2N101\n"
        "\n"
        "cores:\n"
        "  m33_sm:\n"
        "    os: zephyr\n"
        "    app: ./m33\n"
        "    extra_libraries:\n"
        "      - name: badbytes\n"
        f"        profile: {profile}\n"
    )
    path = _write_board(tmp_path, body, name="e2e-board.yaml")
    board_project = load_board_yaml(path)
    conf = _slice_alp_conf(board_project, board_project.cores["m33_sm"])
    assert "# extra_libraries[badbytes] profile parse failed:" in conf

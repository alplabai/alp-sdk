# SPDX-License-Identifier: Apache-2.0
"""`cmake/alp.cmake` refuses to configure without a `tan` that can emit.

The helper has exactly one emitter and no fallback behind it, so its refusal
IS a customer-facing surface: a SoM customer who has just cloned the SDK and
run `west build` on an example meets this message and nothing else.

CI proves only the HAPPY path.  `.github/actions/install-tan` puts a good
`tan` on PATH before every job that configures an example, which is exactly
what makes both refusal branches -- no `tan` at all, and a `tan` that is
present but too old -- invisible to every gate.  This suite is their only
coverage.

`cmake -P` is enough to reach them: the emitter probe runs in alp.cmake's
file body, before either entry point is called, so `include()`ing the module
in script mode with a controlled PATH exercises it with no Zephyr workspace
and no board.yaml.

POSIX-only.  The too-old case needs an executable `tan` on PATH and a
`#!/bin/sh` stub is the smallest one; the logic under test has no
platform-specific branch, so one OS covers it.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ALP_CMAKE = REPO / "cmake" / "alp.cmake"
INSTALL_TAN_ACTION = REPO / ".github" / "actions" / "install-tan" / "action.yml"

# Resolved once, and invoked by absolute path below: the runs narrow PATH to a
# single directory, so `cmake` itself would not be findable on it.
CMAKE = shutil.which("cmake")

pytestmark = [
    pytest.mark.skipif(os.name == "nt", reason="POSIX `tan` stub on PATH"),
    pytest.mark.skipif(CMAKE is None, reason="cmake not installed"),
]


def _stub_tan(bindir: Path, *, help_lines: list[str]) -> Path:
    """An executable `tan` in *bindir* whose `generate --help` prints
    *help_lines* and exits 0 -- the only behaviour the probe inspects.

    Built from `echo` alone, deliberately: the runs below narrow PATH to
    *bindir*, so a stub that shelled out to `cat` (or any other external)
    would exit 127 and be indistinguishable from a too-old `tan`.  `echo` is
    a shell builtin, so it survives an empty PATH.
    """
    bindir.mkdir(parents=True, exist_ok=True)
    tan = bindir / "tan"
    body = "".join(f'echo "{line}"\n' for line in help_lines)
    tan.write_text(f"#!/bin/sh\n{body}", encoding="utf-8")
    tan.chmod(0o755)
    return tan


def _flat(text: str) -> str:
    """Collapse whitespace: CMake re-wraps a `message()` body at its own
    width, so a literal assertion on the message would otherwise fail
    whenever a tmp path shifts where the line breaks land."""
    return " ".join(text.split())


def _include_alp_cmake(tmp_path: Path, bindir: Path) -> subprocess.CompletedProcess:
    """Run `cmake -P` over a script that does nothing but include the helper,
    with PATH narrowed to *bindir*."""
    drive = tmp_path / "drive.cmake"
    drive.write_text(
        f'include("{ALP_CMAKE.as_posix()}")\n'
        'message(STATUS "probe resolved: ${ALP_SDK_TAN_PROGRAM}")\n',
        encoding="utf-8",
    )
    env = dict(os.environ, PATH=str(bindir))
    return subprocess.run(
        [CMAKE, "-P", str(drive)],
        capture_output=True, text=True, env=env, cwd=tmp_path,
    )


def _pinned_tan_ref() -> str:
    """The 40-char tan-cli commit `.github/actions/install-tan` pins.

    Read rather than hardcoded: alp.cmake's error message quotes the same
    install line, and this is what stops the two copies drifting apart.
    """
    match = re.search(r"tan-cli@([0-9a-f]{40})", INSTALL_TAN_ACTION.read_text(encoding="utf-8"))
    assert match, f"no pinned tan-cli commit found in {INSTALL_TAN_ACTION}"
    return match.group(1)


def _assert_actionable(stderr: str) -> None:
    """Whatever the reason, the refusal has to be something a customer can
    act on: the capability needed, and the exact install command."""
    flat = _flat(stderr)
    assert "--output" in flat
    assert "pip install" in flat
    assert _pinned_tan_ref() in flat, \
        "alp.cmake's install line has drifted from .github/actions/install-tan"


def test_no_tan_on_path_is_a_fatal_error(tmp_path):
    """The clean-clone case."""
    empty = tmp_path / "empty"
    empty.mkdir()
    proc = _include_alp_cmake(tmp_path, empty)
    if proc.returncode == 0:
        pytest.skip("this host has a `tan` in a CMake system path; "
                    "the absent-tan branch is unreachable here")
    assert "no `tan` was found on PATH" in _flat(proc.stderr)
    _assert_actionable(proc.stderr)


def test_tan_without_output_is_rejected_as_too_old(tmp_path):
    """Present but too old must land on the SAME actionable message, not on
    tan's own mid-configure argument error.

    Modelled on the real released binary: `tan` v0.4.1 accepts `generate
    --target/--core/--board-yaml/--sdk-root` and NOT `--output`, so probing
    `--output` is what separates a usable `tan` from a stale one.
    """
    tan = _stub_tan(tmp_path / "old", help_lines=[
        "Usage: tan generate [OPTIONS]",
        "      --target <EMIT>",
        "      --core <CORE_ID>",
        "      --board-yaml <PATH>",
        "      --sdk-root <PATH>",
    ])
    proc = _include_alp_cmake(tmp_path, tan.parent)
    assert proc.returncode != 0
    flat = _flat(proc.stderr)
    assert "is too old" in flat
    assert str(tan) in flat, "the message must name the tan it rejected"
    _assert_actionable(proc.stderr)


def test_tan_with_output_satisfies_the_probe(tmp_path):
    """The counter-case that keeps the two above honest: a `tan` offering
    `--output` is accepted, and the path the emits will shell is the one that
    was probed."""
    tan = _stub_tan(tmp_path / "new", help_lines=[
        "Usage: tan generate [OPTIONS]",
        "      --target <EMIT>",
        "      --output <PATH>",
    ])
    proc = _include_alp_cmake(tmp_path, tan.parent)
    assert proc.returncode == 0, proc.stderr
    assert f"probe resolved: {tan}" in proc.stdout

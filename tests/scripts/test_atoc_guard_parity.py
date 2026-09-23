# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2262 parity: `scripts/aen_atoc.py`'s Python ATOC-replace-guard
functions (`compute_query_status`, `foreign_resident_entries`,
`decide_atoc_guard`) are a PORT of `bench_atoc_replace_guard` in
`scripts/bench/aen/bench-env.sh` (alp-sdk#2025), not an independent
reimplementation -- bench-env.sh itself is untouched by #2262 (still bash,
still what runs on the bench today). This file feeds the SAME fixture
transcripts (the real, bench-verified 2026-09-07 AEN EVK captures already
pinned by tests/scripts/test_bench_jlink_connect_guard.py) through BOTH
implementations and asserts identical verdicts, so the two copies cannot
silently drift apart.

The bash side only ever returns 0 (proceed) or 5 (abort) -- the Python side
is more granular (clear/empty/refused-foreign/refused-unverified/replaced).
Parity here means: bash exit 0 <=> a non-refused Python verdict, bash exit 5
<=> a refused Python verdict, and (where bash names foreign entries in its
abort message) the identical entry-name set.

Reuses the exact stub-`maintenance`-script technique and skip guard from
test_bench_jlink_connect_guard.py's `_call_atoc_guard` (see that module for
the full rationale of each choice -- run-unique transcript paths, the
banner-then-gettoc ordering, `cd "$SETOOLS_DIR" && ./maintenance`, etc.);
this file duplicates only the minimum needed to drive
`bench_atoc_replace_guard` standalone, since it is testing PARITY, not the
bash guard's own internal edge cases (those stay owned by
test_bench_jlink_connect_guard.py).
"""
from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"


@pytest.fixture(scope="module")
def aen_atoc():
    spec = importlib.util.spec_from_file_location("aen_atoc", REPO / "scripts" / "aen_atoc.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["aen_atoc"] = mod
    spec.loader.exec_module(mod)
    return mod


def _sanitized_env() -> dict[str, str]:
    """Same rationale as test_bench_jlink_connect_guard.py's own helper of
    this name: never let a sourced bench-env.sh reach real bench
    infrastructure via an operator's own exported LG_* vars."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(var, None)
    return env


def _bash_can_run_a_script() -> bool:
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
    reason="no working `bash` on this host -- parity is checked on the Python "
           "side elsewhere; this file's own assertions require the bash leg too.",
)


def _run_bash_guard(
    tmp_path: Path,
    replace_atoc: str,
    allowed: list[str],
    gettoc_output: str | None,
    setools_has_maintenance: bool = True,
    gettoc_exit: int = 0,
    banner_output: str | None = None,
    banner_exit: int = 0,
) -> subprocess.CompletedProcess[str]:
    """Drive `bench_atoc_replace_guard` (bench-env.sh) against a synthetic
    `maintenance` stub -- the same fake-maintenance shape as
    test_bench_jlink_connect_guard.py's `_call_atoc_guard`, trimmed to only
    what this parity check needs (SE_UART is always set; TMPDIR is always
    tmp_path)."""
    workdir = tmp_path
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())

    setools_dir = workdir / "setools"
    setools_dir.mkdir(exist_ok=True)
    if setools_has_maintenance:
        maint = setools_dir / "maintenance"
        maint.write_text(
            '#!/usr/bin/env bash\n'
            'opt=""\n'
            'prev=""\n'
            'for a in "$@"; do\n'
            '\t[ "$prev" = "-opt" ] && opt="$a"\n'
            '\tprev="$a"\n'
            'done\n'
            'if [ "$opt" = "getbanner" ]; then\n'
            '\tcat "$BANNER_STUB_FILE"\n'
            '\texit "${BANNER_STUB_EXIT:-0}"\n'
            'fi\n'
            'cat "$ATOC_STUB_FILE"\n'
            'exit "${GETTOC_STUB_EXIT:-0}"\n',
            encoding="utf-8",
        )
        maint.chmod(0o755)

    stub = workdir / "gettoc.out"
    stub.write_text(gettoc_output or "", encoding="utf-8")
    banner_stub = workdir / "banner.out"
    banner_stub.write_text(
        banner_output if banner_output is not None else _REAL_GETBANNER, encoding="utf-8"
    )

    gate = workdir / "gate.sh"
    gate.write_bytes(
        (
            "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
            f'export TMPDIR="{workdir}"\n'
            f'export SETOOLS_DIR="{setools_dir.name}"\n'
            'export SE_UART="fake-uart"\n'
            "source ./bench-env.sh\n"
            f'export ATOC_STUB_FILE="{stub}"\n'
            f'export BANNER_STUB_FILE="{banner_stub}"\n'
            f'export GETTOC_STUB_EXIT="{gettoc_exit}"\n'
            f'export BANNER_STUB_EXIT="{banner_exit}"\n'
            f'bench_atoc_replace_guard "{replace_atoc}" parity-test {" ".join(allowed)}\n'
            "exit $?\n"
        ).encode("utf-8")
    )
    return subprocess.run(
        ["bash", "gate.sh"], cwd=workdir, env=_sanitized_env(), capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


# --- fixtures: byte-identical to test_bench_jlink_connect_guard.py's own,
# real captures off an AEN EVK bench unit (2026-09-07) -- see that module
# for full provenance notes. Duplicated here (not imported) so this parity
# file has no import-time dependency on another test module's internals.

_REAL_MULTI_ENTRY_ATOC = """\
|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |      312 |  0.5.0| u V  |
|   DEVICE |  CM0+  | 0x805C1EC0 | 0x805C14C0 | ---------- | ---------- |      372 |  0.5.0| u V  |
| BOOTLOAD | A32_0  | 0x80002000 | 0x8057A8F0 | ---------- | 0x80002000 |    28813 |  0.4.3| u VB |
|  A32_APP | A32_0  | 0x80020000 | 0x8057B2F0 | ---------- | ---------- |  2290048 |  1.0.0| u V  |
|   HP_APP | M55-HP | 0x8057D230 | 0x8057C830 | 0x50000000 | 0x50000000 |     4480 |  1.0.0| uLVB |
|   HE_APP | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |     4480 |  1.0.0| uLVB |
"""

_NO_ATOC = "No ATOC found on target device.\n"

_REAL_GETBANNER = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m SES A1 v1.110.0 Mar  4 2026 19:06:23 \x1b[0m\n"
    " \n"
    "\x1b[?25h\n"
    "\x1b[0m\n"
)

_REAL_GETTOC_9ROW = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |   Name   |  CPU   | Store Addr |  Obj Addr  | Dest Addr  | Boot Addr  |   Size   |  Version  |  Flags | Time (ms)|\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x8057c6f0 | 0x8057BCF0 | ---------- | ---------- |      312 |      0.5.0|u V     |    14.92 |\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x805c1ec0 | 0x805C14C0 | ---------- | ---------- |      372 |      0.5.0|u V     |    15.04 |\n"
    "\x1b[0m\x1b[94m |  * SERAM0|   CM0+ | ---------- | 0x000000C0 | ---------- | ---------- |    64508 |    1.110.0|u s     |     0.00 |\n"
    "\x1b[0m\x1b[94m |    SERAM1|   CM0+ | ---------- | 0x00020AC0 | ---------- | ---------- |    64508 |    1.110.0|------- |     0.00 |\n"
    "\x1b[0m\x1b[94m |  BOOTLOAD| A32_0  | 0x80002000 | 0x8057A8F0 | ---------- | 0x80002000 |    28816 |      0.4.3|u VB    |    16.89 |\n"
    "\x1b[0m\x1b[94m |   A32_APP| A32_0  | 0x80020000 | 0x8057B2F0 | ---------- | ---------- |  2290048 |      1.0.0|u V     |   189.01 |\n"
    "\x1b[0m\x1b[94m |    HP_APP| M55-HP | 0x8057d230 | 0x8057C830 | 0x50000000 | 0x50000000 |     4480 |      1.0.0|uLVB    |    15.90 |\n"
    "\x1b[0m\x1b[94m |    HE_APP| M55-HE | 0x8057edb0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |     4480 |      1.0.0|uLVB    |    15.55 |\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[?25h\n"
    "\x1b[0m\n"
)

_REAL_GETTOC_CLEAN = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |   Name   |  CPU   | Store Addr |  Obj Addr  | Dest Addr  | Boot Addr  |   Size   |  Version  |  Flags | Time (ms)|\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x80564530 | 0x80563B30 | ---------- | ---------- |      312 |      0.5.0|u V     |    14.92 |\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x805c1ec0 | 0x805C14C0 | ---------- | ---------- |      372 |      0.5.0|u V     |    15.04 |\n"
    "\x1b[0m\x1b[94m |  * SERAM0|   CM0+ | ---------- | 0x000000C0 | ---------- | ---------- |    64508 |    1.110.0|u s     |     0.00 |\n"
    "\x1b[0m\x1b[94m |    SERAM1|   CM0+ | ---------- | 0x00020AC0 | ---------- | ---------- |    64508 |    1.110.0|------- |     0.00 |\n"
    "\x1b[0m\x1b[94m |    ALP-HE| M55-HE | 0x80565070 | 0x80564670 | 0x58000000 | 0x58000000 |   110356 |      1.0.0|uLVB    |    29.02 |\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[?25h\n"
    "\x1b[0m\n"
)

_ANSI_COMPLIANT_TABLE = (
    "|   \x1b[32mDEVICE\x1b[0m |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- |"
    " ---------- |      312 |  0.5.0| u V  |\n"
    "|   \x1b[32mALP-HE\x1b[0m | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 |"
    " 0x58000000 |     4480 |  1.0.0| uLVB |\n"
)

_CRLF_ONLY_ALLOWED_ATOC = (
    "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
    "      312 |  0.5.0| u V  |\r\n"
    "|   ALP-HE | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |"
    "     4480 |  1.0.0| uLVB |\r\n"
)


def _python_verdict(aen_atoc, banner_text, banner_rc, gettoc_text, gettoc_rc,
                     allowed, replace_atoc, maintenance_available=True):
    query_status = aen_atoc.compute_query_status(
        maintenance_available, banner_text, banner_rc, gettoc_text, gettoc_rc)
    resident = aen_atoc.parse_resident_atoc_table(gettoc_text or "")
    foreign = aen_atoc.foreign_resident_entries(resident, allowed)
    return aen_atoc.decide_atoc_guard(query_status, foreign, replace_atoc == "1")


@_NEEDS_BASH
def test_parity_clean_board_proceeds(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _REAL_GETTOC_CLEAN)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _REAL_GETTOC_CLEAN, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused
    assert py.status == "clear"


@_NEEDS_BASH
def test_parity_foreign_entries_abort(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _REAL_GETTOC_9ROW)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _REAL_GETTOC_9ROW, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    for entry in ("BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"):
        assert entry in bash.stderr
        assert entry in py.foreign
    # baseline SE state must never appear in either side's foreign set
    for baseline in ("DEVICE", "SERAM0", "SERAM1", "* SERAM0"):
        assert baseline not in py.foreign


@_NEEDS_BASH
def test_parity_no_atoc_proceeds(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _NO_ATOC)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _NO_ATOC, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused
    assert py.status == "empty"


@_NEEDS_BASH
def test_parity_replace_atoc_overrides_foreign_entries(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "1", ["ALP-HE"], _REAL_GETTOC_9ROW)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _REAL_GETTOC_9ROW, 0, ["ALP-HE"], "1")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused
    assert py.status == "replaced"


@_NEEDS_BASH
def test_parity_missing_maintenance_binary_is_unverified(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], None, setools_has_maintenance=False)
    py = _python_verdict(
        aen_atoc, None, None, None, None, ["ALP-HE"], "0", maintenance_available=False)
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
def test_parity_gettoc_failure_after_partial_output_is_unverified(tmp_path, aen_atoc):
    partial = (
        "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
        "      312 |  0.5.0| u V  |\n"
        "|   DEVICE |  CM0+  | 0x805C1EC0 | 0x805C14C0 | ---------- | ---------- |"
        "      372 |  0.5.0| u V  |\n"
        "ERROR: Target did not respond\n"
    )
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], partial, gettoc_exit=1)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, partial, 1, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
def test_parity_ansi_coloured_compliant_table_proceeds(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _ANSI_COMPLIANT_TABLE)
    py = _python_verdict(
        aen_atoc, _REAL_GETBANNER, 0, _ANSI_COMPLIANT_TABLE, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused and py.status == "clear"


@_NEEDS_BASH
def test_parity_crlf_table_proceeds(tmp_path, aen_atoc):
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _CRLF_ONLY_ALLOWED_ATOC)
    py = _python_verdict(
        aen_atoc, _REAL_GETBANNER, 0, _CRLF_ONLY_ALLOWED_ATOC, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused and py.status == "clear"


@_NEEDS_BASH
def test_parity_dualcore_own_write_allows_both_entries(tmp_path, aen_atoc):
    """flash-run-dualcore.sh's own legitimate two-entry write (ALP-HP +
    ALP-HE) must not trip either guard on itself."""
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HP", "ALP-HE"], _REAL_MULTI_ENTRY_ATOC)
    py = _python_verdict(
        aen_atoc, _REAL_GETBANNER, 0, _REAL_MULTI_ENTRY_ATOC, 0, ["ALP-HP", "ALP-HE"], "0")
    # HP_APP/HE_APP are resident under different names than the ALP-HP/ALP-HE
    # this run would write -- still genuinely foreign on both sides.
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    assert "HP_APP" in py.foreign and "HE_APP" in py.foreign

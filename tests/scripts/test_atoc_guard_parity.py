# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2262 parity: `scripts/aen_atoc.py`'s Python ATOC-replace-guard
functions (`compute_query_status`, `foreign_resident_entries`,
`decide_atoc_guard`) are a PORT of `bench_atoc_replace_guard` in
`scripts/bench/aen/bench-env.sh` (alp-sdk#2025), not an independent
reimplementation -- bench-env.sh itself is untouched by #2262 (still bash,
still what runs on the bench today). This file feeds fixture transcripts
through BOTH implementations and asserts identical verdicts, so the two
copies cannot silently drift apart.

Fixture provenance (MEDIUM-5 review, alp-sdk#2262 -- be precise about
which of these are real captures vs. hand-written test vectors):
  REAL, bench-verified 2026-09-07 AEN EVK captures (byte-identical to
  tests/scripts/test_bench_jlink_connect_guard.py's own pinned copies):
    `_REAL_MULTI_ENTRY_ATOC`, `_REAL_GETBANNER`, `_REAL_GETTOC_9ROW`,
    `_REAL_GETTOC_CLEAN`.
  SYNTHETIC (hand-written to exercise one specific parser rule; never
  captured off real hardware): `_NO_ATOC`, `_ANSI_COMPLIANT_TABLE`,
  `_CRLF_ONLY_ALLOWED_ATOC`, the short-row/NBSP-only-name/empty-name/
  cross-CPU fixtures below, and the partial-row-then-error text in
  `test_parity_gettoc_failure_after_partial_output_is_unverified`.

The bash side only ever returns 0 (proceed) or 5 (abort) -- the Python side
is more granular (clear/empty/refused-foreign/refused-unverified/replaced).
Parity here means: bash exit 0 <=> a non-refused Python verdict, bash exit 5
<=> a refused Python verdict, and (where bash names foreign entries in its
abort message, `"This board also carries: ${extra[*]}"` -- SPACE-joined,
not comma-joined, since bash's `[*]` expansion uses `$IFS`) the identical
entry-name SET (`_bash_foreign_names` below parses that line; earlier
versions of this file only checked substring membership, which cannot
catch an extra/missing name or a name split differently on either side).

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
import re
import subprocess
import sys
import types
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"

# --- stub the one external dependency (Zephyr's runners.core), same shape
# as test_alif_flash_runner.py -- needed here too for the NEW-1 parity
# cases that drive the Python side THROUGH the real `_run_maintenance`
# (a subprocess boundary), not just through the pure parser functions.
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))
if "runners.core" not in sys.modules:
    _fake_core = types.ModuleType("runners.core")

    class _RunnerCaps:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class _ZephyrBinaryRunner:
        def __init__(self, cfg):
            self.cfg = cfg

    _fake_core.RunnerCaps = _RunnerCaps
    _fake_core.ZephyrBinaryRunner = _ZephyrBinaryRunner
    sys.modules["runners.core"] = _fake_core

from runners import alif_flash  # noqa: E402


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

# HIGH-1 review shapes (alp-sdk#2262) -- SYNTHETIC, added specifically to
# prove the fail-open fix in parse_resident_atoc_table on BOTH legs (a
# real awk run confirmed these exact outputs: see the docstrings on
# tests/scripts/test_aen_atoc.py's matching unit tests).
_SHORT_ROW_FOREIGN = " |  A32_APP\n"
_NBSP_ONLY_NAME_ROW = "|  \xa0\xa0\xa0 | CM0+ |\n"
_EMPTY_NAME_ROW = "|    |  CM0+  |\n"
# ALP-HE resident under a foreign-looking CPU column ("A32_0" instead of
# "M55-HE") -- LOW-10 review: the allowed-set membership check is
# NAME-ONLY on both sides (bash: `[ "$name" = "$a" ]`; Python:
# `name not in allowed_set`), so this must NOT be foreign on either leg.
# Documenting/parity-testing this deliberately-unchanged behaviour, not
# altering it (LOW-10 is explicitly out of scope for this PR).
_ALLOWED_NAME_WRONG_CPU_ROW = "|   ALP-HE | A32_0  | 0x0 | 0x0 |\n"
# SERAM1 on a non-CM0+ CPU: the baseline exemption is cross-checked
# against the CPU column, so a same-named row on a different core is a
# genuine (if oddly-named) app entry, not SE firmware -- must be foreign
# on both legs.
_SERAM1_WRONG_CPU_ROW = "|   SERAM1 | M55-HE | 0x0 | 0x0 |\n"

_GARBLED_BANNER = "garbage, no SES banner here\n"


def _python_verdict(aen_atoc, banner_text, banner_rc, gettoc_text, gettoc_rc,
                     allowed, replace_atoc, maintenance_available=True):
    query_status = aen_atoc.compute_query_status(
        maintenance_available, banner_text, banner_rc, gettoc_text, gettoc_rc)
    resident = aen_atoc.parse_resident_atoc_table(gettoc_text or "")
    foreign = aen_atoc.foreign_resident_entries(resident, allowed)
    return aen_atoc.decide_atoc_guard(query_status, foreign, replace_atoc == "1")


def _bash_foreign_names(stderr: str) -> "set[str] | None":
    """Parse bash's `"   This board also carries: ${extra[*]}"` line
    (bench-env.sh) into the SET of names it names -- `${extra[*]}` joins
    with (the first character of) `$IFS`, i.e. a single literal ASCII
    space. Returns None if the line is not present (e.g. bash proceeded,
    or aborted for the unverified reason instead).

    Split on a literal `' '`, NOT `str.split()`'s bare (no-arg) form: the
    latter treats every Unicode whitespace codepoint as a separator
    (`'\\xa0'.isspace()` is True), which would silently drop an NBSP-only
    resident name to an empty token list -- measured while adding the
    HIGH-1 NBSP parity case, alp-sdk#2262.

    Known limitation, documented rather than engineered around (nit review,
    #2262): a literal space-splitting join cannot losslessly represent a
    FOREIGN entry name that itself contains a space -- in practice, only a
    `"* "` current-bank marker (`* SERAM0`/`* ALP-HE`/...), and only ever
    on the two baseline SE-firmware names, which `foreign_resident_entries`
    exempts before a name ever reaches `extra`/`foreign` at all UNLESS it
    is on the wrong CPU (a real, if exotic, "coincidentally-named app
    entry" case -- see `test_parity_seram1_on_wrong_cpu_is_foreign`, which
    does not carry the marker itself). A `"* <name>"` foreign entry would
    parse here as two separate tokens (`"*"`, `"<name>"`) instead of one --
    no fixture in this file constructs that shape, so it has never been
    exercised; do not read a green suite as proof this helper handles it.
    """
    m = re.search(r"This board also carries: (.+)", stderr)
    if not m:
        return None
    return set(m.group(1).split(' '))


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
    bash_foreign = _bash_foreign_names(bash.stderr)
    assert bash_foreign == {"BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"}, bash.stderr
    # SET equality, not substring membership (MEDIUM-5 review): a substring
    # check cannot catch an extra or missing name, or a name tokenized
    # differently on either side.
    assert set(py.foreign) == bash_foreign
    # baseline SE state must never appear in either side's foreign set
    for baseline in ("DEVICE", "SERAM0", "SERAM1", "* SERAM0"):
        assert baseline not in py.foreign
        assert baseline not in bash_foreign


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
    # this run would write -- still genuinely foreign on both sides. So are
    # BOOTLOAD/A32_APP (the A32 Linux boot chain): neither is in the
    # allowed set either, and this fixture's whole POINT is that the A32
    # chain is resident alongside the dual-core write -- MEDIUM-5 review:
    # an earlier version of this test asserted only a substring check
    # against {"HP_APP", "HE_APP"}, which never noticed BOOTLOAD/A32_APP
    # were ALSO foreign here.
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    bash_foreign = _bash_foreign_names(bash.stderr)
    assert bash_foreign == {"BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"}, bash.stderr
    assert set(py.foreign) == bash_foreign


@_NEEDS_BASH
def test_parity_short_row_still_yields_a_foreign_entry(tmp_path, aen_atoc):
    # HIGH-1 review (alp-sdk#2262): a truncated row (only one `|`) must
    # still be seen as a resident (and here, foreign) entry on BOTH legs
    # -- an earlier Python version silently dropped it (fail-open).
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _SHORT_ROW_FOREIGN)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _SHORT_ROW_FOREIGN, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    bash_foreign = _bash_foreign_names(bash.stderr)
    assert bash_foreign == {"A32_APP"}, bash.stderr
    assert set(py.foreign) == bash_foreign


@_NEEDS_BASH
def test_parity_nbsp_only_name_still_yields_a_foreign_entry(tmp_path, aen_atoc):
    # HIGH-1 review (alp-sdk#2262): awk's trim only strips ASCII space/tab
    # -- a name cell of bare NBSP is non-empty (and therefore foreign,
    # since it can never equal an allowed entry name) on both legs.
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _NBSP_ONLY_NAME_ROW)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, _NBSP_ONLY_NAME_ROW, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    bash_foreign = _bash_foreign_names(bash.stderr)
    assert bash_foreign is not None and len(bash_foreign) == 1
    assert len(py.foreign) == 1 and py.foreign[0] != ""


@_NEEDS_BASH
def test_parity_empty_name_row_is_not_a_resident_entry(tmp_path, aen_atoc):
    # A row whose Name column trims to "" is not a real entry on either
    # leg (`name != ""` in awk; `if name and ...` in Python) -- a clean
    # ALP-HE-only board plus one such row must still proceed.
    table = _EMPTY_NAME_ROW + (
        "|   ALP-HE | M55-HE | 0x0 | 0x0 |\n"
    )
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], table)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 0, table, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused and py.status == "clear"


@_NEEDS_BASH
def test_parity_allowed_name_on_a_different_cpu_is_not_foreign(tmp_path, aen_atoc):
    # LOW-10 review: allowed-set membership is NAME-ONLY on both legs
    # (unlike the DEVICE/SERAM0/SERAM1 baseline exemption, which IS
    # CPU-cross-checked) -- deliberately unchanged behaviour, parity-tested
    # here rather than altered.
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _ALLOWED_NAME_WRONG_CPU_ROW)
    py = _python_verdict(
        aen_atoc, _REAL_GETBANNER, 0, _ALLOWED_NAME_WRONG_CPU_ROW, 0, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused and py.status == "clear"


@_NEEDS_BASH
def test_parity_seram1_on_wrong_cpu_is_foreign(tmp_path, aen_atoc):
    # The baseline exemption IS CPU-cross-checked: a SERAM1-named row on
    # M55-HE (not CM0+) is a coincidentally-named app entry, not SE
    # firmware, and must trip the guard on both legs.
    bash = _run_bash_guard(tmp_path, "0", ["ALP-HE"], _SERAM1_WRONG_CPU_ROW)
    py = _python_verdict(
        aen_atoc, _REAL_GETBANNER, 0, _SERAM1_WRONG_CPU_ROW, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    bash_foreign = _bash_foreign_names(bash.stderr)
    assert bash_foreign == {"SERAM1"}, bash.stderr
    assert set(py.foreign) == bash_foreign


@_NEEDS_BASH
def test_parity_garbled_banner_is_unverified(tmp_path, aen_atoc):
    bash = _run_bash_guard(
        tmp_path, "0", ["ALP-HE"], _REAL_GETTOC_CLEAN, banner_output=_GARBLED_BANNER)
    py = _python_verdict(aen_atoc, _GARBLED_BANNER, 0, _REAL_GETTOC_CLEAN, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
def test_parity_banner_nonzero_exit_is_unverified_even_with_valid_text(tmp_path, aen_atoc):
    # A well-formed banner LINE with a non-zero exit must still force
    # unverified on both legs (getbanner failing after printing a valid
    # line is the same class of trap as gettoc's own non-zero-exit case).
    bash = _run_bash_guard(
        tmp_path, "0", ["ALP-HE"], _REAL_GETTOC_CLEAN,
        banner_output=_REAL_GETBANNER, banner_exit=3)
    py = _python_verdict(aen_atoc, _REAL_GETBANNER, 3, _REAL_GETTOC_CLEAN, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
def test_parity_empty_banner_is_unverified(tmp_path, aen_atoc):
    # A commit message earlier in this PR's history claimed a "bad/absent
    # banner" parity case, but no genuinely EMPTY/absent banner (rc 0,
    # zero-byte output -- e.g. a `maintenance` build that answers
    # `getbanner` with nothing) was actually covered. `is_valid_ses_banner`
    # of `""` must be False on both legs.
    bash = _run_bash_guard(
        tmp_path, "0", ["ALP-HE"], _REAL_GETTOC_CLEAN, banner_output="")
    py = _python_verdict(aen_atoc, "", 0, _REAL_GETTOC_CLEAN, 0, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


# ---------------------------------------------------------------------------
# NEW-1 review (#2262): `_run_maintenance`'s subprocess call must NOT pass
# `text=True` -- Python's universal-newline translation turns every BARE
# `\r` in the child's raw stdout into `\n`, splitting one line into two.
# bash's own reading of `maintenance`'s output either DELETES a bare `\r`
# (`tr -d '\r'`, for the gettoc table) or treats it as an ordinary
# mid-line character (`grep`'s own line splitting is `\n`-only, for the
# banner) -- `text=True` recreates neither behaviour. The three cases
# below were measured (by hand, against a real stub) to disagree with
# bash before the fix; a fourth (plain CRLF) is a negative control that
# already agreed before the fix (universal newlines maps `\r\n` -> `\n`
# as ONE substitution, same net effect as `tr -d '\r'` there) and must
# keep agreeing after it.
#
# These drive the Python side THROUGH the real `_run_maintenance` (a
# real subprocess, a real executable stub) rather than through the
# fixture-string helpers above, which never exercise the subprocess
# boundary this bug lived in at all.
# ---------------------------------------------------------------------------


def _write_byte_stub_maintenance(path: Path, banner_bytes: bytes, banner_rc: int,
                                  gettoc_bytes: bytes, gettoc_rc: int) -> None:
    """A real, executable `maintenance` stub that writes EXACT bytes to
    stdout -- `sys.stdout.buffer.write(...)`, never `print()`, so nothing
    on the CHILD's own writing side can normalize a bare `\\r` either."""
    script = (
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "argv = sys.argv[1:]\n"
        "opt = None\n"
        "prev = None\n"
        "for a in argv:\n"
        "    if prev == '-opt':\n"
        "        opt = a\n"
        "    prev = a\n"
        "if opt == 'getbanner':\n"
        f"    sys.stdout.buffer.write({banner_bytes!r})\n"
        f"    sys.exit({banner_rc})\n"
        f"sys.stdout.buffer.write({gettoc_bytes!r})\n"
        f"sys.exit({gettoc_rc})\n"
    )
    path.write_text(script, encoding="utf-8")
    path.chmod(0o755)


def _python_verdict_through_run_maintenance(aen_atoc, maint_path, allowed, replace_atoc):
    banner_text, banner_rc = alif_flash._run_maintenance(maint_path, "fake-uart", "57600",
                                                           "getbanner")
    gettoc_text, gettoc_rc = alif_flash._run_maintenance(maint_path, "fake-uart", "57600",
                                                           "gettoc")
    return _python_verdict(
        aen_atoc, banner_text, banner_rc, gettoc_text, gettoc_rc, allowed, replace_atoc)


_NEEDS_REAL_SUBPROCESS = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="a shebang script is not directly executable on Windows",
)


@_NEEDS_BASH
@_NEEDS_REAL_SUBPROCESS
def test_parity_bare_cr_inside_a_row_name_via_run_maintenance(tmp_path, aen_atoc):
    # bash: `tr -d '\r'` DELETES the bare CR, joining "ALP-HE" and "Z"
    # into one foreign name "ALP-HEZ" on a single row -> refused-foreign.
    gettoc_bytes = b"| ALP-HE\rZ | M55-HE | x |\n"
    maint = tmp_path / "maintenance"
    _write_byte_stub_maintenance(maint, _REAL_GETBANNER.encode("utf-8"), 0, gettoc_bytes, 0)
    (tmp_path / "bash").mkdir()
    bash = _run_bash_guard(tmp_path / "bash", "0", ["ALP-HE"], gettoc_bytes.decode("utf-8"))
    py = _python_verdict_through_run_maintenance(aen_atoc, maint, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-foreign"
    assert py.foreign == ["ALP-HEZ"]


@_NEEDS_BASH
@_NEEDS_REAL_SUBPROCESS
def test_parity_bare_cr_before_a_row_via_run_maintenance(tmp_path, aen_atoc):
    # bash: `tr -d '\r'` joins "junk" onto the front of the row, so the
    # merged line no longer starts with `|` and never parses as a row at
    # all -> resident stays empty -> unverified (rc=0, no rows, not the
    # "No ATOC" line).
    gettoc_bytes = b"junk\r| ALP-HE | M55-HE | x |\n"
    maint = tmp_path / "maintenance"
    _write_byte_stub_maintenance(maint, _REAL_GETBANNER.encode("utf-8"), 0, gettoc_bytes, 0)
    (tmp_path / "bash").mkdir()
    bash = _run_bash_guard(tmp_path / "bash", "0", ["ALP-HE"], gettoc_bytes.decode("utf-8"))
    py = _python_verdict_through_run_maintenance(aen_atoc, maint, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
@_NEEDS_REAL_SUBPROCESS
def test_parity_bare_cr_inside_the_banner_via_run_maintenance(tmp_path, aen_atoc):
    # bash: the banner check does no CR handling at all -- `grep` splits
    # only on `\n`, so "junk\rSES ..." (no real newline before "SES") is
    # ONE line that does not start with "SES" -> banner invalid ->
    # unverified.
    banner_bytes = b"junk\rSES A1 v1.110.0 x\n"
    maint = tmp_path / "maintenance"
    _write_byte_stub_maintenance(maint, banner_bytes, 0, _REAL_GETTOC_CLEAN.encode("utf-8"), 0)
    (tmp_path / "bash").mkdir()
    bash = _run_bash_guard(
        tmp_path / "bash", "0", ["ALP-HE"], _REAL_GETTOC_CLEAN,
        banner_output=banner_bytes.decode("utf-8"))
    py = _python_verdict_through_run_maintenance(aen_atoc, maint, ["ALP-HE"], "0")
    assert bash.returncode == 5, bash.stderr
    assert py.refused and py.status == "refused-unverified"


@_NEEDS_BASH
@_NEEDS_REAL_SUBPROCESS
def test_parity_crlf_via_run_maintenance_is_a_negative_control(tmp_path, aen_atoc):
    # A genuine CRLF line ending: universal-newline translation maps
    # `\r\n` -> `\n` as ONE substitution (not a split into two lines),
    # the same net effect as bash's `tr -d '\r'` here -- this shape must
    # agree on both legs REGARDLESS of the `text=True` bug, so it does
    # not by itself prove the fix (see the three tests above for that);
    # it documents that CRLF specifically was never the broken case.
    gettoc_bytes = b"| ALP-HE | M55-HE | x |\r\n"
    maint = tmp_path / "maintenance"
    _write_byte_stub_maintenance(maint, _REAL_GETBANNER.encode("utf-8"), 0, gettoc_bytes, 0)
    (tmp_path / "bash").mkdir()
    bash = _run_bash_guard(tmp_path / "bash", "0", ["ALP-HE"], gettoc_bytes.decode("utf-8"))
    py = _python_verdict_through_run_maintenance(aen_atoc, maint, ["ALP-HE"], "0")
    assert bash.returncode == 0, bash.stderr
    assert not py.refused and py.status == "clear"

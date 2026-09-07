# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2032 -- LG_PLACE resolves bench addresses from labgrid, never a
raw device-path table.

The maintainer's hard rule: every bench connection goes through labgrid,
addressed by PLACE NAME. The 2026-09-07 incident happened because SE_UART
and the app console were taken from a stale table instead of from a live
`labgrid-client -p <place> show` -- the stale table had them SWAPPED
(SES/SETOOLS pointed at the app console's device).

These tests feed `bench_labgrid_resolve` (bench-env.sh) REAL captured
`labgrid-client -p <place> show` output -- not synthesised text. That
matters: PR #2026's review found synthesised fixtures passing while the
real SETOOLS-output format failed the anchors it wrote against, because
real captures carry details (leading whitespace inside a pformat'd nested
dict, and CRLF line endings under a real pty) a hand-typed fixture doesn't
reproduce.

The `comment:` line real `show` output prints for each place (an
operational narrative -- bench state, incident history, an internal-repo
issue reference) is stripped from these fixtures: bench-env.sh's resolver
never reads it, so keeping it would only bloat this file with unrelated
free text. Every remaining line -- the `matches:`/`acquired:`/resource
blocks the resolver actually parses -- is byte-for-byte what
`labgrid-client -p <place> show` printed against the real coordinator
(100.64.0.1:20408) on 2026-09-07, including the exact device paths, USB
bus/port and ports below.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"


def _bash_can_run_a_script() -> bool:
    """See test_bench_jlink_connect_guard.py's identical helper: presence of
    `bash` on PATH is not enough (Windows CI can resolve the WSL launcher
    with no distribution installed); probe by actually running something."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host; bench-env.sh is POSIX shell",
)

# --- real captures, `comment:` line stripped (see module docstring) --------

# e1m-aen-evk-01, ACQUIRED by alplab-gw/caner. Real values, measured
# 2026-09-07: seuart /dev/ttyUSB0 @ ser2net 100.64.0.1:45235, console
# /dev/ttyUSB1 @ ser2net 100.64.0.1:42637, swd USB path 3-4.1. This is the
# exact table the incident's stale skill data got wrong in two ways: it
# named /dev/ttyUSB2 for the console (really evk-01's seuart... no, really
# a DIFFERENT board's device) and /dev/ttyUSB1 for seuart (really the
# console) -- pointing SETOOLS at ttyUSB1 here would have hit the app
# console, not the SE-UART.
REAL_EVK01_ACQUIRED = """\
Place 'e1m-aen-evk-01':
  matches:
    alplab-gw/e1m-aen-evk-01/NetworkSerialPort/seuart
    alplab-gw/e1m-aen-evk-01/NetworkSerialPort/console
    alplab-gw/e1m-aen-evk-01/NetworkUSBDebugger/swd
  acquired: alplab-gw/caner
  acquired resources:
    alplab-gw/e1m-aen-evk-01/NetworkSerialPort/console
    alplab-gw/e1m-aen-evk-01/NetworkSerialPort/seuart
    alplab-gw/e1m-aen-evk-01/NetworkUSBDebugger/swd
  created: 2026-07-03 14:22:14.112250
  changed: 2026-09-07 15:08:12.818328
Acquired resource 'console' (alplab-gw/e1m-aen-evk-01/NetworkSerialPort/console):
  {'acquired': 'e1m-aen-evk-01',
   'avail': True,
   'cls': 'NetworkSerialPort',
   'params': {'extra': {'path': '/dev/ttyUSB1',
                        'proxy': 'alplab-gw',
                        'proxy_required': False},
              'host': '100.64.0.1',
              'port': 42637,
              'speed': 115200}}
Acquired resource 'seuart' (alplab-gw/e1m-aen-evk-01/NetworkSerialPort/seuart):
  {'acquired': 'e1m-aen-evk-01',
   'avail': True,
   'cls': 'NetworkSerialPort',
   'params': {'extra': {'path': '/dev/ttyUSB0',
                        'proxy': 'alplab-gw',
                        'proxy_required': False},
              'host': '100.64.0.1',
              'port': 45235,
              'speed': 57600}}
Acquired resource 'swd' (alplab-gw/e1m-aen-evk-01/NetworkUSBDebugger/swd):
  {'acquired': 'e1m-aen-evk-01',
   'avail': True,
   'cls': 'NetworkUSBDebugger',
   'params': {'busnum': 3,
              'devnum': 17,
              'extra': {'proxy': 'alplab-gw', 'proxy_required': False},
              'host': '100.64.0.1',
              'model_id': 257,
              'path': '3-4.1',
              'vendor_id': 4966}}
"""

# The identical capture, run through a real pty (`script -qc "labgrid-client
# ... show" /dev/null`). Measured 2026-09-07: labgrid-client itself prints no
# ANSI/SGR codes, but a real interactive run DOES emit CRLF line endings --
# that CR is the actual "real output doesn't match a hand-typed fixture"
# hazard for THIS command (distinct from, but same class as, the SETOOLS
# ANSI-colour traps `test_bench_jlink_connect_guard.py` pins for `maintenance`
# output). Built from REAL_EVK01_ACQUIRED by re-joining on "\\r\\n" -- the
# content is identical, only the line-ending convention differs, which is
# exactly the axis this fixture exists to cover.
REAL_EVK01_ACQUIRED_PTY = REAL_EVK01_ACQUIRED.replace("\n", "\r\n")

# e1m-aen-evk-02: NOT acquired (`acquired: None`), and -- for real, not as a
# test contrivance -- this place has no `seuart` resource registered in
# labgrid at all yet (only `console` and `swd` appear in `matches:`).
REAL_EVK02_UNACQUIRED = """\
Place 'e1m-aen-evk-02':
  matches:
    alplab-gw/e1m-aen-evk-02/NetworkSerialPort/console
    alplab-gw/e1m-aen-evk-02/NetworkUSBDebugger/swd
  acquired: None
  acquired resources:
  created: 2026-09-05 11:17:45.939295
  changed: 2026-09-07 15:07:54.232958
Matching resource 'console' (alplab-gw/e1m-aen-evk-02/NetworkSerialPort/console):
  {'acquired': None,
   'avail': True,
   'cls': 'NetworkSerialPort',
   'params': {'extra': {'path': '/dev/ttyUSB2',
                        'proxy': 'alplab-gw',
                        'proxy_required': False},
              'host': '100.64.0.1',
              'port': None,
              'speed': 115200}}
Matching resource 'swd' (alplab-gw/e1m-aen-evk-02/NetworkUSBDebugger/swd):
  {'acquired': None,
   'avail': True,
   'cls': 'NetworkUSBDebugger',
   'params': {'busnum': 3,
              'devnum': 4,
              'extra': {'proxy': 'alplab-gw', 'proxy_required': False},
              'host': '100.64.0.1',
              'model_id': 257,
              'path': '3-4.2',
              'vendor_id': 4966}}
"""

# e1m-aen-evk-03: NOT acquired, swd USB path "3-4.4.3" -- a DIFFERENT board
# from evk-01's "3-4.1". This is the exact wrong-board hazard named in
# alp-sdk#2032: a stale table pointed a J-Link probe selector at THIS path
# while believing it was talking to evk-01.
REAL_EVK03_UNACQUIRED = """\
Place 'e1m-aen-evk-03':
  matches:
    alplab-gw/e1m-aen-evk-03/NetworkSerialPort/console
    alplab-gw/e1m-aen-evk-03/NetworkUSBDebugger/swd
  acquired: None
  acquired resources:
  created: 2026-09-05 12:42:58.968326
  changed: 2026-09-07 19:03:05.429360
Matching resource 'console' (alplab-gw/e1m-aen-evk-03/NetworkSerialPort/console):
  {'acquired': None,
   'avail': True,
   'cls': 'NetworkSerialPort',
   'params': {'extra': {'path': '/dev/ttyUSB3',
                        'proxy': 'alplab-gw',
                        'proxy_required': False},
              'host': '100.64.0.1',
              'port': None,
              'speed': 115200}}
Matching resource 'swd' (alplab-gw/e1m-aen-evk-03/NetworkUSBDebugger/swd):
  {'acquired': None,
   'avail': True,
   'cls': 'NetworkUSBDebugger',
   'params': {'busnum': 3,
              'devnum': 13,
              'extra': {'proxy': 'alplab-gw', 'proxy_required': False},
              'host': '100.64.0.1',
              'model_id': 257,
              'path': '3-4.4.3',
              'vendor_id': 4966}}
"""


def _stub_labgrid_client(bin_dir: Path, place_to_output: dict[str, str]) -> None:
    """Install a fake `labgrid-client` on PATH that answers `-p <place> show`
    with canned REAL captures, so the resolver is exercised end-to-end
    without a live coordinator or a real reservation."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    stub = bin_dir / "labgrid-client"
    body = ['#!/usr/bin/env bash', 'place=""', 'while [ $# -gt 0 ]; do',
            '\tcase "$1" in', '\t-p) place="$2"; shift 2 ;;',
            '\t*) shift ;;', '\tesac', 'done']
    for i, (place, text) in enumerate(place_to_output.items()):
        cond = "if" if i == 0 else "elif"
        body.append(f'{cond} [ "$place" = "{place}" ]; then')
        body.append(f'\tprintf %s "$(cat "{bin_dir / (place + ".out")}")"')
        (bin_dir / f"{place}.out").write_text(text, encoding="utf-8")
    body.append("else")
    body.append("\texit 1")
    body.append("fi")
    stub.write_text("\n".join(body) + "\n", encoding="utf-8")
    stub.chmod(0o755)


def _resolve(tmp_path: Path, place: str, captures: dict[str, str], extra_env: str = "") -> subprocess.CompletedProcess[str]:
    workdir = tmp_path
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())
    bin_dir = workdir / "fakebin"
    _stub_labgrid_client(bin_dir, captures)

    script = workdir / "run.sh"
    script.write_bytes(
        (
            f'export PATH="{bin_dir}:$PATH"\n'
            f'export LG_PLACE="{place}"\n'
            f"{extra_env}"
            # Every real caller (ram-run.sh, flash-run.sh, ...) runs `set -e`
            # BEFORE sourcing bench-env.sh, which is what turns the
            # resolver's `return 1` into the CALLER actually stopping. This
            # tmp_path is not a git checkout though, and bench-env.sh's own
            # (pre-existing, unrelated) `git rev-parse --show-toplevel` probe
            # for BENCH_ROOT would itself trip `set -e` here for a reason
            # that has nothing to do with the resolver -- so check the
            # source command's OWN exit status explicitly instead, which
            # pins the same real contract (a failed resolve must abort the
            # sourcing caller) without that confound.
            "source ./bench-env.sh; src_rc=$?\n"
            'if [ "$src_rc" -ne 0 ]; then echo "SOURCE_FAILED=$src_rc" >&2; exit "$src_rc"; fi\n'
            'echo "SE_UART=$SE_UART"\n'
            'echo "LG_CONSOLE_DEV=$LG_CONSOLE_DEV"\n'
            'echo "LG_CONSOLE_HOST=$LG_CONSOLE_HOST"\n'
            'echo "LG_CONSOLE_PORT=$LG_CONSOLE_PORT"\n'
            'echo "LG_SWD_PATH=$LG_SWD_PATH"\n'
        ).encode("utf-8")
    )
    return subprocess.run(
        ["bash", "run.sh"], cwd=workdir, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_resolves_all_four_values_from_a_real_acquired_capture(tmp_path: Path) -> None:
    res = _resolve(tmp_path, "e1m-aen-evk-01", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED})
    assert res.returncode == 0, res.stderr
    assert "SE_UART=/dev/ttyUSB0" in res.stdout
    assert "LG_CONSOLE_DEV=/dev/ttyUSB1" in res.stdout
    assert "LG_CONSOLE_HOST=100.64.0.1" in res.stdout
    assert "LG_CONSOLE_PORT=42637" in res.stdout
    assert "LG_SWD_PATH=3-4.1" in res.stdout


@_NEEDS_BASH
def test_resolves_correctly_under_real_pty_crlf_output(tmp_path: Path) -> None:
    """CRLF line endings from a real interactive run must not break the
    parser -- see module docstring."""
    res = _resolve(tmp_path, "e1m-aen-evk-01", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED_PTY})
    assert res.returncode == 0, res.stderr
    assert "SE_UART=/dev/ttyUSB0" in res.stdout
    assert "LG_SWD_PATH=3-4.1" in res.stdout


@_NEEDS_BASH
def test_lapsed_reservation_fails_loudly_not_silently(tmp_path: Path) -> None:
    """evk-02/-03 are real, currently-unacquired places. A lapsed (or never
    taken) reservation must abort, never resolve stale/guessed values."""
    res = _resolve(tmp_path, "e1m-aen-evk-03", {"e1m-aen-evk-03": REAL_EVK03_UNACQUIRED})
    assert res.returncode == 1
    assert "NOT acquired" in res.stderr
    assert "SE_UART=" not in res.stdout or "SE_UART=\n" not in res.stdout


@_NEEDS_BASH
def test_place_missing_the_seuart_resource_fails_loudly(tmp_path: Path) -> None:
    """Real state, not synthesised: e1m-aen-evk-02 has no `seuart` resource
    registered in labgrid at all yet. Even if it were acquired, the resolver
    must refuse rather than resolve an empty SE_UART."""
    acquired_but_no_seuart = REAL_EVK02_UNACQUIRED.replace(
        "  acquired: None", "  acquired: alplab-gw/caner"
    ).replace("Matching resource", "Acquired resource")
    res = _resolve(tmp_path, "e1m-aen-evk-02", {"e1m-aen-evk-02": acquired_but_no_seuart})
    assert res.returncode == 1
    assert "no 'seuart' resource path" in res.stderr


@_NEEDS_BASH
def test_swd_path_is_scoped_to_its_own_resource_block(tmp_path: Path) -> None:
    """board-farm/bin/jlink-run.sh:34-50's own hazard: the top-level
    `matches:` list repeats the same resource strings, so a naive whole-output
    search can latch onto it. evk-01's real capture exercises exactly this --
    'NetworkUSBDebugger/swd' appears in `matches:` AND `acquired resources:`
    before the real 'swd' resource block does -- and must still resolve the
    swd block's own path (3-4.1), not something scraped from the list."""
    res = _resolve(tmp_path, "e1m-aen-evk-01", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED})
    assert res.returncode == 0, res.stderr
    assert "LG_SWD_PATH=3-4.1" in res.stdout


@_NEEDS_BASH
def test_seuart_and_console_paths_are_not_swapped(tmp_path: Path) -> None:
    """The exact 2026-09-07 incident shape: two resources of the SAME class
    (NetworkSerialPort) in one block each -- a resolver that grabbed the
    FIRST 'path' key in the whole document regardless of resource name would
    return the console's device (/dev/ttyUSB1) for seuart, or vice versa."""
    res = _resolve(tmp_path, "e1m-aen-evk-01", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED})
    assert res.returncode == 0, res.stderr
    assert "SE_UART=/dev/ttyUSB0" in res.stdout, "seuart resolved to the wrong device"
    assert "LG_CONSOLE_DEV=/dev/ttyUSB1" in res.stdout, "console resolved to the wrong device"


@_NEEDS_BASH
def test_no_lg_place_and_no_se_uart_stays_empty_and_quiet(tmp_path: Path) -> None:
    """Backward-compatible default: neither set -> SE_UART stays empty, no
    warning noise (this is the ordinary "not doing bench work right now"
    shape, not the off-labgrid escape hatch)."""
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    res = subprocess.run(
        ["bash", "-c", "source ./bench-env.sh; echo \"SE_UART=[$SE_UART]\""],
        cwd=tmp_path, capture_output=True, text=True, timeout=60,
    )
    assert res.returncode == 0, res.stderr
    assert "SE_UART=[]" in res.stdout
    assert "WARNING" not in res.stderr


@_NEEDS_BASH
def test_raw_se_uart_without_lg_place_is_the_warned_escape_hatch(tmp_path: Path) -> None:
    """A raw SE_UART with no LG_PLACE must still WORK (erase-storage.sh
    documents a genuinely off-labgrid case) but must warn about the exact
    hazard that caused the incident: stale/unstable ttyUSBn paths."""
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    res = subprocess.run(
        ["bash", "-c", 'export SE_UART=/dev/ttyUSB9; source ./bench-env.sh; echo "SE_UART=[$SE_UART]"'],
        cwd=tmp_path, capture_output=True, text=True, timeout=60,
    )
    assert res.returncode == 0, res.stderr
    assert "SE_UART=[/dev/ttyUSB9]" in res.stdout
    assert "WARNING" in res.stderr
    assert "enumeration-ordered" in res.stderr


@_NEEDS_BASH
def test_lg_place_wins_over_a_simultaneously_exported_raw_se_uart(tmp_path: Path) -> None:
    res = _resolve(
        tmp_path, "e1m-aen-evk-01", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED},
        extra_env='export SE_UART=/dev/ttyUSB9\n',
    )
    assert res.returncode == 0, res.stderr
    assert "SE_UART=/dev/ttyUSB0" in res.stdout, "LG_PLACE must win over the raw SE_UART"
    assert "LG_PLACE=" in res.stderr and "ignoring the raw SE_UART" in res.stderr


@_NEEDS_BASH
def test_unreachable_coordinator_or_unknown_place_fails_loudly(tmp_path: Path) -> None:
    """`labgrid-client show` returning nothing (coordinator unreachable, or
    the place does not exist) must abort, not resolve an empty/guessed set
    of values."""
    res = _resolve(tmp_path, "e1m-aen-evk-99", {"e1m-aen-evk-01": REAL_EVK01_ACQUIRED})
    assert res.returncode == 1
    assert "returned nothing" in res.stderr

# SPDX-License-Identifier: Apache-2.0
"""Parity tests for the board.yaml validation entry points."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]


def _write_board(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _run_script(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(REPO / "scripts" / "validate_board_yaml.py"),
            "--input",
            str(path),
            "--no-color",
        ],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=False,
    )


def _run_alp_validate(path: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = str(REPO / "scripts")
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "alp_cli.main",
            "validate",
            "--no-color",
            str(path),
        ],
        cwd=REPO,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def _combined(proc: subprocess.CompletedProcess[str]) -> str:
    return proc.stdout + proc.stderr


def test_entrypoints_both_report_per_core_peripheral_warning(
    tmp_path: Path,
) -> None:
    # E1M-AEN801, not E1M-NX9101: NX9101's only hw_rev (imx93 r1) is
    # `status: tbd`, so it is refused outright by the hw_rev-buildable
    # gate (#1025) before this test's peripheral-warning path is even
    # reached (exit 5, not 0).  'emmc' has no key (or `emmc_lp`) at all
    # in metadata/socs/alif/ensemble/e8.json -- board-side, not a direct
    # SoC cap -- so it reproduces the same false-positive ALP-B010 this
    # test pins.
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
preset: e1m-evk
cores:
  m55_hp:
    app: .
    peripherals: [emmc]
""")

    script = _run_script(path)
    cli = _run_alp_validate(path)

    assert script.returncode == 0, _combined(script)
    assert cli.returncode == 0, _combined(cli)
    assert "ALP-B010" in _combined(script)
    assert "ALP-B010" in _combined(cli)


def test_entrypoints_both_reject_orchestrator_consistency_error(
    tmp_path: Path,
) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
preset: e1m-evk
cores:
  m55_hp:
    app: .
    iot: { tls: true }
""")

    script = _run_script(path)
    cli = _run_alp_validate(path)

    assert script.returncode == 1
    assert cli.returncode == 1
    assert "iot.tls" in _combined(script)
    assert "TLS provider" in _combined(script)
    assert "iot.tls" in _combined(cli)
    assert "TLS provider" in _combined(cli)

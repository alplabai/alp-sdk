# SPDX-License-Identifier: Apache-2.0
"""
Shared fixtures for the tests/scripts/test_orchestrate_*.py suite -- the
test_orchestrate_*.py suite split (issue #460 / #673 Phase 3,
module-size reduction).

Not a test module itself: no test_* functions/classes live here, just
the constants + helpers every split-out test_orchestrate_*.py file needs
to invoke scripts/alp_orchestrate/ the same way the monolith did.
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""

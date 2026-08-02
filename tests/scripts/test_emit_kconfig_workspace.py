# SPDX-License-Identifier: Apache-2.0
"""
Workspace-dependent test for `--emit kconfig` (#893) Task 3 -- Approach A's
stub `west build --cmake-only` + kconfiglib load.

Skips entirely when `ZEPHYR_BASE` isn't set (the normal case for a local
dev box / this repo's own hermetic CI jobs).  The authoritative
verification of this half is the CI schema/smoke contract instead:
`scripts/check_emit_kconfig_contract.py`, run in the Zephyr-bootstrapped
`pr-twister` job (see .github/workflows/pr-twister.yml).

Run in a bootstrapped workspace:

    ZEPHYR_BASE=/path/to/zephyrproject/zephyr \\
        python -m pytest tests/scripts/test_emit_kconfig_workspace.py -v
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import load_board_yaml  # noqa: E402
from alp_orchestrate.kconfig_symbols import emit_kconfig  # noqa: E402

pytestmark = pytest.mark.skipif(
    not os.environ.get("ZEPHYR_BASE"),
    reason="--emit kconfig Approach-A load needs a bootstrapped "
           "ZEPHYR_BASE (v4.4.0); verified instead by "
           "check_emit_kconfig_contract.py in the pr-twister CI job",
)


def test_emit_kconfig_happy_path_on_bootstrapped_workspace(tmp_path) -> None:
    import json

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    out = emit_kconfig(project, "m33_sm")
    envelope = json.loads(out)

    assert envelope["schemaVersion"] == 1
    assert envelope["core"] == "m33_sm"
    assert envelope["symbols"], "expected a non-empty promptable symbol menu"
    names = {s["name"] for s in envelope["symbols"]}
    assert "LOG" in names

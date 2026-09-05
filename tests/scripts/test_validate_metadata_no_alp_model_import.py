# SPDX-License-Identifier: Apache-2.0
"""`scripts/validate_metadata.py` must survive `scripts/alp_model/` being
deleted outright (#1943).

`validate_metadata.py` is a PR-blocking gate; `scripts/alp_model/` is a
package a future change (ADR-0028 Task 6) is expected to delete outright.
This fix moved `resolve_targets()`/`npu_backend()`/`accel_config()` out of
`alp_model.targets` and into `alp_project_loader.py` precisely so the gate
no longer depends on that package.

An earlier version of this test AST-walked `validate_metadata.py`'s
top-level imports for the literal name `alp_model` -- a proxy for what
actually matters. A transitive import (some other module the gate imports
reaching into `alp_model`) or a lazy in-function `import alp_model` would
both pass that proxy and still break Task 6. The direct check is the same
size: copy the real `scripts/`, `metadata/`, and `zephyr/boards/alp` trees,
delete `scripts/alp_model/` from the copy, and run `validate_metadata.py`
against it end to end -- the same proof this fix's own scratch-deletion
check used.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]


def test_validate_metadata_survives_alp_model_deletion(tmp_path):
    shutil.copytree(_REPO / "scripts", tmp_path / "scripts")
    shutil.copytree(_REPO / "metadata", tmp_path / "metadata")
    shutil.copytree(
        _REPO / "zephyr" / "boards" / "alp",
        tmp_path / "zephyr" / "boards" / "alp",
    )
    shutil.rmtree(tmp_path / "scripts" / "alp_model")

    result = subprocess.run(
        [sys.executable, str(tmp_path / "scripts" / "validate_metadata.py")],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert "alp_model" not in result.stderr, (
        "validate_metadata.py still depends on scripts/alp_model/ somewhere "
        f"(direct, transitive, or lazy import) -- traceback:\n{result.stderr}"
    )
    assert result.returncode == 0, (
        f"validate_metadata.py exited {result.returncode} against real "
        f"metadata with scripts/alp_model/ deleted:\nSTDOUT:\n{result.stdout}\n"
        f"STDERR:\n{result.stderr}"
    )

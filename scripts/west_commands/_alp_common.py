# SPDX-License-Identifier: Apache-2.0
"""
Shared helpers for the alp-sdk west extension commands.

The five wrappers (alp_build / alp_image / alp_flash / alp_clean /
alp_renode) all need to:

  1. Locate the SDK root so they can call scripts/alp_orchestrate/.
  2. Resolve the board.yaml the customer points at.
  3. Bootstrap ALP_SDK_ROOT + EXTRA_ZEPHYR_MODULES in the sub-process
     env without spamming the system PATH.

This module centralises that boilerplate.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Optional


def find_sdk_root() -> Optional[Path]:
    """Locate the alp-sdk root through (in order):

      ALP_SDK_ROOT env -> this file's grandparent ->
      EXTRA_ZEPHYR_MODULES / ZEPHYR_EXTRA_MODULES entries.

    Returns the first path that contains scripts/alp_orchestrate/;
    None when no candidate works.
    """
    env_root = os.environ.get("ALP_SDK_ROOT", "").strip()
    if env_root:
        p = Path(env_root)
        if (p / "scripts" / "alp_orchestrate" / "__init__.py").is_file():
            return p

    # scripts/west_commands/_alp_common.py -> the sdk root is two
    # parents up.
    candidate = Path(__file__).resolve().parents[2]
    if (candidate / "scripts" / "alp_orchestrate" / "__init__.py").is_file():
        return candidate

    for var in ("EXTRA_ZEPHYR_MODULES", "ZEPHYR_EXTRA_MODULES"):
        for entry in os.environ.get(var, "").split(os.pathsep):
            entry = entry.strip()
            if entry and (Path(entry) / "scripts" /
                          "alp_orchestrate" / "__init__.py").is_file():
                return Path(entry)
    return None


def python_exe() -> str:
    """Return the current Python interpreter; falls back to `python3`."""
    return sys.executable or "python3"


def resolve_board_yaml(app_path: Path,
                       explicit: Optional[str]) -> Path:
    """Resolve `<app>/board.yaml` (or an explicit override path)."""
    if explicit:
        p = Path(explicit).resolve()
    else:
        p = (app_path / "board.yaml").resolve()
    return p


def env_with_sdk(sdk_root: Path) -> dict[str, str]:
    """Build a sub-process env dict with ALP_SDK_ROOT +
    EXTRA_ZEPHYR_MODULES wired."""
    env = os.environ.copy()
    existing = env.get("EXTRA_ZEPHYR_MODULES", "")
    sep = os.pathsep
    if str(sdk_root) not in existing.split(sep):
        env["EXTRA_ZEPHYR_MODULES"] = (existing + sep + str(sdk_root)
                                        if existing else str(sdk_root))
    env["ALP_SDK_ROOT"] = str(sdk_root)
    # Make sure the alp_orchestrate package is importable when a wrapper
    # invokes the python module form (`python -m alp_orchestrate`).
    pp = env.get("PYTHONPATH", "")
    sdk_scripts = str(sdk_root / "scripts")
    if sdk_scripts not in pp.split(sep):
        env["PYTHONPATH"] = (pp + sep + sdk_scripts) if pp else sdk_scripts
    return env

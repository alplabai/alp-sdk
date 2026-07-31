# SPDX-License-Identifier: Apache-2.0
"""
Shared helpers for the alp-sdk west extension commands.

A wrapper using this module needs to:

  1. Locate the SDK root so it can hand it to the planner.
  2. Resolve the board.yaml the customer points at.
  3. Bootstrap ALP_SDK_ROOT + EXTRA_ZEPHYR_MODULES in the sub-process
     env without spamming the system PATH.

This module centralises that boilerplate.  `alp_emit` is the only
current consumer -- the build wrappers that shared it went away with the
SDK-side executor (ADR-0020 Phase 4).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Optional

# The alp-sdk root marker.  NOT west.yml or pyproject.toml: every Zephyr
# module ships the first and every Python project the second, so bare
# existence would resolve a workspace that merely NESTS alp-sdk (or any
# sibling repo) to the wrong root.  metadata/sdk_version.yaml is named
# after alp-sdk, is committed, and already carries the version every
# emit checks a board's hw_rev against -- it cannot outlive the SDK it
# identifies.
_SDK_MARKER = ("metadata", "sdk_version.yaml")


def _is_sdk_root(path: Path) -> bool:
    """Whether `path` is an alp-sdk checkout root (see `_SDK_MARKER`)."""
    return path.joinpath(*_SDK_MARKER).is_file()


def find_sdk_root() -> Optional[Path]:
    """Locate the alp-sdk root through (in order):

      ALP_SDK_ROOT env -> this file's grandparent ->
      EXTRA_ZEPHYR_MODULES / ZEPHYR_EXTRA_MODULES entries.

    Returns the first path carrying `_SDK_MARKER`; None when no
    candidate works.
    """
    env_root = os.environ.get("ALP_SDK_ROOT", "").strip()
    if env_root:
        p = Path(env_root)
        if _is_sdk_root(p):
            return p

    # scripts/west_commands/_alp_common.py -> the sdk root is two
    # parents up.
    candidate = Path(__file__).resolve().parents[2]
    if _is_sdk_root(candidate):
        return candidate

    for var in ("EXTRA_ZEPHYR_MODULES", "ZEPHYR_EXTRA_MODULES"):
        # `;`-joined always: this is a CMake list (Zephyr's
        # zephyr_module.py splits it on `;` on every platform), never an
        # OS path list -- os.pathsep would double-split on Linux/WSL,
        # where `;` != `:`.
        for entry in os.environ.get(var, "").split(";"):
            entry = entry.strip()
            if entry and _is_sdk_root(Path(entry)):
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
    # EXTRA_ZEPHYR_MODULES is a CMake list: Zephyr's zephyr_module.py
    # splits it on `;` on every platform, never os.pathsep -- joining
    # with `:` on Linux/WSL makes "is not a valid zephyr module" fail
    # the configure step.
    existing = env.get("EXTRA_ZEPHYR_MODULES", "")
    zsep = ";"
    if str(sdk_root) not in existing.split(zsep):
        env["EXTRA_ZEPHYR_MODULES"] = (existing + zsep + str(sdk_root)
                                        if existing else str(sdk_root))
    env["ALP_SDK_ROOT"] = str(sdk_root)
    # No PYTHONPATH injection: the planner is tan's (`tan.planner`), and
    # it resolves metadata off the SDK root it is handed, not off an
    # import path.  Putting <sdk>/scripts on PYTHONPATH would only make
    # the SDK's own soon-to-go Python importable again.
    return env

"""Shared project / SDK / west-workspace discovery for the `alp` CLI.

The build / run / flash / emit / monitor verbs are thin wrappers over the
same machinery the `west alp-*` extension commands drive (the orchestrator
package, `scripts/alp_project.py`, the flash-backend dispatcher).  This
module centralises the three questions every wrapper asks:

  1. Where is the app?          (walk up from cwd until board.yaml)
  2. Where is the SDK checkout? (ALP_SDK_ROOT env, else this package's repo)
  3. Am I inside a west workspace? (walk up until a `.west/` directory)

plus the sub-process env wiring (`ALP_SDK_ROOT`, `EXTRA_ZEPHYR_MODULES`,
`PYTHONPATH=<sdk>/scripts`) that mirrors
`scripts/west_commands/_alp_common.env_with_sdk()` so a CLI-invoked
orchestrator run behaves identically to a `west alp-build` one.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def find_project(start: Path) -> Path | None:
    """Walk up from *start* until a directory containing board.yaml."""
    cursor = start.resolve()
    while True:
        if (cursor / "board.yaml").is_file():
            return cursor
        if cursor.parent == cursor:
            return None
        cursor = cursor.parent


def sdk_root() -> Path | None:
    """Locate the alp-sdk checkout.

    Resolution order matches ``_alp_common.find_sdk_root()``:
    ``ALP_SDK_ROOT`` env first, then this package's own repo (the alp CLI is
    installed editable from ``<sdk>/scripts/alp_cli``, so two parents up from
    the package is the SDK root).
    """
    env_root = os.environ.get("ALP_SDK_ROOT", "").strip()
    if env_root:
        p = Path(env_root)
        if (p / "scripts" / "alp_orchestrate" / "__init__.py").is_file():
            return p
    candidate = Path(__file__).resolve().parents[2]
    if (candidate / "scripts" / "alp_orchestrate" / "__init__.py").is_file():
        return candidate
    return None


def find_west_topdir(start: Path) -> Path | None:
    """Walk up from *start* until a directory containing `.west/`."""
    cursor = start.resolve()
    while True:
        if (cursor / ".west").is_dir():
            return cursor
        if cursor.parent == cursor:
            return None
        cursor = cursor.parent


def subprocess_env(root: Path) -> dict[str, str]:
    """Sub-process env with the SDK wired in (mirrors _alp_common.env_with_sdk).

    Sets ``ALP_SDK_ROOT``, appends the SDK to ``EXTRA_ZEPHYR_MODULES``, and
    puts ``<sdk>/scripts`` on ``PYTHONPATH`` so ``python -m alp_orchestrate``
    resolves regardless of how the CLI itself was installed.
    """
    env = os.environ.copy()
    sep = os.pathsep
    existing = env.get("EXTRA_ZEPHYR_MODULES", "")
    if str(root) not in existing.split(sep):
        env["EXTRA_ZEPHYR_MODULES"] = (
            existing + sep + str(root) if existing else str(root)
        )
    env["ALP_SDK_ROOT"] = str(root)
    pp = env.get("PYTHONPATH", "")
    sdk_scripts = str(root / "scripts")
    if sdk_scripts not in pp.split(sep):
        env["PYTHONPATH"] = (pp + sep + sdk_scripts) if pp else sdk_scripts
    return env


def python_exe() -> str:
    """The current interpreter (the venv the CLI was installed into)."""
    return sys.executable or "python3"

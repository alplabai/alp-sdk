# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_example_storage_claims.py."""

from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_example_storage_claims.py"

sys.path.insert(0, str(REPO / "scripts"))
import check_example_storage_claims as storage_claims  # noqa: E402


def _write(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _example(root: Path, name: str, testcase: str, source: str) -> Path:
    ex = root / "examples" / name
    _write(ex, "testcase.yaml", testcase)
    _write(ex, "src/main.c", source)
    return ex


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def test_claiming_alp_storage_while_using_zephyr_flash_fails(tmp_path: Path) -> None:
    ex = _example(
        tmp_path,
        "bad-flash",
        """
        sample:
          description: validates `<alp/storage.h>` storage surface
        """,
        """
        #include <zephyr/drivers/flash.h>
        int app(void) { return flash_erase(0, 0, 0); }
        """,
    )
    errors = storage_claims.check_example(ex)
    assert len(errors) == 2
    assert "claims Alp storage coverage" in errors[0]


def test_zephyr_storage_demo_without_alp_claim_passes(tmp_path: Path) -> None:
    ex = _example(
        tmp_path,
        "zephyr-flash",
        """
        sample:
          description: validates the Zephyr flash driver
        """,
        """
        #include <zephyr/drivers/flash.h>
        int app(void) { return flash_write(0, 0, 0, 0); }
        """,
    )
    assert storage_claims.check_example(ex) == []


def test_alp_storage_claim_without_zephyr_api_passes(tmp_path: Path) -> None:
    ex = _example(
        tmp_path,
        "alp-storage",
        """
        sample:
          description: validates the portable storage surface
        """,
        """
        #include "alp/storage.h"
        int app(void) { return 0; }
        """,
    )
    assert storage_claims.check_example(ex) == []


def test_live_repo_is_clean() -> None:
    proc = _run("--root", str(REPO))
    assert proc.returncode == 0, proc.stdout + proc.stderr

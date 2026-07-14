"""Unit tests for scripts/check_version_doc_sync.py.

The gate checks only the machine-read version copies (version.h, pyproject.toml,
the alp_banner.c sample line).  README/docs prose is de-versioned and
scripts/alp_cli/__init__.py derives its __version__ from sdk_version.yaml, so
neither is synced here.
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_version_doc_sync.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _scaffold(root: Path, version: str = "0.9.0", *, banner_version: str = None):
    """Build a minimal repo-shaped tree with every version copy the gate
    checks, all in sync at `version` unless a specific override is given
    (used to inject drift for the *_fails tests)."""
    banner_version = banner_version if banner_version is not None else version

    (root / "metadata").mkdir(parents=True, exist_ok=True)
    (root / "metadata" / "sdk_version.yaml").write_text(
        f"version: {version}\nstatus:  released\n", encoding="utf-8")

    (root / "include" / "alp").mkdir(parents=True, exist_ok=True)
    major, minor, patch = version.split(".")
    (root / "include" / "alp" / "version.h").write_text(
        f'#define ALP_VERSION_MAJOR {major}\n'
        f'#define ALP_VERSION_MINOR {minor}\n'
        f'#define ALP_VERSION_PATCH {patch}\n'
        f'#define ALP_VERSION_STRING "{version}"\n', encoding="utf-8")

    (root / "pyproject.toml").write_text(
        f'[project]\nname = "alp-sdk-cli"\nversion = "{version}"\n', encoding="utf-8")

    (root / "src" / "zephyr").mkdir(parents=True, exist_ok=True)
    (root / "src" / "zephyr" / "alp_banner.c").write_text(
        f"/*\n * Sample banner:\n *\n"
        f" *   Alp SDK {banner_version}  |  E1M-AEN801  |  (c) Alp Lab AB\n */\n",
        encoding="utf-8")


def test_all_in_sync_passes(tmp_path):
    _scaffold(tmp_path)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_stale_banner_fails(tmp_path):
    _scaffold(tmp_path, banner_version="0.6.0")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_banner.c" in proc.stdout + proc.stderr


def test_stale_version_h_fails(tmp_path):
    _scaffold(tmp_path)
    (tmp_path / "include" / "alp" / "version.h").write_text(
        '#define ALP_VERSION_MAJOR 0\n'
        '#define ALP_VERSION_MINOR 6\n'
        '#define ALP_VERSION_PATCH 0\n'
        '#define ALP_VERSION_STRING "0.6.0"\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "version.h" in proc.stdout + proc.stderr


def test_stale_pyproject_fails(tmp_path):
    _scaffold(tmp_path)
    (tmp_path / "pyproject.toml").write_text(
        '[project]\nname = "alp-sdk-cli"\nversion = "0.6.0"\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "pyproject.toml" in proc.stdout + proc.stderr

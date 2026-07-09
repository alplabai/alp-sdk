"""Unit tests for scripts/check_version_doc_sync.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_version_doc_sync.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _scaffold(root: Path, version: str = "0.9.0", *, readme_minor: str = None,
              release_pin: str = None, board_config_version: str = None,
              cli_version: str = None, banner_version: str = None,
              dev_tooling_minor: str = None, hetero_flow_minor: str = None,
              verification_status_minor: str = None):
    """Build a minimal repo-shaped tree with every version copy the gate
    checks, all in sync at `version` unless a specific override is given
    (used to inject drift for the *_fails tests)."""
    mm = ".".join(version.split(".")[:2])
    readme_minor = readme_minor if readme_minor is not None else mm
    release_pin = release_pin if release_pin is not None else version
    board_config_version = board_config_version if board_config_version is not None else version
    cli_version = cli_version if cli_version is not None else version
    banner_version = banner_version if banner_version is not None else version
    dev_tooling_minor = dev_tooling_minor if dev_tooling_minor is not None else mm
    hetero_flow_minor = hetero_flow_minor if hetero_flow_minor is not None else mm
    verification_status_minor = (
        verification_status_minor if verification_status_minor is not None else mm)

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

    (root / "scripts" / "alp_cli").mkdir(parents=True, exist_ok=True)
    (root / "scripts" / "alp_cli" / "__init__.py").write_text(
        f'"""Alp SDK command-line interface."""\n\n__version__ = "{cli_version}"\n',
        encoding="utf-8")

    (root / "src" / "zephyr").mkdir(parents=True, exist_ok=True)
    (root / "src" / "zephyr" / "alp_banner.c").write_text(
        f"/*\n * Sample banner:\n *\n"
        f" *   Alp SDK {banner_version}  |  E1M-AEN801  |  (c) Alp Lab AB\n */\n",
        encoding="utf-8")

    (root / "README.md").write_text(
        f"> Partially silicon-verified (`v{readme_minor}`): blah\n\n"
        f"**v{readme_minor} ramp — paper-correct, mostly pre-HIL**\n\n"
        f"A v{readme_minor} project is **one declarative file** plus per-core.\n\n"
        f"      revision: main        # pin to a release tag — v{release_pin} is the latest\n\n"
        "  ┌───────────────┐\n"
        "  │ Dev Tooling   │\n"
        f"  │ (v{dev_tooling_minor})        │\n"
        "  └───────────────┘\n\n"
        f"# Zephyr (heterogeneous slice, v{hetero_flow_minor} flow)\n",
        encoding="utf-8")

    (root / "docs").mkdir(parents=True, exist_ok=True)
    (root / "docs" / "architecture.md").write_text(
        f"      revision: main           # pin to a release tag — v{release_pin} is the latest\n",
        encoding="utf-8")
    (root / "docs" / "board-config.md").write_text(
        "```\nmetadata/\n"
        f'├── sdk_version.yaml   # SDK release version (currently "version: {board_config_version}")\n'
        "```\n", encoding="utf-8")
    (root / "docs" / "verification-status.md").write_text(
        f"## Where v{verification_status_minor} actually sits\n\n"
        "| Layer | Status |\n|---|---|\n",
        encoding="utf-8")


def test_all_in_sync_passes(tmp_path):
    _scaffold(tmp_path)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_stale_cli_version_fails(tmp_path):
    # The #445 defect: CLI __version__ drifted to an old release.
    _scaffold(tmp_path, cli_version="0.6.0")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_cli/__init__.py" in proc.stdout + proc.stderr
    assert "0.6.0" in proc.stdout + proc.stderr


def test_stale_banner_fails(tmp_path):
    _scaffold(tmp_path, banner_version="0.6.0")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_banner.c" in proc.stdout + proc.stderr


def test_stale_release_tag_pin_fails(tmp_path):
    _scaffold(tmp_path, release_pin="0.8.1")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    out = proc.stdout + proc.stderr
    assert "README.md" in out
    assert "docs/architecture.md" in out


def test_stale_board_config_doc_fails(tmp_path):
    _scaffold(tmp_path, board_config_version="0.6.0")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "board-config.md" in proc.stdout + proc.stderr


def test_stale_readme_quickstart_minor_fails(tmp_path):
    _scaffold(tmp_path, readme_minor="0.8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "quick-start" in (proc.stdout + proc.stderr).lower()


def test_stale_dev_tooling_label_fails(tmp_path):
    _scaffold(tmp_path, dev_tooling_minor="0.8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "Dev Tooling" in proc.stdout + proc.stderr


def test_stale_heterogeneous_flow_comment_fails(tmp_path):
    _scaffold(tmp_path, hetero_flow_minor="0.8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "heterogeneous" in (proc.stdout + proc.stderr).lower()


def test_stale_verification_status_heading_fails(tmp_path):
    _scaffold(tmp_path, verification_status_minor="0.8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "verification-status.md" in proc.stdout + proc.stderr


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

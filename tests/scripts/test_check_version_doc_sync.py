"""Unit tests for scripts/check_version_doc_sync.py.

The gate checks the machine-read version copies (version.h, pyproject.toml,
the alp_banner.c sample line) plus VERSIONS.md's roadmap-table row for the
declared version (#1213/#1199).  README/docs status prose is de-versioned and
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

    (root / "VERSIONS.md").write_text(
        "# Alp SDK Versions\n\n"
        "| Version | Status   | Target |\n"
        "|---------|----------|--------|\n"
        f"| v{version} | released | test row |\n",
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


def test_versions_md_missing_row_fails(tmp_path):
    """#1213/#1199: a version bump with no matching VERSIONS.md roadmap
    row used to stay green here -- the gate only checked version.h /
    pyproject.toml / alp_banner.c."""
    _scaffold(tmp_path)
    (tmp_path / "VERSIONS.md").write_text(
        "# Alp SDK Versions\n\n"
        "| Version | Status   | Target |\n"
        "|---------|----------|--------|\n"
        "| v0.8.0  | released | old row, no v0.9.0 row |\n",
        encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "VERSIONS.md" in proc.stdout + proc.stderr
    assert "v0.9.0" in proc.stdout + proc.stderr


def test_versions_md_missing_file_fails(tmp_path):
    _scaffold(tmp_path)
    (tmp_path / "VERSIONS.md").unlink()
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "VERSIONS.md" in proc.stdout + proc.stderr


# ---------------------------------------------------------------------
# #1902: metadata/sdk_version.yaml's `version:` may carry a SemVer
# pre-release suffix during an rc window -- the gate must accept that
# shape (not sys.exit(2) as unparsable) and check ALP_VERSION_STRING
# against the FULL string while every other copy still checks the CORE
# triple only.
# ---------------------------------------------------------------------


def _scaffold_prerelease(root: Path, core: str = "0.9.0", suffix: str = "rc1"):
    """Same shape as `_scaffold`, but `version:` / ALP_VERSION_STRING carry
    a pre-release suffix while every other copy stays pinned to `core`
    (matching what scripts/bump_version.py actually writes)."""
    full = f"{core}-{suffix}"

    (root / "metadata").mkdir(parents=True, exist_ok=True)
    (root / "metadata" / "sdk_version.yaml").write_text(
        f"version: {full}\nstatus:  released\n", encoding="utf-8")

    (root / "include" / "alp").mkdir(parents=True, exist_ok=True)
    major, minor, patch = core.split(".")
    (root / "include" / "alp" / "version.h").write_text(
        f'#define ALP_VERSION_MAJOR {major}\n'
        f'#define ALP_VERSION_MINOR {minor}\n'
        f'#define ALP_VERSION_PATCH {patch}\n'
        f'#define ALP_VERSION_STRING "{full}"\n', encoding="utf-8")

    (root / "pyproject.toml").write_text(
        f'[project]\nname = "alp-sdk-cli"\nversion = "{core}"\n', encoding="utf-8")

    (root / "src" / "zephyr").mkdir(parents=True, exist_ok=True)
    (root / "src" / "zephyr" / "alp_banner.c").write_text(
        f"/*\n * Sample banner:\n *\n"
        f" *   Alp SDK {core}  |  E1M-AEN801  |  (c) Alp Lab AB\n */\n",
        encoding="utf-8")

    (root / "VERSIONS.md").write_text(
        "# Alp SDK Versions\n\n"
        "| Version | Status   | Target |\n"
        "|---------|----------|--------|\n"
        f"| v{core} | released | test row |\n",
        encoding="utf-8")


def test_prerelease_all_in_sync_passes(tmp_path):
    """The shape scripts/bump_version.py --to 0.9.0-rc1 actually produces
    must be accepted, not flagged as drift or a parse error."""
    _scaffold_prerelease(tmp_path)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_prerelease_version_h_string_missing_the_suffix_fails(tmp_path):
    """version.h's ALP_VERSION_STRING must carry the SAME suffix as
    sdk_version.yaml -- a build silently reporting the bare GA version
    during an rc window is exactly issue #1902."""
    _scaffold_prerelease(tmp_path)
    (tmp_path / "include" / "alp" / "version.h").write_text(
        '#define ALP_VERSION_MAJOR 0\n'
        '#define ALP_VERSION_MINOR 9\n'
        '#define ALP_VERSION_PATCH 0\n'
        '#define ALP_VERSION_STRING "0.9.0"\n',  # missing "-rc1"
        encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "version.h" in proc.stdout + proc.stderr
    assert "0.9.0-rc1" in proc.stdout + proc.stderr


def test_prerelease_suffix_leaking_into_pyproject_fails(tmp_path):
    """pyproject.toml must stay pinned to the CORE version even during an
    rc window (PEP 440 doesn't accept a bare "-rc1" suffix) -- catches a
    future bump_version.py regression that started leaking it in."""
    _scaffold_prerelease(tmp_path)
    (tmp_path / "pyproject.toml").write_text(
        '[project]\nname = "alp-sdk-cli"\nversion = "0.9.0-rc1"\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "pyproject.toml" in proc.stdout + proc.stderr

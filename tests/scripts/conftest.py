"""Pytest configuration for tests/scripts/: put scripts/ on sys.path."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make packages under scripts/ (alp_cli, alp_orchestrate, ...) importable directly.
_scripts = Path(__file__).resolve().parents[2] / "scripts"
if str(_scripts) not in sys.path:
    sys.path.insert(0, str(_scripts))

_REPO = Path(__file__).resolve().parents[2]
_CLANG_FORMAT_STYLE = _REPO / ".clang-format"

# The repo pins this exact version (pr-generated-files.yml / pr-static-
# analysis.yml, both `pip install clang-format==22.1.5`) because clang-format
# output moves between versions; the committed cap.h/cap.c/soc_caps.h are
# v22-formatted. Comparing against any other version turns a host-tool
# mismatch into a false "generator drifted" failure.
_CLANG_FORMAT_PIN = "22.1.5"


def clang_format_text(tmp_path: Path, name: str, text: str) -> str:
    """Write `text` under tmp_path and run it through the repo's clang-format,
    the way a generator's own post-processing pass formats its real output --
    without ever writing into the working tree.

    Uses an explicit `file:<path>` style argument rather than plain
    `--style=file` (which walks up from the *file's own* directory): tmp_path
    lives outside the repo, so implicit lookup would silently pick up whatever
    stray .clang-format (if any) happens to sit above it on the filesystem,
    not this repo's.

    Skips (does not fail) unless the exact pinned clang-format is on PATH:
    the byte-identity check this feeds is already gated with the pinned
    toolchain by the `generated-files` CI job, so a host without it (or with
    a different version) isn't a real drift signal -- see #973 finding 1/2.
    """
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if not exe:
        pytest.skip("clang-format not found on PATH")
    version_out = subprocess.run(
        [exe, "--version"], capture_output=True, text=True, check=True
    ).stdout
    match = re.search(r"(\d+\.\d+\.\d+)", version_out)
    found = match.group(1) if match else version_out.strip()
    if found != _CLANG_FORMAT_PIN:
        pytest.skip(
            f"clang-format {found} on PATH, need pinned {_CLANG_FORMAT_PIN}"
        )
    path = tmp_path / name
    path.write_text(text, encoding="utf-8", newline="")
    subprocess.run(
        [exe, "-i", f"--style=file:{_CLANG_FORMAT_STYLE}", str(path)], check=True
    )
    return path.read_text(encoding="utf-8")

"""Pytest configuration for tests/scripts/: put scripts/ on sys.path."""
import shutil
import subprocess
import sys
from pathlib import Path

# Make packages under scripts/ (alp_model, alp_cli, ...) importable directly.
_scripts = Path(__file__).resolve().parents[2] / "scripts"
if str(_scripts) not in sys.path:
    sys.path.insert(0, str(_scripts))

_REPO = Path(__file__).resolve().parents[2]
_CLANG_FORMAT_STYLE = _REPO / ".clang-format"


def clang_format_text(tmp_path: Path, name: str, text: str) -> str:
    """Write `text` under tmp_path and run it through the repo's clang-format,
    the way a generator's own post-processing pass formats its real output --
    without ever writing into the working tree.

    Uses an explicit `file:<path>` style argument rather than plain
    `--style=file` (which walks up from the *file's own* directory): tmp_path
    lives outside the repo, so implicit lookup would silently pick up whatever
    stray .clang-format (if any) happens to sit above it on the filesystem,
    not this repo's.
    """
    path = tmp_path / name
    path.write_text(text, encoding="utf-8", newline="")
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    assert exe, "clang-format not found on PATH -- required to compare formatted output"
    subprocess.run(
        [exe, "-i", f"--style=file:{_CLANG_FORMAT_STYLE}", str(path)], check=True
    )
    return path.read_text(encoding="utf-8")

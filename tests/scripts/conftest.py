"""Pytest configuration for tests/scripts/: put scripts/ on sys.path."""
import sys
from pathlib import Path

# Make packages under scripts/ (alp_model, alp_cli, ...) importable directly.
_scripts = Path(__file__).resolve().parents[2] / "scripts"
if str(_scripts) not in sys.path:
    sys.path.insert(0, str(_scripts))

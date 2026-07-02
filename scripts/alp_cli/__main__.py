"""`python -m alp_cli` entry point.

Same front door as the installed `alp` console script -- useful straight
from a checkout (with `scripts/` on `sys.path` / `PYTHONPATH`) before
`pip install -e .` has run.
"""

from __future__ import annotations

from alp_cli.main import cli

if __name__ == "__main__":
    cli(prog_name="alp")

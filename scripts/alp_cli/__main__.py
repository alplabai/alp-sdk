"""`python -m alp_cli` entry point.

SDK-internal/reference CLI, useful straight from a checkout with `scripts/` on
`sys.path` / `PYTHONPATH`. The user-facing command surface is Python Tan; this
package deliberately has no installed `alp` console script.
"""

from __future__ import annotations

from alp_cli.main import cli

if __name__ == "__main__":
    cli(prog_name="alp")

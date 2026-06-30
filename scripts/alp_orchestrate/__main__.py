# SPDX-License-Identifier: Apache-2.0
"""`python -m alp_orchestrate` entry point.

Thin on purpose: it imports `main` from `cli` (not the other way round) so that
nothing imports `__main__`, which would re-enter the package under runpy and
emit a RuntimeWarning on every invocation.
"""

from alp_orchestrate.cli import main

if __name__ == "__main__":
    raise SystemExit(main())

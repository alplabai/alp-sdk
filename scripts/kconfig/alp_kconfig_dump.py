#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Dumps one board's promptable Kconfig symbols as JSON.

Runs INSIDE Zephyr's own Kconfig environment via the `EXTRA_KCONFIG_TARGET`
mechanism (`cmake/modules/kconfig.cmake` ~199-243, the same seam
`menuconfig`/`guiconfig`/`hardenconfig`/`traceconfig` use): a
`west build --cmake-only -- -DEXTRA_KCONFIG_TARGETS=alpkconfigjson
-DEXTRA_KCONFIG_TARGET_COMMAND_FOR_alpkconfigjson=<this>;--output;<path>`
registers a custom target that (per kconfig.cmake:227-242) runs

    ${CMAKE_COMMAND} -E env <COMMON_KCONFIG_ENV_SETTINGS...> \\
        ${PYTHON_EXECUTABLE} <this> --output <path> ${KCONFIG_ROOT}

i.e. Zephyr's CMake hands this script the *exact* env it computed for that
board/toolchain/module set (srctree, ARCH, BOARD_DIR, module Kconfigs, ...)
plus `KCONFIG_ROOT` as the trailing positional -- no env reconstruction
needed. `west build -d <build> -t alpkconfigjson` (a second, separate
invocation -- `add_custom_target` isn't built by `--cmake-only` alone) is
what actually runs it. See `alp_orchestrate/kconfig_symbols.py
(_load_board_symbols)` for the caller.

An earlier version tried a `-DPYTHON_EXECUTABLE=<spy>` override to capture
that env instead of using this target mechanism -- discarded: Zephyr's own
`cmake/modules/python.cmake` re-derives `PYTHON_EXECUTABLE` from
`find_package(Python3)` with a plain (non-`_ifndef`) `set()`, so any
`-D`-supplied override is silently clobbered before `kconfig.cmake` even
runs.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

# kconfiglib is a plain module file under $ZEPHYR_BASE/scripts/kconfig, not
# pip-installed -- ZEPHYR_BASE is guaranteed set in this script's env (see
# module docstring: COMMON_KCONFIG_ENV_SETTINGS always sets it).
sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts", "kconfig"))
import kconfiglib  # noqa: E402

# Reuse the hermetic projection alp-sdk already unit-tests (tests/scripts/
# test_emit_kconfig.py) instead of re-implementing it here.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from alp_orchestrate.kconfig_symbols import _project_symbols  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("kconfig_root")
    args = ap.parse_args()

    kconf = kconfiglib.Kconfig(args.kconfig_root, warn=True, warn_to_stderr=False)
    symbols = _project_symbols(
        kconf.unique_defined_syms,
        type_to_str=kconfiglib.TYPE_TO_STR,
        expr_str=kconfiglib.expr_str,
    )
    args.output.write_text(json.dumps(symbols), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())

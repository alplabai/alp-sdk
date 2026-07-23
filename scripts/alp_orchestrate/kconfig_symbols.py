#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`--emit kconfig` -- the board-scoped, user-settable Kconfig symbol menu
for the vscode `prj.conf` LSP (#893); unblocks tan-cli #35 (`tan kconfig`).

Deliberately separate from `kconfig.py` (the alp.conf/local.conf string
templater, which never runs kconfiglib/west -- see its module docstring):
this module is the SDK's first workspace-dependent emit.  Every other
`--emit` mode is hermetic (provable from board.yaml + this repo's own
metadata alone); this one needs a bootstrapped Zephyr workspace
(`ZEPHYR_BASE`, v4.4.0) because only the real Kconfig solver knows which
symbols are user-promptable for a given board.

Two independent halves:

  * `_project_symbols` / `_envelope` -- pure JSON shaping, unit-tested from
    a fake symbol list with NO Zephyr installed (see
    tests/scripts/test_emit_kconfig.py).  Reused verbatim (imported, not
    duplicated) by `scripts/kconfig/alp_kconfig_dump.py` -- see below.
  * `_load_board_symbols` -- Approach A: a stub `west build --cmake-only`
    registers Zephyr's `EXTRA_KCONFIG_TARGET` custom-target mechanism
    (`cmake/modules/kconfig.cmake` ~199-243 -- the same seam `west build -t
    menuconfig` uses) pointed at `scripts/kconfig/alp_kconfig_dump.py`;
    `west build -t alpkconfigjson` then runs it INSIDE the exact Kconfig
    env Zephyr's own CMake computed for that board/toolchain/module set
    (no env reconstruction needed -- Zephyr hands it over directly). This
    is workspace-dependent and is skipped locally without a bootstrapped
    ZEPHYR_BASE; it is verified by the pr-twister CI job instead (see
    `scripts/check_emit_kconfig_contract.py`).

    An earlier version tried overriding `-DPYTHON_EXECUTABLE=<spy>` to
    capture kconfig.py's own env instead -- discarded: Zephyr's
    `cmake/modules/python.cmake` re-derives `PYTHON_EXECUTABLE` via a
    plain `set()` (not `_ifndef`), so a `-D` override is silently
    clobbered before `kconfig.cmake` ever runs, and kconfig.py never
    actually invokes the spy.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path
from typing import Any, Callable, Iterable, Optional

from .models import BoardProject, OrchestratorError

SCHEMA_VERSION = 1

# ---------------------------------------------------------------------
# Workspace guard (Task 1)
# ---------------------------------------------------------------------


def _kconfig_dir(zephyr_base: Path) -> Path:
    return zephyr_base / "scripts" / "kconfig"


def _kconfiglib_path(zephyr_base: Path) -> Path:
    return _kconfig_dir(zephyr_base) / "kconfiglib.py"


def _require_workspace() -> Path:
    """Return the bootstrapped `ZEPHYR_BASE`, or print the actionable
    message + exit(2) -- this mode's one deliberate deviation from the
    rest of the orchestrator's `OrchestratorError` -> exit(1) convention,
    marking "no workspace" as distinct from an ordinary usage error.

    This process itself never imports kconfiglib (only
    `scripts/kconfig/alp_kconfig_dump.py` does, in its own subprocess
    running inside Zephyr's own env -- see `_load_board_symbols`); this
    just probes `kconfiglib.py`'s presence as the "is this ZEPHYR_BASE
    real" check.
    """
    raw = os.environ.get("ZEPHYR_BASE")
    if raw:
        zephyr_base = Path(raw)
        if _kconfiglib_path(zephyr_base).is_file():
            return zephyr_base
    print(
        "alp_orchestrate: --emit kconfig requires a bootstrapped Zephyr "
        "workspace (set ZEPHYR_BASE; west init/update v4.4.0)",
        file=sys.stderr,
    )
    sys.exit(2)


# ---------------------------------------------------------------------
# Hermetic symbol projection + envelope (Task 2) -- no kconfiglib import
# at module scope, so this half unit-tests with no Zephyr installed.
# ---------------------------------------------------------------------


def _project_symbols(
    syms: Iterable[Any],
    *,
    type_to_str: dict[Any, str],
    expr_str: Callable[[Any], str],
) -> list[dict[str, Any]]:
    """Project promptable symbols to `{name, type, prompt, depends,
    default, help}`, sorted by name.

    `type_to_str` / `expr_str` are dependency-injected rather than
    imported from kconfiglib here: kconfiglib's own BOOL/TRISTATE/...
    type constants are internal tokenizer values with NO guaranteed
    numeric stability across releases (kconfiglib.py's own comment:
    "Client code shouldn't rely on [the values] though") -- the real
    call site (`_load_board_symbols`/`emit_kconfig`) passes kconfiglib's
    own `TYPE_TO_STR` dict and `expr_str` function, so this stays correct
    for whatever kconfiglib the bootstrapped ZEPHYR_BASE ships, and the
    hermetic unit test passes its own small fakes with no kconfiglib
    installed at all.

    Only symbols with a prompt on their first `MenuNode` are kept --
    `sym.nodes and sym.nodes[0].prompt` -- the scope is a user-settable
    `prj.conf` menu, not the full invisible symbol tree (~26k symbols).
    """
    projected: list[dict[str, Any]] = []
    for sym in syms:
        if not (sym.nodes and sym.nodes[0].prompt):
            continue
        node = sym.nodes[0]
        default = None
        if sym.orig_defaults:
            default_expr, _cond = sym.orig_defaults[0]
            default = expr_str(default_expr)
        projected.append({
            "name":    sym.name,
            "type":    type_to_str.get(sym.type, "unknown"),
            "prompt":  node.prompt[0],
            "depends": expr_str(sym.direct_dep) or "",
            "default": default,
            "help":    getattr(node, "help", None) or "",
        })
    projected.sort(key=lambda entry: entry["name"])
    return projected


def _envelope(board: str, core: str, symbols: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "board":         board,
        "core":          core,
        "symbols":       symbols,
    }


# ---------------------------------------------------------------------
# Approach-A Zephyr load (Task 3) -- workspace-dependent
# ---------------------------------------------------------------------

_STUB_CMAKELISTS = textwrap.dedent("""\
    # SPDX-License-Identifier: Apache-2.0
    # Auto-generated by alp_orchestrate/kconfig_symbols.py (#893) -- a
    # throwaway stub app whose sole purpose is to make Zephyr's own CMake
    # configure the Kconfig tree for one board so kconfiglib can be
    # pointed at the real env it computed.  Never built.
    cmake_minimum_required(VERSION 3.20.0)
    find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
    project(alp_emit_kconfig_stub)
    target_sources(app PRIVATE src/main.c)
    """)

_STUB_MAIN_C = "int main(void)\n{\n\treturn 0;\n}\n"

# scripts/kconfig/alp_kconfig_dump.py -- a sibling of this package, not a
# submodule of it (it has to run as a standalone script inside Zephyr's own
# Kconfig env, not import this package).
_DUMPER = Path(__file__).resolve().parent.parent / "kconfig" / "alp_kconfig_dump.py"

_KCONFIG_TARGET = "alpkconfigjson"


def _write_stub_app(app_dir: Path) -> None:
    (app_dir / "src").mkdir(parents=True)
    (app_dir / "CMakeLists.txt").write_text(_STUB_CMAKELISTS, encoding="utf-8")
    (app_dir / "prj.conf").write_text("", encoding="utf-8")
    (app_dir / "src" / "main.c").write_text(_STUB_MAIN_C, encoding="utf-8")


def _load_board_symbols(zephyr_base: Path, board_triple: str) -> list[dict[str, Any]]:
    """Approach A: configure a stub app for `board_triple`, then run
    `alp_kconfig_dump.py` INSIDE Zephyr's own Kconfig env via the
    `EXTRA_KCONFIG_TARGET` mechanism (the same seam `west build -t
    menuconfig` uses -- see `alp_kconfig_dump.py`'s module docstring).
    Returns the already-projected `list[dict]` the dumper wrote.

    `west build` is a west extension command -- it needs to run from
    inside a west workspace (a `.west/` upward from cwd), which
    `alp_orchestrate` itself is never a part of. `$ZEPHYR_BASE/..` is
    that workspace's topdir under every documented Zephyr `west init`
    layout (Getting Started's `~/zephyrproject/{.west, zephyr,
    modules,...}`, and the same layout pr-twister.yml's own `west init
    -m .../zephyr .` step produces).
    """
    tmp_dir = Path(tempfile.mkdtemp(prefix="alp-emit-kconfig-"))
    try:
        app_dir = tmp_dir / "stub"
        _write_stub_app(app_dir)

        build_dir = tmp_dir / "build"
        output_json = build_dir / "alp_kconfig.json"
        # A CMake list (semicolon-separated -- COMMAND_EXPAND_LISTS on the
        # custom target splits it into argv tokens): the dumper script,
        # then its own `--output <path>` -- Zephyr appends ${KCONFIG_ROOT}
        # as the final token itself (kconfig.cmake:238).
        target_cmd = f"{_DUMPER};--output;{output_json}"

        configure_cmd = [
            "west", "build", "--cmake-only",
            "-b", board_triple,
            "-d", str(build_dir),
            str(app_dir),
            "--",
            f"-DEXTRA_KCONFIG_TARGETS={_KCONFIG_TARGET}",
            f"-DEXTRA_KCONFIG_TARGET_COMMAND_FOR_{_KCONFIG_TARGET}={target_cmd}",
        ]
        proc = subprocess.run(configure_cmd, cwd=zephyr_base.parent,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise OrchestratorError(
                f"--emit kconfig: `west build --cmake-only -b "
                f"{board_triple}` failed:\n{proc.stderr.strip()}")

        # `add_custom_target` never runs at configure time -- `-t` builds
        # it explicitly (a second, separate `west build`).
        build_cmd = ["west", "build", "-d", str(build_dir), "-t", _KCONFIG_TARGET]
        proc = subprocess.run(build_cmd, cwd=zephyr_base.parent,
                              capture_output=True, text=True)
        # TEMPORARY (#893): forward stderr (carries alp_kconfig_dump.py's
        # ALP_KCONFIG_DIAG lines) even on success -- otherwise it's
        # captured here and dropped, invisible to the CI log. Remove once
        # the partial-tree symptom is root-caused (paired with the
        # dumper's own temporary diagnostic).
        if proc.stderr:
            print(proc.stderr, file=sys.stderr, end="")
        if proc.returncode != 0:
            raise OrchestratorError(
                f"--emit kconfig: `west build -t {_KCONFIG_TARGET}` failed "
                f"for board '{board_triple}':\n{proc.stderr.strip()}")
        if not output_json.is_file():
            raise OrchestratorError(
                f"--emit kconfig: `west build -t {_KCONFIG_TARGET}` "
                f"completed but never wrote {output_json} -- never emit a "
                f"partial/empty menu")

        return json.loads(output_json.read_text(encoding="utf-8"))
    finally:
        import shutil
        shutil.rmtree(tmp_dir, ignore_errors=True)


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------


def emit_kconfig(project: BoardProject, core: Optional[str]) -> str:
    """`--emit kconfig`: the board-scoped, user-settable Kconfig symbol
    menu for the resolved `--core <id>` slice, as JSON.

    Validates `core` against the project's own resolved slices first
    (a plain `OrchestratorError` -- exit(1) via the cli's usual path),
    THEN checks the Zephyr workspace is bootstrapped (exit(2) -- see
    `_require_workspace`).
    """
    if not core:
        raise OrchestratorError(
            "--emit kconfig requires --core <id> (the SDK plans one "
            "Kconfig symbol menu per board, not per project)")
    if core not in project.cores:
        raise OrchestratorError(
            f"--core {core} not present in this board.yaml's cores "
            f"(have: {', '.join(sorted(project.cores))})")
    slice_ = project.cores[core]
    if not slice_.board:
        raise OrchestratorError(
            f"core '{core}' has no resolved Zephyr board target "
            f"(os: {slice_.os}); --emit kconfig only applies to "
            f"Zephyr cores")

    zephyr_base = _require_workspace()

    # `alp_kconfig_dump.py` (run inside Zephyr's own Kconfig env -- see
    # `_load_board_symbols`) already projects with the real kconfiglib, so
    # this is just the envelope wrap.
    symbols = _load_board_symbols(zephyr_base, slice_.board)
    envelope = _envelope(slice_.board, core, symbols)
    return json.dumps(envelope, indent=2) + "\n"

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
    tests/scripts/test_emit_kconfig.py).
  * `_load_board_symbols` -- Approach A (a stub `west build --cmake-only`,
    then read kconfiglib against the *exact* env Zephyr's own
    cmake/modules/kconfig.cmake computed for that invocation).  This is
    workspace-dependent and is skipped locally without a bootstrapped
    ZEPHYR_BASE; it is verified by the pr-twister CI job instead (see
    `scripts/check_emit_kconfig_contract.py`).
"""

from __future__ import annotations

import json
import os
import stat
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


def _kconfiglib_path(zephyr_base: Path) -> Path:
    return zephyr_base / "scripts" / "kconfig" / "kconfiglib.py"


def _require_workspace() -> Path:
    """Return the bootstrapped `ZEPHYR_BASE`, or print the actionable
    message + exit(2) -- this mode's one deliberate deviation from the
    rest of the orchestrator's `OrchestratorError` -> exit(1) convention,
    marking "no workspace" as distinct from an ordinary usage error.
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

# A transparent `PYTHON_EXECUTABLE` spy.  `cmake/modules/kconfig.cmake`
# (Zephyr v4.4.0) invokes `${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/
# kconfig/kconfig.py --zephyr-base=... <KCONFIG_ROOT> ...` inside
# `${CMAKE_COMMAND} -E env <COMMON_KCONFIG_ENV_SETTINGS...>` -- a ~15-var
# env (srctree, ARCH, ARCH_DIR, BOARD/BOARD_QUALIFIERS/BOARD_REVISION,
# KCONFIG_BINARY_DIR, KCONFIG_BOARD_DIR, TOOLCHAIN_KCONFIG_DIR, EDT_PICKLE,
# per-module ZEPHYR_<NAME>_KCONFIG, ...) that CMake alone computes from the
# board/toolchain/module set.  Rather than hand-derive those ~15 vars here
# (version-fragile -- see cmake/modules/kconfig.cmake), this spy captures
# the exact env + argv CMake already worked out for THIS invocation, by
# standing in for PYTHON_EXECUTABLE for the whole `--cmake-only` configure
# and transparently forwarding every call to the real interpreter (so the
# configure itself is unaffected) -- except the one call to kconfig.py,
# which it snapshots first.
_ENV_SPY = textwrap.dedent("""\
    #!/usr/bin/env python3
    import json, os, subprocess, sys

    _dump = os.environ.get("_ALP_KCONFIG_ENV_DUMP")
    _args = sys.argv[1:]
    if _dump and _args and _args[0].replace("\\\\", "/").endswith(
            "scripts/kconfig/kconfig.py"):
        with open(_dump, "w", encoding="utf-8") as _f:
            json.dump({"argv": _args, "environ": dict(os.environ)}, _f)
    sys.exit(subprocess.call([sys.executable] + _args))
    """)


def _write_stub_app(app_dir: Path) -> None:
    (app_dir / "src").mkdir(parents=True)
    (app_dir / "CMakeLists.txt").write_text(_STUB_CMAKELISTS, encoding="utf-8")
    (app_dir / "prj.conf").write_text("", encoding="utf-8")
    (app_dir / "src" / "main.c").write_text(_STUB_MAIN_C, encoding="utf-8")


def _kconfig_root_from_argv(argv: list[str], zephyr_base: Path) -> str:
    """The `kconfig_file` kconfig.py positional (== KCONFIG_ROOT) -- the
    first non-flag token after the script path.  Falls back to
    `$ZEPHYR_BASE/Kconfig` (the stub app declares no app-level Kconfig,
    so that's what `cmake/modules/kconfig.cmake` resolves it to) if the
    capture ever comes up short.
    """
    positionals = [a for a in argv[1:] if not a.startswith("--")]
    if positionals:
        return positionals[0]
    return str(zephyr_base / "Kconfig")


def _load_board_symbols(zephyr_base: Path, board_triple: str) -> list[Any]:
    """Approach A: configure a stub app for `board_triple`, capture the
    exact Kconfig env/root Zephyr's own CMake computed, and load it with
    kconfiglib directly.  Returns `kconf.unique_defined_syms`.
    """
    import kconfiglib  # local: only reachable once _require_workspace()
                        # confirmed ZEPHYR_BASE/kconfiglib.py exist.

    tmp_dir = Path(tempfile.mkdtemp(prefix="alp-emit-kconfig-"))
    try:
        app_dir = tmp_dir / "stub"
        _write_stub_app(app_dir)

        spy = tmp_dir / "_alp_kconfig_env_spy.py"
        spy.write_text(_ENV_SPY, encoding="utf-8")
        spy.chmod(spy.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

        env_dump = tmp_dir / "kconfig-env.json"
        build_dir = tmp_dir / "build"

        env = dict(os.environ)
        env["_ALP_KCONFIG_ENV_DUMP"] = str(env_dump)

        cmd = [
            "west", "build", "--cmake-only",
            "-b", board_triple,
            "-d", str(build_dir),
            str(app_dir),
            "--", f"-DPYTHON_EXECUTABLE={spy}",
        ]
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if proc.returncode != 0:
            raise OrchestratorError(
                f"--emit kconfig: `west build --cmake-only -b "
                f"{board_triple}` failed:\n{proc.stderr.strip()}")
        if not env_dump.is_file():
            raise OrchestratorError(
                "--emit kconfig: `west build --cmake-only` completed but "
                "never invoked kconfig.py -- unexpected Zephyr CMake "
                "layout for this ZEPHYR_BASE (the PYTHON_EXECUTABLE spy "
                "never fired)")

        captured = json.loads(env_dump.read_text(encoding="utf-8"))
        kconfig_root = _kconfig_root_from_argv(captured["argv"], zephyr_base)

        prior_environ = dict(os.environ)
        os.environ.clear()
        os.environ.update(captured["environ"])
        try:
            kconf = kconfiglib.Kconfig(
                kconfig_root, warn=True, warn_to_stderr=False)
        finally:
            os.environ.clear()
            os.environ.update(prior_environ)
        return list(kconf.unique_defined_syms)
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

    import kconfiglib  # noqa: E402  (deferred: only importable once the
                        # workspace guard above has confirmed it exists)

    syms = _load_board_symbols(zephyr_base, slice_.board)
    symbols = _project_symbols(
        syms, type_to_str=kconfiglib.TYPE_TO_STR, expr_str=kconfiglib.expr_str)
    envelope = _envelope(slice_.board, core, symbols)
    return json.dumps(envelope, indent=2) + "\n"

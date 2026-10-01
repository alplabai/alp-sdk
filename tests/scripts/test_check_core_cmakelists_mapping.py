# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_core_cmakelists_mapping.py -- the dual-core
CMakeLists.txt mapping gate (issue #1275 Unit A).

Each case scaffolds a scratch `examples/<name>/` tree under `tmp_path` but
loads it against the REAL `metadata/` (E1M-AEN801 is a real, fully-defined,
buildable SoM -- no synthetic SoM preset needed, unlike the NX9101 fixture
`_orchestrate_support._synthetic_nx9101_root` uses to dodge a real
`status: tbd` hw_rev). `load_board_yaml` resolves `metadata_root` from the
real repo unconditionally (`alp_orchestrate.paths.METADATA_ROOT`), so this
works regardless of where the board.yaml itself lives.

Run locally:

    python -m pytest tests/scripts/test_check_core_cmakelists_mapping.py -v
"""
from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_core_cmakelists_mapping as gate  # noqa: E402

_CMAKE = """\
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(demo LANGUAGES C)
target_sources(app PRIVATE src/main.c)
"""


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8")
    return path


def test_clean_tree_passes(tmp_path):
    # One core, app: ./src -> the root CMakeLists.txt. No sharing.
    ex = tmp_path / "examples" / "demo"
    _write(ex / "board.yaml", """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        cores:
          a32_cluster:
            os: "off"
          m55_he:
            app: ./src
        diagnostics:
          log_level: info
        """)
    _write(ex / "CMakeLists.txt", _CMAKE)

    assert gate.find_problems(tmp_path) == []


def test_undeclared_core_defaults_to_shared_stock_shim_is_not_flagged(
        tmp_path):
    # Only m55_hp is declared; m55_he is left at the SoM topology default
    # (`alp-stock-shim`) -- the same file EVERY unconfigured core across the
    # whole SDK resolves to by design (mirrors the real
    # examples/multicore/mproc-mailbox/board.yaml pattern). Must not be
    # flagged as "sharing".
    ex = tmp_path / "examples" / "demo"
    _write(ex / "board.yaml", """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        cores:
          a32_cluster:
            os: "off"
          m55_hp:
            app: ./src
        diagnostics:
          log_level: info
        """)
    _write(ex / "CMakeLists.txt", _CMAKE)

    assert gate.find_problems(tmp_path) == []


def test_shared_cmakelists_flagged_assertion_2(tmp_path):
    # The #1275 trap: m55_hp is ADDED pointing at the same `./src` the
    # already-wired m55_he uses, so both cores would share one app dir (and
    # one generated/alp.conf).
    ex = tmp_path / "examples" / "demo"
    _write(ex / "board.yaml", """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        cores:
          a32_cluster:
            os: "off"
          m55_he:
            app: ./src
          m55_hp:
            app: ./src
        diagnostics:
          log_level: info
        """)
    _write(ex / "CMakeLists.txt", _CMAKE)

    problems = gate.find_problems(tmp_path)
    sharing = [p for p in problems if "shared by" in p]
    assert len(sharing) == 1, problems
    assert "m55_he" in sharing[0] and "m55_hp" in sharing[0]
    assert "2 distinct Zephyr cores" in sharing[0]


def test_real_corpus_clean():
    # The real repo, audited (issue #1275: not pre-verified before this
    # gate shipped) -- 100 examples/**/board.yaml, ~96 real per-example
    # Zephyr core -> app: mappings (73 more default to the shared stock
    # shim and are excluded by design). Pins the audit result: clean.
    problems = gate.find_problems(REPO)
    assert problems == [], problems

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

_CMAKE_TMPL = """\
cmake_minimum_required(VERSION 3.20)
execute_process(
    COMMAND python3 alp_project.py --input board.yaml
            --emit zephyr-conf --core {core} --output x
)
find_package(Zephyr REQUIRED HINTS $ENV{{ZEPHYR_BASE}})
project(demo LANGUAGES C)
target_sources(app PRIVATE src/main.c)
"""


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8")
    return path


def test_clean_tree_passes(tmp_path):
    # One core, app: ./src -> the root CMakeLists.txt, baked with that
    # SAME core's --core literal. No sharing, no mismatch.
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
    _write(ex / "CMakeLists.txt", _CMAKE_TMPL.format(core="m55_he"))

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
    _write(ex / "CMakeLists.txt", _CMAKE_TMPL.format(core="m55_hp"))

    assert gate.find_problems(tmp_path) == []


def test_literal_mismatch_flagged_assertion_1(tmp_path):
    # Assertion 1 (literal agreement), standalone: one core, no sharing,
    # but the CMakeLists.txt is baked with a DIFFERENT core's literal.
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
    _write(ex / "CMakeLists.txt", _CMAKE_TMPL.format(core="m55_hp"))

    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "literal agreement violated" in problems[0]
    assert "baked `--core m55_hp`" in problems[0]
    assert "core 'm55_he'" in problems[0]


def test_shared_cmakelists_flagged_assertion_2(tmp_path):
    # Assertion 2 (no sharing) on the #1275 trap: m55_hp is ADDED pointing at
    # the same `./src` the already-wired m55_he uses, and the root
    # CMakeLists.txt is still baked `--core m55_he`.
    #
    # NOTE (mutation-tested): assertion 1 also fires on THIS shape -- the
    # m55_hp core resolves to a literal that is not its own. Assertion 2 is
    # independently load-bearing only when the shared CMakeLists bakes no
    # `--core` literal at all, which assertion 1 structurally cannot check.
    # Both are asserted here because the gate must catch the sharing itself,
    # not merely a literal mismatch that happens to accompany it.
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
    _write(ex / "CMakeLists.txt", _CMAKE_TMPL.format(core="m55_he"))

    problems = gate.find_problems(tmp_path)
    sharing = [p for p in problems if "shared by" in p]
    assert len(sharing) == 1, problems
    assert "m55_he" in sharing[0] and "m55_hp" in sharing[0]
    assert "2 distinct Zephyr cores" in sharing[0]


def test_shared_cmakelists_with_no_core_literal_flagged_assertion_2_only(
        tmp_path):
    # Assertion 2 standalone (see the gate's module docstring): the
    # shared CMakeLists.txt bakes NO `--core` literal at all, so
    # `_CORE_RE.search` returns None and assertion 1 structurally
    # cannot fire -- only assertion 2 can catch this sharing. No such
    # file exists in the real corpus today, so this is the one shape
    # nothing else exercises.
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
    _write(ex / "CMakeLists.txt", """
        cmake_minimum_required(VERSION 3.20)
        find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
        project(demo LANGUAGES C)
        target_sources(app PRIVATE src/main.c)
        """)

    problems = gate.find_problems(tmp_path)
    assert not any("literal agreement violated" in p for p in problems), problems
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

# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for `--emit kconfig` (#893):

  * Task 1 -- emit-mode wiring + the loud "no workspace" guard.
  * Task 2 -- hermetic symbol projection + envelope shaping (a fake
    symbol list; no kconfiglib / Zephyr installed).
  * cross-repo contract (#893 shared fixture) -- `_envelope()` +
    `_project_symbols()`'s combined key shape matches the canonical
    `tests/fixtures/kconfig-contract/emit-kconfig.golden.json` tan-cli and
    alp-sdk-vscode both test against, so a field rename is caught here
    too, not only by pr-twister's check_emit_kconfig_contract.py.

The workspace-dependent load (Task 3 -- Approach A: a stub `west build
--cmake-only` then read kconfiglib against the real env) has its own
skipif-gated suite: test_emit_kconfig_workspace.py.  The authoritative
verification of that half is the CI schema/smoke contract (Task 4:
scripts/check_emit_kconfig_contract.py, run in the Zephyr-bootstrapped
pr-twister job).

Run locally:

    python -m pytest tests/scripts/test_emit_kconfig.py -v
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import OrchestratorError, load_board_yaml  # noqa: E402
from alp_orchestrate.kconfig_symbols import (  # noqa: E402
    _envelope,
    _project_symbols,
    emit_kconfig,
)


# ---------------------------------------------------------------------
# Task 1: emit-mode wiring + the workspace guard
# ---------------------------------------------------------------------


def test_emit_kconfig_requires_zephyr_base(tmp_path, monkeypatch, capsys) -> None:
    """No ZEPHYR_BASE -> exit(2) + an actionable stderr message, checked
    only AFTER --core resolves to a real Zephyr slice."""
    monkeypatch.delenv("ZEPHYR_BASE", raising=False)
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    with pytest.raises(SystemExit) as exc_info:
        emit_kconfig(project, "m33_sm")
    assert exc_info.value.code == 2
    err = capsys.readouterr().err
    assert "ZEPHYR_BASE" in err
    assert "--emit kconfig" in err


def test_emit_kconfig_unknown_core(tmp_path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    with pytest.raises(OrchestratorError, match="not present"):
        emit_kconfig(project, "does_not_exist")


def test_emit_kconfig_requires_core(tmp_path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    with pytest.raises(OrchestratorError, match="requires --core"):
        emit_kconfig(project, None)


def test_emit_kconfig_rejects_non_zephyr_core(tmp_path) -> None:
    """a55_cluster is a Yocto core with no Zephyr board target -- this
    errors before ever checking for a workspace."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)

    with pytest.raises(OrchestratorError, match="no resolved Zephyr board"):
        emit_kconfig(project, "a55_cluster")


# ---------------------------------------------------------------------
# Task 2: hermetic symbol projection + envelope (no kconfiglib import)
# ---------------------------------------------------------------------


class _FakeNode:
    def __init__(self, prompt=None, help=None):
        self.prompt = prompt
        self.help = help


class _FakeSym:
    """Duck-types kconfiglib.Symbol just enough for `_project_symbols`."""

    def __init__(self, name, type_, nodes, direct_dep="", orig_defaults=()):
        self.name = name
        self.type = type_
        self.nodes = nodes
        self.direct_dep = direct_dep
        self.orig_defaults = list(orig_defaults)


# Small fake type table -- deliberately NOT kconfiglib's real (unstable)
# int values, to prove `_project_symbols` never assumes a specific
# numbering and only ever consults the dict it's handed.
_FAKE_BOOL, _FAKE_INT = 7, 42
_FAKE_TYPE_TO_STR = {_FAKE_BOOL: "bool", _FAKE_INT: "int"}


def test_project_symbols_keeps_only_promptable_and_sorts() -> None:
    syms = [
        _FakeSym(
            "ZEBRA_LOG", _FAKE_BOOL,
            nodes=[_FakeNode(prompt=("Logging", None), help="Enable logging.")],
            direct_dep="LOG_ENABLED",
            orig_defaults=[("n", None)],
        ),
        _FakeSym(
            "HIDDEN_INTERNAL", _FAKE_BOOL,
            nodes=[_FakeNode(prompt=None, help=None)],
        ),
        _FakeSym(
            "ALP_ARENA_KIB", _FAKE_INT,
            nodes=[_FakeNode(prompt=("Arena size (KiB)", None), help=None)],
        ),
    ]

    projected = _project_symbols(syms, type_to_str=_FAKE_TYPE_TO_STR, expr_str=str)

    # Promptless symbol excluded; the two promptable ones sorted by name.
    names = [entry["name"] for entry in projected]
    assert names == ["ALP_ARENA_KIB", "ZEBRA_LOG"]

    zebra = next(e for e in projected if e["name"] == "ZEBRA_LOG")
    assert zebra["type"] == "bool"
    assert zebra["prompt"] == "Logging"
    assert zebra["depends"] == "LOG_ENABLED"
    assert zebra["default"] == "n"
    assert zebra["help"] == "Enable logging."

    arena = next(e for e in projected if e["name"] == "ALP_ARENA_KIB")
    assert arena["type"] == "int"
    assert arena["depends"] == ""
    assert arena["default"] is None
    assert arena["help"] == ""


def test_project_symbols_checks_every_node_not_just_the_first() -> None:
    """Regression (#893): a real Zephyr symbol can be re-declared multiple
    times -- e.g. a board/SoC Kconfig.defconfig fragment overrides just
    the default, with no prompt, and Kconfig.zephyr sources those BEFORE
    the canonical declaration -- so `nodes[0]` alone silently dropped
    real symbols like LOG/SERIAL/MAIN_STACK_SIZE. Only a symbol with NO
    prompt on ANY node stays excluded."""
    syms = [
        _FakeSym(
            "SERIAL", _FAKE_BOOL,
            nodes=[
                _FakeNode(prompt=None, help=None),                    # defconfig override
                _FakeNode(prompt=("Serial drivers", None), help="Enable serial."),
            ],
        ),
        _FakeSym(
            "STILL_HIDDEN", _FAKE_BOOL,
            nodes=[_FakeNode(prompt=None, help=None), _FakeNode(prompt=None, help=None)],
        ),
    ]

    projected = _project_symbols(syms, type_to_str=_FAKE_TYPE_TO_STR, expr_str=str)

    names = [entry["name"] for entry in projected]
    assert names == ["SERIAL"]
    assert projected[0]["prompt"] == "Serial drivers"
    assert projected[0]["help"] == "Enable serial."


def test_envelope_shape() -> None:
    symbols = [{"name": "LOG", "type": "bool", "prompt": "Logging",
                "depends": "", "default": "n", "help": ""}]
    envelope = _envelope("alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33",
                         "m33_sm", symbols)

    assert envelope["schemaVersion"] == 1
    assert envelope["board"] == "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"
    assert envelope["core"] == "m33_sm"
    assert envelope["symbols"] == symbols


# ---------------------------------------------------------------------
# Cross-repo contract regression (#893): tan-cli's `parse_kconfig` and
# alp-sdk-vscode's `kconfigSymbolsFromEnvelope` both test their own parsers
# against tests/fixtures/kconfig-contract/emit-kconfig.golden.json. Pin the
# SDK's own producer against the SAME file so a field rename is caught in
# this fast hermetic gate, not only in pr-twister.
# ---------------------------------------------------------------------

_GOLDEN = (Path(__file__).resolve().parent.parent / "fixtures"
           / "kconfig-contract" / "emit-kconfig.golden.json")


def test_envelope_and_symbols_match_golden_key_sets() -> None:
    golden = json.loads(_GOLDEN.read_text(encoding="utf-8"))

    syms = [
        _FakeSym(
            entry["name"], _FAKE_BOOL,
            nodes=[_FakeNode(prompt=(entry["prompt"], None), help=entry["help"])],
            direct_dep=entry["depends"],
            orig_defaults=([(entry["default"], None)]
                            if entry["default"] is not None else []),
        )
        for entry in golden["symbols"]
    ]
    projected = _project_symbols(syms, type_to_str=_FAKE_TYPE_TO_STR, expr_str=str)
    envelope = _envelope(golden["board"], golden["core"], projected)

    assert set(envelope.keys()) == set(golden.keys())
    golden_symbol_keys = set(golden["symbols"][0].keys())
    for sym in projected:
        assert set(sym.keys()) == golden_symbol_keys

# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_agents_md_generators.py.

Issue #1264: AGENTS.md hand-writes ALP_E1M_*/ALP_E1M_X_* identifiers and
--emit choice lists nothing checked against the real generators.  These
tests lock down that a missing anchor is a FAILURE, not a silent pass,
that the identifier check reuses check_doc_drift.py's _is_known() (not a
re-implementation), and that its _ALLOWLIST escape hatch actually works --
alongside the ordinary seeded-violation cases.

Run locally:

    python -m pytest tests/scripts/test_check_agents_md_generators.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_agents_md_generators as gate  # noqa: E402

_AGENTS_MD = """\
# AGENTS.md

Identifiers: `ALP_E1M_I2C0`, `ALP_E1M_PWM3`, or the `ALP_E1M_X_*` family.

- `scripts/alp_project.py --emit {zephyr-conf,cmake-args}`
- `python -m alp_orchestrate --emit {system-manifest,build-plan}`
"""

_E1M_PINOUT_H = """\
#define ALP_E1M_I2C0 0u
#define ALP_E1M_PWM3 3u
"""

_E1M_X_PINOUT_H = """\
#define ALP_E1M_X_I2C0 0u
"""

_ALP_PROJECT_PY = """\
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--emit", choices=["zephyr-conf", "cmake-args"])
"""

_ALP_ORCHESTRATE_CLI_PY = """\
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--emit", choices=["system-manifest", "build-plan"])
"""


def _scaffold(root: Path, agents_md: str = _AGENTS_MD) -> None:
    (root / "AGENTS.md").write_text(agents_md, encoding="utf-8")
    include_dir = root / "include" / "alp"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "e1m_pinout.h").write_text(_E1M_PINOUT_H, encoding="utf-8")
    (include_dir / "e1m_x_pinout.h").write_text(_E1M_X_PINOUT_H, encoding="utf-8")
    scripts_dir = root / "scripts"
    orchestrate_dir = scripts_dir / "alp_orchestrate"
    orchestrate_dir.mkdir(parents=True, exist_ok=True)
    (scripts_dir / "alp_project.py").write_text(_ALP_PROJECT_PY, encoding="utf-8")
    (orchestrate_dir / "cli.py").write_text(_ALP_ORCHESTRATE_CLI_PY, encoding="utf-8")


def test_clean_tree_passes(tmp_path: Path):
    _scaffold(tmp_path)
    assert gate.find_problems(tmp_path) == []


def test_dead_identifier_fails(tmp_path: Path):
    _scaffold(tmp_path, _AGENTS_MD.replace("ALP_E1M_PWM3", "ALP_E1M_PWM99"))
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "AGENTS.md:3" in problems[0]
    assert "ALP_E1M_PWM99" in problems[0]


def test_wildcard_prefix_with_real_match_passes(tmp_path: Path):
    """ALP_E1M_X_* is a real prefix (ALP_E1M_X_I2C0 exists) -- must pass;
    removing the only real ALP_E1M_X_* define must flip the SAME AGENTS.md
    text to a failure, proving this actually looks for a real prefix
    match rather than passing unconditionally."""
    _scaffold(tmp_path)
    assert gate.find_problems(tmp_path) == []
    (tmp_path / "include" / "alp" / "e1m_x_pinout.h").write_text(
        "// no ALP_E1M_X_* defines left\n", encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("ALP_E1M_X_" in p for p in problems)


def test_wildcard_prefix_with_no_real_match_fails(tmp_path: Path):
    """A family wildcard whose prefix matches NOTHING real is still drift."""
    agents = _AGENTS_MD.replace("ALP_E1M_X_*", "ALP_E1M_ZZZ_*")
    _scaffold(tmp_path, agents)
    problems = gate.find_problems(tmp_path)
    assert any("ALP_E1M_ZZZ_" in p for p in problems)


def test_allowlisted_identifier_counter_example_passes(tmp_path: Path, monkeypatch):
    """A deliberate non-real identifier AGENTS.md cites as a counter-
    example (the way it already does for `alp_lsm6dso_` in prose) must be
    escapable via _ALLOWLIST -- without it, the same text is drift."""
    agents = _AGENTS_MD + "\n`ALP_E1M_GHOST9` does not exist.\n"
    _scaffold(tmp_path, agents)
    problems = gate.find_problems(tmp_path)
    assert any("ALP_E1M_GHOST9" in p for p in problems)
    monkeypatch.setattr(gate, "_ALLOWLIST", {"ALP_E1M_GHOST9"})
    assert gate.find_problems(tmp_path) == []


def test_emit_choice_not_in_alp_project_fails(tmp_path: Path):
    agents = _AGENTS_MD.replace(
        "--emit {zephyr-conf,cmake-args}", "--emit {zephyr-conf,cmake-args,ghost-emit}")
    _scaffold(tmp_path, agents)
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "ghost-emit" in problems[0]
    assert "scripts/alp_project.py" in problems[0]


def test_emit_choice_not_in_orchestrate_fails(tmp_path: Path):
    agents = _AGENTS_MD.replace(
        "--emit {system-manifest,build-plan}",
        "--emit {system-manifest,build-plan,ghost-emit}")
    _scaffold(tmp_path, agents)
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "ghost-emit" in problems[0]
    assert "alp_orchestrate" in problems[0]


def test_missing_emit_anchor_fails_loudly_not_silently(tmp_path: Path):
    """If AGENTS.md no longer names the generator's invocation string at
    all, that must be a hard failure (the check can no longer verify
    anything), never a quiet pass."""
    agents = _AGENTS_MD.replace("scripts/alp_project.py --emit", "some_other_tool --emit")
    _scaffold(tmp_path, agents)
    problems = gate.find_problems(tmp_path)
    assert any("could not find the literal" in p for p in problems)


def test_missing_agents_md_fails_loudly(tmp_path: Path):
    (tmp_path / "include" / "alp").mkdir(parents=True)
    problems = gate.find_problems(tmp_path)
    assert any("AGENTS.md" in p and "not found" in p for p in problems)


def test_missing_pinout_header_fails_loudly(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "include" / "alp" / "e1m_x_pinout.h").unlink()
    problems = gate.find_problems(tmp_path)
    assert any("e1m_x_pinout.h" in p and "not found" in p for p in problems)

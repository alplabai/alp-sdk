# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_cc3501e_bridge_copies.py (issue #2163).

The gate pins one invariant: every `cc3501e_bridge.{c,h}` copied into an
example is byte-identical to the canonical pair, unless the gate itself
declares that copy divergent and says why.
"""
from pathlib import Path

import pytest

import check_cc3501e_bridge_copies as gate
from check_cc3501e_bridge_copies import BRIDGE_FILES, CANONICAL_DIR, find_problems, resync

CANONICAL_C = "/* canonical bring-up: WIFI_EN + nRESET pads, RX_SAMPLE_DLY poke */\n"
CANONICAL_H = "#define CC3501E_BRIDGE_SPI_FREQ_HZ 25000000u\n"


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def _canonical(root: Path) -> None:
    _write(root, f"{CANONICAL_DIR}/cc3501e_bridge.c", CANONICAL_C)
    _write(root, f"{CANONICAL_DIR}/cc3501e_bridge.h", CANONICAL_H)


def _copy(root: Path, rel_dir: str, *, c: str = CANONICAL_C, h: str = CANONICAL_H) -> None:
    _write(root, f"{rel_dir}/cc3501e_bridge.c", c)
    _write(root, f"{rel_dir}/cc3501e_bridge.h", h)


@pytest.fixture
def no_divergences(monkeypatch):
    """A synthetic tree declares its own exemptions, not the repo's."""
    monkeypatch.setattr(gate, "DIVERGENT_COPIES", {})


def test_identical_copies_pass(tmp_path, no_divergences):
    _canonical(tmp_path)
    _copy(tmp_path, "examples/aen/aen-cc3501e-gpio/src")
    _copy(tmp_path, "examples/connectivity/mqtt-telemetry/src")
    assert find_problems(tmp_path) == []


def test_drifted_copy_is_reported(tmp_path, no_divergences):
    """The #2163 failure: one copy takes a silicon fix, the others do not."""
    _canonical(tmp_path)
    _copy(tmp_path, "examples/aen/aen-cc3501e-gpio/src")
    _copy(
        tmp_path,
        "examples/connectivity/mqtt-telemetry/src",
        c=CANONICAL_C.replace("RX_SAMPLE_DLY poke", "no RX_SAMPLE_DLY poke"),
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "examples/connectivity/mqtt-telemetry/src/cc3501e_bridge.c" in problems[0]
    # The report carries the drift itself, not just the filename.
    assert "no RX_SAMPLE_DLY poke" in problems[0]


def test_half_copied_pair_is_reported(tmp_path, no_divergences):
    _canonical(tmp_path)
    _write(tmp_path, "examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.c", CANONICAL_C)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "cc3501e_bridge.h" in problems[0]


def test_missing_canonical_pair_is_reported(tmp_path, no_divergences):
    """A vanished template must fail loudly, not silently grade nothing."""
    _copy(tmp_path, "examples/aen/aen-cc3501e-gpio/src")
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert CANONICAL_DIR in problems[0]


def test_declared_divergence_passes(tmp_path, monkeypatch):
    monkeypatch.setattr(
        gate, "DIVERGENT_COPIES",
        {"examples/peripheral-io/alp-console/src": "cold-boot soak instead"},
    )
    _canonical(tmp_path)
    _copy(tmp_path, "examples/peripheral-io/alp-console/src", c="/* soak */\n")
    assert find_problems(tmp_path) == []


def test_divergence_that_came_back_in_line_is_reported(tmp_path, monkeypatch):
    """An exemption nobody removes is where the next drift hides."""
    monkeypatch.setattr(
        gate, "DIVERGENT_COPIES",
        {"examples/peripheral-io/alp-console/src": "cold-boot soak instead"},
    )
    _canonical(tmp_path)
    _copy(tmp_path, "examples/peripheral-io/alp-console/src")
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "DIVERGENT_COPIES" in problems[0]


def test_stale_divergence_entry_is_reported(tmp_path, monkeypatch):
    monkeypatch.setattr(
        gate, "DIVERGENT_COPIES", {"examples/gone/src": "example was deleted"},
    )
    _canonical(tmp_path)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "examples/gone/src" in problems[0]


def test_fix_resyncs_every_graded_copy(tmp_path, monkeypatch):
    monkeypatch.setattr(
        gate, "DIVERGENT_COPIES",
        {"examples/peripheral-io/alp-console/src": "cold-boot soak instead"},
    )
    _canonical(tmp_path)
    _copy(tmp_path, "examples/aen/aen-cc3501e-gpio/src", c="/* drifted */\n")
    _copy(tmp_path, "examples/peripheral-io/alp-console/src", c="/* soak */\n")

    fixed = resync(tmp_path)

    assert fixed == ["examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.c"]
    assert find_problems(tmp_path) == []
    # --fix must not touch a copy that is divergent on purpose.
    console = tmp_path / "examples/peripheral-io/alp-console/src/cc3501e_bridge.c"
    assert console.read_text(encoding="utf-8") == "/* soak */\n"


def test_repo_tree_is_in_sync():
    """The gate's own subject: the real examples/ tree passes today."""
    assert find_problems(gate.REPO) == []


def test_every_bridge_file_name_is_graded(tmp_path, no_divergences):
    """Both halves are compared -- a .h-only drift must fail too."""
    _canonical(tmp_path)
    _copy(tmp_path, "examples/aen/aen-cc3501e-gpio/src", h="#define CC3501E_BRIDGE_SPI_FREQ_HZ 1u\n")
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert problems[0].startswith("examples/aen/aen-cc3501e-gpio/src/cc3501e_bridge.h")
    assert set(BRIDGE_FILES) == {"cc3501e_bridge.c", "cc3501e_bridge.h"}

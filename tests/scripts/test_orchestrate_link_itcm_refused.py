# SPDX-License-Identifier: Apache-2.0
"""`diagnostics.link: itcm` (tan-cli#1350) is implemented by tan's planner only
(ADR-0026).  alp-sdk's own `alp_orchestrate` must refuse it loudly instead of
silently emitting an MRAM-linked image."""

from __future__ import annotations

import textwrap
from pathlib import Path

import pytest
from alp_orchestrate.loader import load_board_yaml
from alp_orchestrate.models import OrchestratorError

_BOARD = """
name: link-itcm
som:
  sku: E1M-AEN801
cores:
  m55_he:
    os: zephyr
    app: ./he
diagnostics:
  link: {value}
"""


def _board(tmp_path: Path, value: str) -> Path:
    path = tmp_path / "board.yaml"
    path.write_text(textwrap.dedent(_BOARD.format(value=value)).lstrip("\n"), encoding="utf-8")
    return path


def test_itcm_is_refused_with_a_message_naming_tan(tmp_path):
    with pytest.raises(OrchestratorError, match=r"implemented by `tan build` only"):
        load_board_yaml(_board(tmp_path, "itcm"))


def test_auto_loads_as_before(tmp_path):
    project = load_board_yaml(_board(tmp_path, "auto"))
    assert project.diagnostics["link"] == "auto"

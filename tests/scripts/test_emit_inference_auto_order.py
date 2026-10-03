# SPDX-License-Identifier: Apache-2.0
"""`inference.auto_order` (SoM preset) reaches the build as -DALP_SDK_INFERENCE_AUTO_ORDER."""
from __future__ import annotations

import sys
import types
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from alp_orchestrate import kconfig as K  # noqa: E402
from alp_orchestrate.models import Slice  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT as _MR  # noqa: E402


def _args(sku: str) -> str:
    som = yaml.safe_load((_MR / "e1m_modules" / f"{sku}.yaml").read_text(encoding="utf-8"))
    proj = types.SimpleNamespace(som_preset=som, sku=sku, board_name=None, libraries=None,
                                 effective_metadata_root=lambda: _MR)
    return K._slice_cmake_args(proj, Slice(core_id="a55_cluster", os="yocto"))


@pytest.mark.parametrize("sku,order", [("E1M-V2M103", "deepx_dxm1,drpai,cpu"),
                                       ("E1M-V2N101", "drpai,cpu")])
def test_auto_order_is_emitted(sku, order):
    assert f"-DALP_SDK_INFERENCE_AUTO_ORDER={order}" in _args(sku).splitlines()


def test_sku_without_auto_order_emits_nothing():
    assert "AUTO_ORDER" not in _args("E1M-AEN301")

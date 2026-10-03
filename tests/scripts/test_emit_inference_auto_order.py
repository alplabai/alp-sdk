# SPDX-License-Identifier: Apache-2.0
"""`inference.auto_order` (SoM preset) reaches the Yocto build through the real
emit path: `alp_project.py --emit yocto-conf` and the build plan's local.conf."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import _run_loader, _write_board  # noqa: E402


def _yocto_conf(sku: str) -> str:
    with tempfile.TemporaryDirectory() as td:
        path = _write_board(Path(td), f"""
            som:
              sku: {sku}
            cores:
              a55_cluster:
                os: yocto
                image: alp-image-edge
        """)
        rv = _run_loader(input_path=path, emit="yocto-conf", core="a55_cluster")
    assert rv.returncode == 0, rv.stderr
    return rv.stdout


@pytest.mark.parametrize("sku,order", [("E1M-V2M103", "deepx_dxm1,drpai,cpu"),
                                       ("E1M-V2N101", "drpai,cpu")])
def test_auto_order_reaches_yocto_local_conf(sku, order):
    assert f'ALP_SDK_INFERENCE_AUTO_ORDER ?= "{order}"' in _yocto_conf(sku).splitlines()


def test_recipe_forwards_the_variable_to_cmake():
    root = Path(__file__).resolve().parents[2]
    bb = (root / "meta-alp-sdk/recipes-core/alp-sdk/alp-sdk_0.6.bb").read_text(encoding="utf-8")
    assert "-DALP_SDK_INFERENCE_AUTO_ORDER=" in bb
    assert 'd.getVar(\'ALP_SDK_INFERENCE_AUTO_ORDER\')' in bb

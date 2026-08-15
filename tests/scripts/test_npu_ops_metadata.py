# SPDX-License-Identifier: Apache-2.0
"""`metadata/npu_ops/*.json` -- the per-NPU op-support data asset."""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


@pytest.mark.parametrize("backend", ["ethos_u", "drpai", "deepx_dxm1"])
def test_op_support_file_shape(backend):
    data = json.loads((_META / "npu_ops" / f"{backend}.json").read_text("utf-8"))
    assert data["backend"] == backend
    assert data["version"] and data["source"]
    assert isinstance(data["supported_ops"], list) and data["supported_ops"]
    # Op names are TFLite builtin identifiers (UPPER_SNAKE), deduped
    assert all(op == op.upper() for op in data["supported_ops"])
    assert len(data["supported_ops"]) == len(set(data["supported_ops"]))
    # Every NPU must at least run the compute-dominant ops the estimator scores
    assert {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"} <= set(data["supported_ops"])


def test_no_cpu_op_support_file():
    """`cpu` is the universal fallback -- it supports everything and is never
    looked up, so a `cpu.json` would be dead data that could drift."""
    assert not (_META / "npu_ops" / "cpu.json").exists()

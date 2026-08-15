# SPDX-License-Identifier: Apache-2.0
"""`metadata/npu_ops/*.json` -- the per-NPU op-support data asset."""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


#: Each backend's compiler ingests exactly ONE source format, so its op list
#: must be spelled in that format's vocabulary. Mirrors the adapters'
#: `accepts()`: ethos_u/cpu -> "tflite", drpai/deepx_dxm1 -> "onnx".
_EXPECTED_NAMESPACE = {"ethos_u": "tflite", "drpai": "onnx", "deepx_dxm1": "onnx"}

#: The compute-dominant ops the estimator scores, per vocabulary. ONNX has no
#: DEPTHWISE_CONV_2D -- a depthwise convolution is `Conv` with group == C.
_REQUIRED_OPS = {
    "tflite": {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"},
    "onnx": {"Conv", "Gemm"},
}


@pytest.mark.parametrize("backend", ["ethos_u", "drpai", "deepx_dxm1"])
def test_op_support_file_shape(backend):
    data = json.loads((_META / "npu_ops" / f"{backend}.json").read_text("utf-8"))
    assert data["backend"] == backend
    assert data["version"] and data["source"]
    assert isinstance(data["supported_ops"], list) and data["supported_ops"]
    assert len(data["supported_ops"]) == len(set(data["supported_ops"]))

    ns = data["op_namespace"]
    assert ns == _EXPECTED_NAMESPACE[backend], (
        f"{backend}.json declares op_namespace={ns!r}, but its compiler ingests "
        f"{_EXPECTED_NAMESPACE[backend]!r} -- see the adapter's accepts()"
    )
    assert _REQUIRED_OPS[ns] <= set(data["supported_ops"])

    if ns == "tflite":
        # TFLite builtins are UPPER_SNAKE
        assert all(op == op.upper() for op in data["supported_ops"])
    else:
        # ONNX operators are CamelCase and are NOT upper-snake; this is the
        # assertion that would have caught the whole list being spelled in the
        # wrong vocabulary.
        assert not any(op == op.upper() for op in data["supported_ops"])


def test_no_cpu_op_support_file():
    """`cpu` is the universal fallback -- it supports everything and is never
    looked up, so a `cpu.json` would be dead data that could drift."""
    assert not (_META / "npu_ops" / "cpu.json").exists()


def test_every_npu_ops_filename_matches_its_backend_field():
    """A fourth backend added later inherits no per-name parametrize case, so
    assert over the DIRECTORY, not a hard-coded list -- otherwise
    `ethos_u_v2.json` carrying `"backend": "ethos_u"` passes every gate."""
    for path in sorted((_META / "npu_ops").glob("*.json")):
        data = json.loads(path.read_text("utf-8"))
        assert data["backend"] == path.stem, (
            f"{path.name} declares backend={data['backend']!r}"
        )

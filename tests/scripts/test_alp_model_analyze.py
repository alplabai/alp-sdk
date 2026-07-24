"""alp_model.analyze — static fit/perf analyzer."""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"
_FIXTURE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
_ONNX_FIXTURE = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


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


def _op(name, inputs, outputs):
    from alp_model.tensorio import OpDesc, TensorDesc
    def td(shape, const=False, dtype="int8", nb=None):
        n = (nb if nb is not None else max(1, __import__("math").prod(shape)))
        return TensorDesc(shape=list(shape), dtype=dtype, nbytes=n, is_const=const)
    return OpDesc(op=name,
                  inputs=[td(*i) if isinstance(i, tuple) else td(i) for i in inputs],
                  outputs=[td(o) for o in outputs])


def test_estimate_macs_fully_connected():
    from alp_model.analyze import estimate_macs
    # activation [1,4], weights [2,4] const -> 1 * units(2) * in_f(4) = 8
    op = _op("FULLY_CONNECTED", [([1, 4],), ([2, 4], True)], [[1, 2]])
    assert estimate_macs([op]) == 8


def test_estimate_macs_conv2d():
    from alp_model.analyze import estimate_macs
    # out [1,8,8,16], weights [16,3,3,4] -> 8*8*16*3*3*4 = 36864
    op = _op("CONV_2D", [([1, 10, 10, 4],), ([16, 3, 3, 4], True)], [[1, 8, 8, 16]])
    assert estimate_macs([op]) == 36864


def test_estimate_macs_unknown_op_is_zero():
    from alp_model.analyze import estimate_macs
    assert estimate_macs([_op("CUSTOM_THING", [[1, 4]], [[1, 4]])]) == 0


def test_estimate_peak_sram_excludes_constants_and_rounds_up():
    from alp_model.analyze import estimate_peak_sram_kib
    # activations: in 4B + out 2B = 6B -> ceil(6/1024) = 1 KiB; weights (const) excluded
    op = _op("FULLY_CONNECTED", [([1, 4], False, "int8", 4), ([2, 4], True, "int8", 8)],
             [[1, 2]])
    # give the output an explicit 2-byte size
    from alp_model.tensorio import OpDesc, TensorDesc
    op = OpDesc(op="FULLY_CONNECTED",
                inputs=[TensorDesc([1, 4], "int8", 4, False),
                        TensorDesc([2, 4], "int8", 8, True)],
                outputs=[TensorDesc([1, 2], "int8", 2, False)])
    assert estimate_peak_sram_kib([op]) == 1


def test_op_coverage_partial():
    from alp_model.analyze import op_coverage
    ops = [_op("CONV_2D", [[1, 4]], [[1, 4]]), _op("CUSTOM_X", [[1, 4]], [[1, 4]])]
    pct, unsupported = op_coverage(ops, {"CONV_2D"})
    assert pct == 50.0 and unsupported == ["CUSTOM_X"]


def test_latency_prefers_mac_per_cycle_then_tops():
    from alp_model.analyze import npu_mac_per_s
    assert npu_mac_per_s({"mac_per_cycle": 256, "freq_mhz": 400}) == 256 * 400e6
    assert npu_mac_per_s({"tops": 4}) == 4e12 / 2
    assert npu_mac_per_s({"gops": 200}) == 200e9 / 2
    assert npu_mac_per_s({}) is None


def test_analyze_model_tflite_structural_invariants():
    pytest.importorskip("tflite")
    from alp_model.analyze import analyze_model
    res = analyze_model(_FIXTURE, "E1M-AEN801", metadata_root=_META)
    assert res.sku == "E1M-AEN801"
    assert res.backends, "expected at least one backend"
    # cpu is the universal fallback: always present, always fits, source static
    cpu = [b for b in res.backends if b.backend == "cpu"]
    assert len(cpu) == 1 and cpu[0].verdict == "fits"
    assert all(b.source == "static" for b in res.backends)
    # AEN701 -> alif e7 exposes an Ethos-U backend
    assert any(b.backend == "ethos_u" for b in res.backends)
    # tiny FC model, all-supported, budget unknown -> ethos verdict never no-fit
    for b in res.backends:
        if b.backend == "ethos_u":
            assert b.verdict in ("fits", "cpu-fallback")
            assert b.est_sram_kib == 1        # 6B activations -> 1 KiB
            assert b.budget_sram_kib is None   # e7 arena is a 0 placeholder


def test_analyze_model_rejects_non_tflite():
    from alp_model.analyze import analyze_model, UnsupportedModelError
    with pytest.raises(UnsupportedModelError):
        analyze_model(_ONNX_FIXTURE, "E1M-AEN801", metadata_root=_META)


def test_analyze_model_drpai_backend_present():
    pytest.importorskip("tflite")
    from alp_model.analyze import analyze_model
    res = analyze_model(_FIXTURE, "E1M-V2N101", metadata_root=_META)
    assert any(b.backend == "drpai" for b in res.backends)

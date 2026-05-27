# scripts/alp_model/tensorio.py
"""Extract model input/output tensor descriptors for the .alpmodel manifest.

Parses a TFLite flatbuffer's first subgraph I/O into Stage-1a `Tensor` records
(dtype/rank/shape/scale/zp -> mirrors alp_inference_tensor_t). Best-effort:
returns ([], []) when the source isn't a .tflite, the `tflite` reader (from the
`model-compile` extra) isn't installed, or the bytes don't parse -- the model
still packages, just without I/O metadata (compile-what's-available)."""
from __future__ import annotations
from pathlib import Path
from .manifest import Tensor


def extract_io(source: Path, *, raw: bytes | None = None) -> tuple[list[Tensor], list[Tensor]]:
    if source.suffix.lower() != ".tflite":
        return [], []                       # ONNX I/O extraction is a later follow-up
    try:
        import tflite
    except ImportError:
        return [], []                       # parser not installed -> skip
    # Reuse the caller's already-read bytes when provided (build_model reads the
    # source once for the manifest sha); an OSError on our own read is a real
    # failure, not a parse problem.
    if raw is None:
        raw = source.read_bytes()
    try:
        model = tflite.Model.GetRootAs(raw, 0)
        if model.SubgraphsLength() == 0:
            return [], []
        g = model.Subgraphs(0)
        dtype_map = {
            tflite.TensorType.FLOAT32: "f32", tflite.TensorType.FLOAT16: "f16",
            tflite.TensorType.INT32: "int32", tflite.TensorType.UINT8: "uint8",
            tflite.TensorType.INT16: "int16", tflite.TensorType.INT8: "int8",
        }
        ins = [_describe(g, g.Inputs(i), dtype_map) for i in range(g.InputsLength())]
        outs = [_describe(g, g.Outputs(i), dtype_map) for i in range(g.OutputsLength())]
        return ins, outs
    except Exception:
        return [], []                       # malformed / unexpected schema -> no metadata


def _describe(g, idx: int, dtype_map: dict[int, str]) -> Tensor:
    ten = g.Tensors(idx)
    shape = [int(ten.Shape(i)) for i in range(ten.ShapeLength())]
    qp = ten.Quantization()
    scale = float(qp.Scale(0)) if qp is not None and qp.ScaleLength() else 0.0
    zp = int(qp.ZeroPoint(0)) if qp is not None and qp.ZeroPointLength() else 0
    # Honest marker for an unmapped TFLite dtype (e.g. INT64/BOOL) rather than a
    # silent wrong "f32" -- the SDK Tensor.dtype vocabulary has no such types.
    return Tensor(dtype=dtype_map.get(ten.Type(), f"tflite:{ten.Type()}"), rank=len(shape),
                  shape=shape, scale=scale, zp=zp)

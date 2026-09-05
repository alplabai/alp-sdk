# scripts/alp_model/convert.py
"""Convert a source model to ONNX for the prep pipeline. `.onnx` passes through;
`.tflite` (the SDK's native format) converts via tf2onnx. The conversion deps
(tf2onnx + tensorflow) are the heavy `model-convert` extra, lazy-imported only
for the .tflite path so importing this module stays cheap."""
from __future__ import annotations

from pathlib import Path


class ConvertError(Exception):
    """A source model could not be converted to ONNX (unsupported format,
    missing conversion deps, or a tf2onnx failure)."""


def to_onnx(src: Path, dst: Path, *, opset: int = 13) -> Path:
    suffix = src.suffix.lower()
    if suffix == ".onnx":
        return src                                  # already ONNX — no conversion
    if suffix != ".tflite":
        raise ConvertError(
            f"unsupported model format {src.suffix}; expected .onnx or .tflite")
    try:
        import onnx
        import tf2onnx
    except ImportError as exc:
        raise ConvertError(
            ".tflite input needs the conversion toolchain — "
            "pip install alp-sdk-cli[model-convert]") from exc
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        model_proto, _ = tf2onnx.convert.from_tflite(str(src), opset=opset)
        onnx.save(model_proto, str(dst))
    except Exception as exc:                         # tf2onnx raises broad types
        if dst.is_file():
            dst.unlink()                             # no partial/garbage ONNX
        raise ConvertError(
            f"TFLite->ONNX conversion failed for {src.name}: {exc}") from exc
    return dst

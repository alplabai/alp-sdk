# scripts/alp_model/adapters/deepx.py
"""DEEPX DX-M1 detect-and-skip adapter.

The DEEPX SDK `dx_com` model compiler is proprietary and not bundled.
is_available() is True only when `dx_com` is on PATH or ALP_DEEPX_SDK_HOME points
at an install; otherwise the deepx_dxm1 target is a coverage skip. Real compile
lands in Stage 2."""
from __future__ import annotations
import os
import shutil
from pathlib import Path
from . import CompilerAdapter, Blob


class DeepxAdapter(CompilerAdapter):
    backend = "deepx_dxm1"

    def is_available(self) -> bool:
        if shutil.which("dx_com"):
            return True
        root = os.environ.get("ALP_DEEPX_SDK_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format in ("onnx", "tflite")     # DEEPX dx_com front-ends

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        raise NotImplementedError("real DEEPX compile lands in Stage 2")

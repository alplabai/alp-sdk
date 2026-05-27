# scripts/alp_model/adapters/drpai.py
"""Renesas DRP-AI detect-and-skip adapter.

The DRP-AI TVM compiler (`vendors/renesas-rzv2n/rzv_drp-ai_tvm/`) is a large
proprietary toolchain that is not bundled. is_available() is True only when its
install root is on the environment (ALP_DRPAI_TVM_HOME); otherwise the build
records the drpai target as a coverage skip. Real compile lands in Stage 2."""
from __future__ import annotations
import os
from pathlib import Path
from . import CompilerAdapter, Blob


class DrpaiAdapter(CompilerAdapter):
    backend = "drpai"

    def is_available(self) -> bool:
        root = os.environ.get("ALP_DRPAI_TVM_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format in ("onnx", "tflite")     # DRP-AI TVM front-ends

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        raise NotImplementedError("real DRP-AI compile lands in Stage 2")

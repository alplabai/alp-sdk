# scripts/alp_model/adapters/deepx.py
"""DEEPX DX-M1 detect-and-skip adapter (host model compiler).

DEEPX's compiler ships as the proprietary, license-gated `dx-com` Python wheel
(console script `dxcom`; `dx-com` 2.3.0 verified -- Linux x86_64, Python
3.8-3.12, ONNX frontend). It compiles an ONNX model into a DEEPX NPU binary:
    dxcom -m <model.onnx> -c <config.json> -o <out_dir>
with a per-model JSON config + a calibration dataset (post-training quant). The
wheel is not redistributable, so it is NOT bundled. is_available() is True only
when `dxcom` is on PATH or ALP_DEEPX_SDK_HOME points at an install; otherwise the
deepx_dxm1 target is a coverage skip. The real compile (plus plumbing the JSON
config + calibration through board.yaml `models:`) and the dx_rt A55/PCIe runtime
land in Stage 2."""
from __future__ import annotations
import os
import shutil
from pathlib import Path
from . import CompilerAdapter, Blob


class DeepxAdapter(CompilerAdapter):
    backend = "deepx_dxm1"

    def is_available(self) -> bool:
        if shutil.which("dxcom"):            # the dx-com wheel's console script
            return True
        root = os.environ.get("ALP_DEEPX_SDK_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format == "onnx"          # dxcom is an ONNX frontend

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        raise NotImplementedError("real DEEPX (dxcom) compile lands in Stage 2")

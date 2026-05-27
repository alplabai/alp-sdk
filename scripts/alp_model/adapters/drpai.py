# scripts/alp_model/adapters/drpai.py
"""Renesas DRP-AI detect-and-skip adapter (host model compiler).

DRP-AI uses the OPEN-SOURCE DRP-AI TVM toolchain
(github.com/renesas-rz/rzv_drp-ai_tvm -- build-from-source or Docker, NOT
license-gated): a TVM-based compiler that takes an ONNX model and emits
"Runtime Model Data" (a directory) deployed to the RZ/V2N A55 alongside the
DRP-AI TVM runtime library + a C++ inference app. The toolchain is large and not
bundled. is_available() is True only when its built install root is on the
environment (ALP_DRPAI_TVM_HOME); otherwise the drpai target is a coverage skip.
The real compile + the A55 DRP-AI runtime backend land in Stage 2."""
from __future__ import annotations
import os
from pathlib import Path
from . import CompilerAdapter, Blob


class DrpaiAdapter(CompilerAdapter):
    backend = "drpai"
    requires_compile_opts = True

    def is_available(self) -> bool:
        root = os.environ.get("ALP_DRPAI_TVM_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format == "onnx"          # DRP-AI TVM ingests ONNX

    def compile(self, source: Path, *, accel_config: str, out_dir: Path, opts: dict | None = None) -> Blob:
        raise NotImplementedError("real DRP-AI (TVM) compile lands in Stage 2")

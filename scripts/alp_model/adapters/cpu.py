# scripts/alp_model/adapters/cpu.py
"""CPU/TFLM passthrough adapter: the model runs as-is on reference kernels."""
from __future__ import annotations
from pathlib import Path
from . import CompilerAdapter, Blob


class CpuAdapter(CompilerAdapter):
    backend = "cpu"

    def is_available(self) -> bool:
        return True              # always available; no external tool

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        payload = source.read_bytes()
        return Blob(format="tflite", payload=payload, arena_bytes=0,
                    compiler_version="passthrough")

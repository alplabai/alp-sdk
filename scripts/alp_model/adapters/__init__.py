# scripts/alp_model/adapters/__init__.py
"""Compiler-adapter interface: one adapter per backend toolchain."""
from __future__ import annotations
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Blob:
    """One compiled artifact + the manifest metadata the writer needs."""
    format: str                 # vela_tflite | tflite | drpai_dir | dxnn
    payload: bytes
    arena_bytes: int = 0
    compiler_version: str = ""
    req_sram_kib: int = 0


class CompilerAdapter(ABC):
    backend: str                # cpu | ethos_u | drpai | deepx_dxm1

    @abstractmethod
    def is_available(self) -> bool:
        """True if this backend's compiler is installed/usable on this host."""

    @abstractmethod
    def accepts(self, src_format: str) -> bool:
        """True if this adapter can consume the given source format (onnx|tflite)."""

    @abstractmethod
    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        """Compile @source for @accel_config; return the Blob."""

# scripts/alp_model/adapters/__init__.py
"""Compiler-adapter interface: one adapter per backend toolchain."""
from __future__ import annotations
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

from alp_project_loader import TargetSpec  # scripts/ is always on sys.path (see build.py)


@dataclass
class Blob:
    """One compiled artifact + the manifest metadata the writer needs."""
    format: str                 # one of manifest.VALID_BLOB_FORMATS
                                 # (vela_tflite | tflite | drpai_dir | dxnn | onnx)
    payload: bytes
    arena_bytes: int = 0
    compiler_version: str = ""
    req_sram_kib: int = 0


class CompilerAdapter(ABC):
    backend: str                # cpu | ethos_u | drpai | deepx_dxm1
    # True for backends that need a per-model compile config the SDK can't
    # derive (DEEPX JSON+calibration, DRP-AI spec). build_model records a
    # "no compile config" coverage skip for these when no opts block is given.
    requires_compile_opts: bool = False

    @abstractmethod
    def is_available(self) -> bool:
        """True if this backend's compiler is installed/usable on this host."""

    @abstractmethod
    def accepts(self, src_format: str) -> bool:
        """True if this adapter can consume the given source format (onnx|tflite)."""

    @abstractmethod
    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None, target: TargetSpec | None = None) -> Blob:
        """Compile @source for @accel_config; return the Blob.

        @opts is the per-model compile config for this backend
        (board.yaml `models[].compile.<backend>`), with any path values already
        resolved to absolute paths by the caller; None when the backend needs
        no per-model config (cpu, ethos_u).

        @target is the resolved `alp_project_loader.TargetSpec` this compile
        call came from -- the silicon-determined facts (e.g. ethos_u's
        `vela_memory_mode`) the caller already resolved from metadata/, never
        re-read from disk by the adapter itself. None only in a caller that
        hasn't threaded it (e.g. a hand-built test); every real backend but
        ethos_u ignores it."""

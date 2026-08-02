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
    # True for backends that need a per-model compile config the SDK can't
    # derive (DEEPX JSON+calibration, DRP-AI spec). build_model records a
    # "no compile config" coverage skip for these when no opts block is given.
    requires_compile_opts: bool = False
    tool: str = ""              # console command probed for availability ("" = builtin, no tool)

    def version(self) -> str:
        """Best-effort tool version string; only meaningful when available."""
        return ""

    def reason(self) -> str:
        """Why this backend is unavailable (empty when available)."""
        return f"{self.tool or self.backend} not available"

    def probe(self) -> dict:
        """Read-only availability report for `alp model doctor`."""
        avail = self.is_available()
        return {"backend": self.backend, "tool": self.tool, "available": avail,
                "version": self.version() if avail else None,
                "reason": None if avail else self.reason()}

    @abstractmethod
    def is_available(self) -> bool:
        """True if this backend's compiler is installed/usable on this host."""

    @abstractmethod
    def accepts(self, src_format: str) -> bool:
        """True if this adapter can consume the given source format (onnx|tflite)."""

    @abstractmethod
    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None) -> Blob:
        """Compile @source for @accel_config; return the Blob.

        @opts is the per-model compile config for this backend
        (board.yaml `models[].compile.<backend>`), with any path values already
        resolved to absolute paths by the caller; None when the backend needs
        no per-model config (cpu, ethos_u)."""

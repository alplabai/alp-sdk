# scripts/alp_model/adapters/deepx.py
"""DEEPX DX-M1 compiler adapter (host model compiler).

DEEPX's compiler is the proprietary, license-gated `dx-com` Python wheel
(console script `dxcom`; `dx-com` 2.3.0 verified -- Linux x86_64, CPython 3.12,
ONNX frontend, Target Hardware: M1). It compiles an ONNX model into a DEEPX NPU
model binary:
    dxcom -m <model.onnx> -c <config.json> -o <output_dir>
with a per-model JSON config (the calibration dataset is referenced from inside
that config -- `dxcom` has no separate calibration CLI flag). `-o` is a
*directory*, but a successful compile writes exactly one canonical artifact into
it: ``<model_stem>.dxnn`` -- a self-describing flatbuffer (magic ``DXNN`` + a
JSON header). That single ``.dxnn`` is what the on-device `dx_rt` runtime loads,
so the adapter returns its raw bytes with `blob_format` ``dxnn`` (matching the
device-side ALP_INFERENCE_MODEL_DXNN that `alp_model_select` decodes), NOT a tar
of the directory. (Confirmed against a real `dxcom` 2.3.0 run, 2026-05-27: a
tiny CNN and a real yolo11n both emit a single ``<stem>.dxnn``; `compiler.log`
only appears with `--gen_log`. dxcom also requires >15 GB host RAM.)

The wheel is not redistributable, so it is NOT bundled; is_available() is True
only when `dxcom` is on PATH or ALP_DEEPX_SDK_HOME points at an install. The
per-model config + calibration come from board.yaml `models[].compile.deepx_dxm1`
(threaded in as `opts`). The dx_rt A55/PCIe *runtime* backend
(`src/yocto/inference_deepx.cpp`) is Stage 2 step 4 (bench-gated)."""
from __future__ import annotations
import os
import re
import shutil
import subprocess
from pathlib import Path
from . import CompilerAdapter, Blob

# dxcom does post-training quantization + compilation (torch/onnx under the
# hood); minutes for a real model, but never unbounded in CI.
_DXCOM_TIMEOUT_S = 1800


def _dxcom_version() -> str:
    """Best-effort compiler version, e.g. 'DX-COM 2.3.0'; 'dxcom' on failure."""
    try:
        proc = subprocess.run(["dxcom", "-v"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return "dxcom"
    m = re.search(r"DX-COM[^\d]*(\d+\.\d+\.\d+)", proc.stdout + proc.stderr)
    return f"DX-COM {m.group(1)}" if m else "dxcom"


class DeepxAdapter(CompilerAdapter):
    backend = "deepx_dxm1"
    requires_compile_opts = True          # needs a per-model dxcom JSON config
    tool = "dxcom"

    def version(self) -> str:
        return _dxcom_version()

    def reason(self) -> str:
        return "dxcom not found (dx-com wheel not installed and ALP_DEEPX_SDK_HOME unset)"

    def is_available(self) -> bool:
        if shutil.which("dxcom"):            # the dx-com wheel's console script
            return True
        root = os.environ.get("ALP_DEEPX_SDK_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format == "onnx"          # dxcom is an ONNX frontend

    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None) -> Blob:
        config = (opts or {}).get("config")
        if not config:
            raise RuntimeError(
                "DEEPX compile needs models[].compile.deepx_dxm1.config "
                "(a dxcom JSON config; the calibration set is referenced from it)")
        dst = out_dir / f"{source.stem}_dxnn"
        dst.mkdir(parents=True, exist_ok=True)
        cmd = ["dxcom", "-m", str(source), "-c", str(config), "-o", str(dst)]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=_DXCOM_TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"dxcom timed out after {exc.timeout}s") from exc
        if proc.returncode != 0:
            raise RuntimeError(f"dxcom failed: {proc.stderr.strip() or proc.stdout.strip()}")
        artifacts = sorted(dst.glob("*.dxnn"))
        if not artifacts:
            raise RuntimeError(f"dxcom produced no .dxnn in {dst}")
        # A single-input compile emits exactly one <stem>.dxnn; prefer the
        # model-stem-named artifact if dxcom ever emits more than one.
        canonical = next((p for p in artifacts if p.stem == source.stem), artifacts[0])
        return Blob(format="dxnn", payload=canonical.read_bytes(),
                    compiler_version=_dxcom_version())

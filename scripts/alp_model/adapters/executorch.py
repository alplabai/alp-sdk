# scripts/alp_model/adapters/executorch.py
"""ExecuTorch passthrough adapter (host model compiler).

ExecuTorch programs are exported ahead of time on the ML-engineer's side
(``torch.export`` + ``to_edge_transform_and_lower``), the same way a
``.tflite`` flatbuffer arrives pre-compiled for CpuAdapter: there is no
alp-sdk-invoked host toolchain step, only a byte passthrough of the exported
``.pte`` program into the ``.alpmodel`` package.

``backend`` is ``"cpu"`` -- ExecuTorch is a CPU-tier program format, an
alternative to plain TFLM ``.tflite`` for that same tier, not a distinct
physical accelerator (see ``ALP_INFERENCE_BACKEND_*`` in
``include/alp/inference.h``: cpu | ethos_u | drpai | deepx_dxm1, no
"executorch" backend). Because ``build.py``'s default adapter registry is
keyed one-adapter-per-backend (``by_backend = {a.backend: a for a in
registry}``) and already carries ``CpuAdapter`` for ``"cpu"``, this adapter is
NOT auto-registered in ``build.py``'s ``_ADAPTERS`` list -- adding it there
would silently swap every board's CPU tier from TFLite to ExecuTorch. Use it
explicitly instead: ``build_model(..., adapters=[ExecutorchAdapter(), ...])``
for a variant that wants ExecuTorch instead of TFLM on the CPU tier.

The device-side format decode (blob_format string "executorch" ->
``ALP_INFERENCE_MODEL_EXECUTORCH``) is ``_fmt_enum()`` in
``src/backends/inference/alp_model_select.c``; there is still no on-device
ExecuTorch *runtime* backend (issue #1260 closes the write-side gap only --
the SDK can now describe and produce an ExecuTorch blob, not yet run one)."""
from __future__ import annotations
from pathlib import Path
from . import CompilerAdapter, Blob


class ExecutorchAdapter(CompilerAdapter):
    backend = "cpu"

    def is_available(self) -> bool:
        return True              # always available; no external tool

    def accepts(self, src_format: str) -> bool:
        return src_format == "pte"       # ExecuTorch's own exported program format

    def compile(self, source: Path, *, accel_config: str, out_dir: Path, opts: dict | None = None) -> Blob:
        payload = source.read_bytes()
        return Blob(format="executorch", payload=payload, arena_bytes=0,
                    compiler_version="passthrough")

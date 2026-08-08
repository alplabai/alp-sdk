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
"executorch" backend). It IS registered in ``build.py``'s default
``_ADAPTERS`` list alongside ``CpuAdapter`` -- both carry ``backend ==
"cpu"``, and ``build_model()`` selects between them per model by
``accepts(src_fmt)``, not by registration order: a ``.tflite`` source still
resolves to ``CpuAdapter`` (this adapter's ``accepts()`` rejects it), a
``.pte`` source resolves here. No board's existing TFLite build is affected;
`alp model build` reaches this adapter automatically the moment a board.yaml
`models[].source` ends in ``.pte``.

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
        # "pte" is ExecuTorch's own upstream export extension (torch.export /
        # to_edge_transform_and_lower writes a `.pte` file); it is not an
        # in-tree-derived fact -- metadata/schemas/board.schema.json's
        # `models[].source` description names it explicitly (see there).
        return src_format == "pte"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path, opts: dict | None = None) -> Blob:
        payload = source.read_bytes()
        return Blob(format="executorch", payload=payload, arena_bytes=0,
                    compiler_version="passthrough")

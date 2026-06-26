# scripts/alp_model/adapters/drpai.py
"""Renesas DRP-AI compiler adapter (host model compiler).

DRP-AI uses the DRP-AI TVM toolchain (github.com/renesas-rz/rzv_drp-ai_tvm),
powered by the EdgeCortix MERA(TM) compiler. Its high-level driver is the
``compile_onnx_model_quant.py`` tutorial script: it takes an ONNX model + a
calibration image directory and emits a multi-file "Runtime Model Data" object
DIRECTORY (drp_desc.bin / weight.bin / addr_map.txt / deploy.json / deploy.so /
preprocess/ ...) that the on-device DRP-AI TVM runtime (MeraDrpRuntimeWrapper)
loads via ``LoadModel(<dir>)``. The toolchain build also needs the cross
toolchain root and (for V2N/V2H) the ``PRODUCT`` env set; the tutorial script
hard-requires ``PRODUCT in {V2H, V2N}``.

Because the artifact is a *directory*, the adapter packs it into a single
deterministic ``.tar`` byte blob with ``blob_format`` ``"drpai_dir"`` (the
device-side ``_fmt_enum`` maps that to ``ALP_INFERENCE_MODEL_DRPAI``; the
``.alpmodel`` loader stages the tar back out to a dir before handing the path
to the runtime).

The toolchain is large and account-/source-gated (the prebuilt MERA2 wheels +
the DRP-AI Translator), so it is NOT bundled. ``is_available()`` is True only
when ``ALP_DRPAI_TVM_HOME`` points at a built install; otherwise the drpai
target is a coverage skip (detect-and-skip), mirroring the DEEPX ``dxcom``
adapter. The A55 DRP-AI runtime backend is ``src/yocto/inference_drpai.cpp``
(bench-gated)."""
from __future__ import annotations
import io
import os
import re
import subprocess
import tarfile
from pathlib import Path

from . import CompilerAdapter, Blob

# The TVM/MERA build runs quantization + DRP-AI translation; minutes for a real
# model, bounded so CI (when it ever runs) can't hang.
_DRPAI_TIMEOUT_S = 1800

# Files the DRP-AI Translator is contractually required to emit into the object
# dir (per run_drp_quant_compiler.sh's expected_result_files). We assert at
# least the core descriptor + weights landed before declaring success.
_REQUIRED_ARTIFACTS = ("drp_desc.bin", "weight.bin", "addr_map.txt")


def _tvm_home() -> Path | None:
    root = os.environ.get("ALP_DRPAI_TVM_HOME")
    return Path(root) if root and Path(root).is_dir() else None


def _compiler_version(tvm_home: Path) -> str:
    """Best-effort toolchain version from the DRP-AI TVM checkout.

    Reads the repo's setup version file when present; falls back to a plain
    'drp-ai_tvm' tag. Never raises -- version reporting is non-fatal."""
    for rel in ("setup/version", "version", "VERSION"):
        vf = tvm_home / rel
        try:
            txt = vf.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        m = re.search(r"\d+\.\d+(?:\.\d+)?", txt)
        if m:
            return f"drp-ai_tvm {m.group(0)}"
    return "drp-ai_tvm"


def _tar_dir(obj_dir: Path) -> bytes:
    """Pack the object dir into a single deterministic .tar byte blob.

    Deterministic: sorted entries, zeroed mtime/uid/gid/owner so the same
    inputs reproduce byte-identical blobs (matters for content-addressed
    .alpmodel builds + CI diffing)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for path in sorted(obj_dir.rglob("*")):
            if not path.is_file():
                continue
            info = tarfile.TarInfo(name=str(path.relative_to(obj_dir)))
            data = path.read_bytes()
            info.size = len(data)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


class DrpaiAdapter(CompilerAdapter):
    backend = "drpai"
    requires_compile_opts = True             # needs input shape/name + calib images

    def is_available(self) -> bool:
        return _tvm_home() is not None

    def accepts(self, src_format: str) -> bool:
        return src_format == "onnx"          # DRP-AI TVM ingests ONNX

    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None) -> Blob:
        tvm_home = _tvm_home()
        if tvm_home is None:
            raise RuntimeError(
                "DRP-AI compile needs the DRP-AI TVM toolchain; set "
                "ALP_DRPAI_TVM_HOME to a built rzv_drp-ai_tvm install")

        opts = opts or {}
        # The TVM driver needs the model input geometry + a calibration image
        # set (post-training INT8 quantization); these come from board.yaml
        # models[].compile.drpai.  (accel_config is "" for DRP-AI -- targets.py
        # only populates it for Ethos-U; the DRP-AI product comes from the
        # models[].compile.drpai config, not accel_config.)
        input_shape = opts.get("input_shape")
        input_name = opts.get("input_name")
        images = opts.get("images")
        if not input_shape or not input_name or not images:
            raise RuntimeError(
                "DRP-AI compile needs models[].compile.drpai with "
                "input_shape, input_name and images (a calibration image dir)")

        product = (opts.get("product") or accel_config or "V2N").upper()
        if product not in ("V2N", "V2H"):
            raise RuntimeError(
                f"DRP-AI compile: unsupported product {product!r} "
                "(expected V2N or V2H)")

        obj_dir = out_dir / f"{source.stem}_drpai"
        obj_dir.mkdir(parents=True, exist_ok=True)

        # The tutorial driver lives in the toolchain checkout; invoke it with
        # the model, the output object dir, the input geometry and the
        # calibration images. PRODUCT is consumed from the environment.
        script = tvm_home / "tutorials" / "compile_onnx_model_quant.py"
        cmd = [
            "python3", str(script), str(source),
            "-o", str(obj_dir),
            "-s", str(input_shape),
            "-i", str(input_name),
            "--images", str(images),
        ]
        env = {**os.environ, "PRODUCT": product}
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=_DRPAI_TIMEOUT_S, env=env)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"DRP-AI compile timed out after {exc.timeout}s") from exc
        if proc.returncode != 0:
            raise RuntimeError(
                f"DRP-AI compile failed: {proc.stderr.strip() or proc.stdout.strip()}")

        missing = [f for f in _REQUIRED_ARTIFACTS if not (obj_dir / f).is_file()]
        if missing:
            raise RuntimeError(
                f"DRP-AI compile produced no {', '.join(missing)} in {obj_dir}")

        return Blob(format="drpai_dir", payload=_tar_dir(obj_dir),
                    compiler_version=_compiler_version(tvm_home))

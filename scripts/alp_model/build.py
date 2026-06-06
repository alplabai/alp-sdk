# scripts/alp_model/build.py
"""Build driver: SKU + source model -> .alpmodel package (compile-what's-available).

Resolves the SoM's targets, runs each *available* compiler adapter, and assembles
the package. A backend whose adapter is missing, or whose tool is not installed,
is recorded as a `coverage` skip; a source format no adapter accepts is
`incompatible`. If *no* blob is produced the build fails loudly -- an .alpmodel
with zero runnable blobs is broken."""
from __future__ import annotations
import hashlib
from pathlib import Path

from .adapters import CompilerAdapter
from .adapters.cpu import CpuAdapter
from .adapters.ethos_u import VelaAdapter
from .adapters.drpai import DrpaiAdapter
from .adapters.deepx import DeepxAdapter
from .manifest import Manifest, Target, Coverage
from .package import write_package
from .targets import resolve_targets
from .tensorio import extract_io

# Default adapter registry. Each is detect-and-skip (is_available() False when
# its tool is absent); vela (ethos_u) skips on hosts without the ethos-u-vela package.
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter(), VelaAdapter(), DrpaiAdapter(), DeepxAdapter()]


def _src_format(source: Path) -> str:
    return source.suffix.lstrip(".").lower()        # "tflite" | "onnx"


def build_model(*, sku: str, name: str, source: Path, out_dir: Path,
                metadata_root: Path,
                adapters: list[CompilerAdapter] | None = None,
                compile_opts: dict[str, dict] | None = None) -> Path:
    registry = list(_ADAPTERS if adapters is None else adapters)
    by_backend = {a.backend: a for a in registry}
    specs = resolve_targets(sku, metadata_root=metadata_root)
    src_fmt = _src_format(source)
    opts_by_backend = compile_opts or {}

    out_dir.mkdir(parents=True, exist_ok=True)
    targets: list[Target] = []
    coverage: list[Coverage] = []
    blobs: list[bytes] = []
    for spec in specs:
        adapter = by_backend.get(spec.backend)
        if adapter is None:
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"no compiler adapter for {spec.backend}"))
            continue
        backend_opts = opts_by_backend.get(spec.backend)
        if adapter.requires_compile_opts and not backend_opts:
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"no compile config for {spec.backend} "
                                     f"(add models[].compile.{spec.backend} to board.yaml)"))
            continue
        if not adapter.is_available():
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"{spec.backend} compiler not installed"))
            continue
        if not adapter.accepts(src_fmt):
            coverage.append(Coverage(spec.backend, spec.accel_config, "incompatible",
                                     f"{spec.backend} does not accept .{src_fmt}"))
            continue
        blob = adapter.compile(source, accel_config=spec.accel_config, out_dir=out_dir, opts=backend_opts)
        targets.append(Target(
            backend=spec.backend, silicon_ref=spec.silicon_ref,
            blob_format=blob.format, accel_config=spec.accel_config,
            arena=blob.arena_bytes,
            requires={"sram_kib": blob.req_sram_kib, "op_features": []},
            blob=len(blobs), compiler_version=blob.compiler_version))
        blobs.append(blob.payload)

    if not blobs:
        detail = "; ".join(f"{c.backend}:{c.status} ({c.reason})" for c in coverage)
        raise ValueError(f"no blob compiled for model '{name}' (.{src_fmt}); coverage: {detail}")

    src_bytes = source.read_bytes()          # read once: shared by the sha + tensor-I/O
    inputs, outputs = extract_io(source, raw=src_bytes)
    mft = Manifest(name=name, src_sha=hashlib.sha256(src_bytes).digest(),
                   inputs=inputs, outputs=outputs,
                   targets=targets, coverage=coverage)
    out_path = out_dir / f"{name}.alpmodel"
    out_path.write_bytes(write_package(mft, blobs))
    return out_path

# scripts/alp_model/build.py
"""Build driver: SKU + source model -> .alpmodel package (compile-what's-available)."""
from __future__ import annotations
import hashlib
from pathlib import Path

from .adapters import CompilerAdapter
from .adapters.cpu import CpuAdapter
from .manifest import Manifest, Target, Coverage
from .package import write_package
from .targets import resolve_targets

# 1b-i: only the CPU adapter is wired. vela (ethos_u) + drpai/deepx land in 1b-ii;
# until then their targets are recorded as coverage skips.
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter()]


def _src_format(source: Path) -> str:
    return source.suffix.lstrip(".").lower()        # "tflite" | "onnx"


def build_model(*, sku: str, name: str, source: Path,
                out_dir: Path, metadata_root: Path) -> Path:
    specs = resolve_targets(sku, metadata_root=metadata_root)
    src_fmt = _src_format(source)
    adapters = {a.backend: a for a in _ADAPTERS if a.is_available()}

    out_dir.mkdir(parents=True, exist_ok=True)
    targets: list[Target] = []
    coverage: list[Coverage] = []
    blobs: list[bytes] = []
    for spec in specs:
        adapter = adapters.get(spec.backend)
        if adapter is None:
            coverage.append(Coverage(backend=spec.backend, accel_config=spec.accel_config,
                                     status="skipped", reason="no adapter available (1b-i)"))
            continue
        if not adapter.accepts(src_fmt):
            coverage.append(Coverage(backend=spec.backend, accel_config=spec.accel_config,
                                     status="incompatible",
                                     reason=f"{spec.backend} does not accept .{src_fmt}"))
            continue
        blob = adapter.compile(source, accel_config=spec.accel_config, out_dir=out_dir)
        targets.append(Target(
            backend=spec.backend, silicon_ref=spec.silicon_ref,
            blob_format=blob.format, accel_config=spec.accel_config,
            arena=blob.arena_bytes,
            requires={"sram_kib": blob.req_sram_kib, "op_features": []},
            blob=len(blobs)))
        blobs.append(blob.payload)

    mft = Manifest(name=name, src_sha=hashlib.sha256(source.read_bytes()).digest(),
                   inputs=[], outputs=[],        # tensor-I/O extraction is 1b-ii
                   targets=targets, coverage=coverage)
    out_path = out_dir / f"{name}.alpmodel"
    out_path.write_bytes(write_package(mft, blobs))
    return out_path

# scripts/alp_model/build.py
"""Build driver: SKU + source model -> .alpmodel package (compile-what's-available).

Resolves the SoM's targets, runs each *available* compiler adapter, and assembles
the package. A backend whose adapter is missing, or whose tool is not installed,
is recorded as a `coverage` skip; a source format no adapter accepts is
`incompatible`. If *no* blob is produced the build fails loudly -- an .alpmodel
with zero runnable blobs is broken."""
from __future__ import annotations
import hashlib
import re
from pathlib import Path

from .adapters import CompilerAdapter
from .adapters.cpu import CpuAdapter
from .adapters.ethos_u import VelaAdapter
from .adapters.drpai import DrpaiAdapter
from .adapters.deepx import DeepxAdapter
from .adapters.executorch import ExecutorchAdapter
from .manifest import Manifest, Target, Coverage
from .package import write_package
from .targets import resolve_targets
from .tensorio import extract_io

# Default adapter registry. Each is detect-and-skip (is_available() False when
# its tool is absent); vela (ethos_u) skips on hosts without the ethos-u-vela package.
# A backend may carry more than one adapter (cpu: CpuAdapter for .tflite,
# ExecutorchAdapter for .pte) -- see the by_backend grouping below, which
# selects among a backend's adapters by accepts(src_fmt), not by last-one-wins.
_ADAPTERS: list[CompilerAdapter] = [
    CpuAdapter(), VelaAdapter(), DrpaiAdapter(), DeepxAdapter(), ExecutorchAdapter(),
]

# #1125: mirrors metadata/schemas/board.schema.json's `models[].name` pattern.
# build_model() is called directly by non-CLI callers (tests, future tooling),
# not just tan model build's spawned driver (python/tan/commands/model_cmd.py
# in tan-cli, which imports alp_model.build and calls build_model() itself)
# -- an allowlist here is the root-cause guard, independent of whether the
# caller validated board.yaml.
_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")


def _src_format(source: Path) -> str:
    return source.suffix.lstrip(".").lower()        # "tflite" | "onnx"


def build_model(*, sku: str, name: str, source: Path, out_dir: Path,
                metadata_root: Path,
                adapters: list[CompilerAdapter] | None = None,
                compile_opts: dict[str, dict] | None = None) -> Path:
    if not _NAME_RE.fullmatch(name):
        raise ValueError(f"invalid model name {name!r}: must match {_NAME_RE.pattern!r}")
    registry = list(_ADAPTERS if adapters is None else adapters)
    by_backend: dict[str, list[CompilerAdapter]] = {}
    for a in registry:
        by_backend.setdefault(a.backend, []).append(a)
    specs = resolve_targets(sku, metadata_root=metadata_root)
    src_fmt = _src_format(source)
    opts_by_backend = compile_opts or {}

    out_dir.mkdir(parents=True, exist_ok=True)
    targets: list[Target] = []
    coverage: list[Coverage] = []
    blobs: list[bytes] = []
    for spec in specs:
        candidates = by_backend.get(spec.backend, [])
        if not candidates:
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"no compiler adapter for {spec.backend}"))
            continue
        if len(candidates) > 1:
            # A backend with more than one adapter (cpu: CpuAdapter + ExecutorchAdapter)
            # is disambiguated by source format up front -- accepts() decides identity,
            # not registration order. A single-adapter backend keeps the original order
            # below (requires_compile_opts / is_available reported before "incompatible"),
            # so an unrelated format mismatch doesn't mask a "no compile config" skip.
            adapter = next((a for a in candidates if a.accepts(src_fmt)), None)
            if adapter is None:
                coverage.append(Coverage(spec.backend, spec.accel_config, "incompatible",
                                         f"{spec.backend} does not accept .{src_fmt}"))
                continue
        else:
            adapter = candidates[0]
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
    # Belt-and-suspenders: the name allowlist above already makes escape
    # impossible for a bare filename, but fail closed on containment too --
    # a resolved-path check catches this even if the join expression above
    # ever grows a second path segment.
    resolved_out_dir = out_dir.resolve()
    if not out_path.resolve().is_relative_to(resolved_out_dir):
        raise ValueError(f"refusing to write outside out_dir: {out_path}")
    out_path.write_bytes(write_package(mft, blobs))
    return out_path

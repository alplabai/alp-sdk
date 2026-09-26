# scripts/alp_model/adapters/ethos_u.py
"""Arm Ethos-U (Vela) compiler adapter.

Wraps the `vela` CLI from `ethos-u-vela` (the `model-compile` optional
dependency). is_available() is True when `vela` is on PATH; compile() shells out
for the given accelerator-config and reads back `<stem>_vela.tflite`. The
arena/peak-SRAM footprint is parsed best-effort from vela's summary CSV (column
names drift across vela versions, so matching is tolerant; 0 when unavailable).

req_sram_kib is the tensor ARENA only (vela's `sram_memory_used` column),
never the const/weights region (`on_chip_flash_memory_used`) -- see
_parse_vela_summary. The const region is carried in the model blob itself and
sized by the integrator from the blob's own byte length (`blob_len`); it is
never summed into req_sram_kib. This mirrors tan-cli's `_footprint()`
(tan-cli#1011, `python/tan/model/adapters/ethos_u.py`) and the SRAM-port
pinning in `src/backends/inference/ethos_u_aen.cpp`."""
from __future__ import annotations
import csv
import math
import os
import shutil
import subprocess
from collections.abc import Callable
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
from . import CompilerAdapter, Blob

_VELA_TIMEOUT_S = 600        # vela compiles are minutes at most; never unbounded in CI


def _vela_version() -> str:
    try:
        return f"vela {version('ethos-u-vela')}"
    except PackageNotFoundError:
        return "vela"


def _parse_vela_summary(out_dir: Path, stem: str) -> tuple[int, int]:
    """Best-effort (arena_bytes, req_sram_kib) from vela's <stem>_summary_*.csv.

    vela's `sram_memory_used` column is ALREADY in KiB (not bytes, despite
    looking byte-scale at a glance -- e.g. `72.0`); this is the tensor ARENA
    only. req_sram_kib is rounded UP (ceil, never floor/truncate): the
    device-side fit gate (`src/backends/inference/alp_model_select.c`) must
    never under-report a model's requirement. arena_bytes mirrors it in bytes.
    on_chip_flash_memory_used (the const/weights region) is deliberately never
    read here -- it is carried in the model blob, sized by blob_len.

    Returns (0, 0) when the summary is missing or unparseable, or when vela
    reported no SRAM at all (a full CPU fallback)."""
    matches = sorted(out_dir.glob(f"{stem}_summary_*.csv"))
    if not matches:
        return 0, 0
    with open(matches[0], newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        return 0, 0
    row = rows[0]

    def _num(pred: Callable[[str], bool]) -> float:
        for key, val in row.items():
            if key and pred(key.lower()):
                try:
                    return float(val)
                except (TypeError, ValueError):
                    continue
        return 0.0

    sram_kib = _num(lambda k: "sram" in k and "used" in k)
    if sram_kib <= 0:
        return 0, 0
    return round(sram_kib * 1024), math.ceil(sram_kib)


class VelaAdapter(CompilerAdapter):
    backend = "ethos_u"

    def is_available(self) -> bool:
        return shutil.which("vela") is not None

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path, opts: dict | None = None) -> Blob:
        out_dir.mkdir(parents=True, exist_ok=True)
        cmd = ["vela", str(source), "--accelerator-config", accel_config,
               "--output-dir", str(out_dir)]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                  env={**os.environ, "PYTHONIOENCODING": "utf-8"}, timeout=_VELA_TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"vela timed out after {exc.timeout}s for {accel_config}") from exc
        if proc.returncode != 0:
            raise RuntimeError(f"vela failed for {accel_config}: {proc.stderr.strip()}")
        produced = out_dir / f"{source.stem}_vela.tflite"
        if not produced.is_file():
            raise RuntimeError(f"vela produced no output at {produced}")
        arena, sram_kib = _parse_vela_summary(out_dir, source.stem)
        return Blob(format="vela_tflite", payload=produced.read_bytes(),
                    arena_bytes=arena, compiler_version=_vela_version(),
                    req_sram_kib=sram_kib)

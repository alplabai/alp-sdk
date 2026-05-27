# scripts/alp_model/adapters/ethos_u.py
"""Arm Ethos-U (Vela) compiler adapter.

Wraps the `vela` CLI from `ethos-u-vela` (the `model-compile` optional
dependency). is_available() is True when `vela` is on PATH; compile() shells out
for the given accelerator-config and reads back `<stem>_vela.tflite`. The
arena/peak-SRAM footprint is parsed best-effort from vela's summary CSV (column
names drift across vela versions, so matching is tolerant; 0 when unavailable)."""
from __future__ import annotations
import csv
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
    """Best-effort (arena_bytes, peak_sram_kib) from vela's <stem>_summary_*.csv.
    Returns (0, 0) when the summary is missing or unparseable."""
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

    sram_bytes = _num(lambda k: "sram" in k and "used" in k)
    arena = _num(lambda k: "arena" in k) or sram_bytes
    return int(arena), int(sram_bytes // 1024)


class VelaAdapter(CompilerAdapter):
    backend = "ethos_u"

    def is_available(self) -> bool:
        return shutil.which("vela") is not None

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        out_dir.mkdir(parents=True, exist_ok=True)
        cmd = ["vela", str(source), "--accelerator-config", accel_config,
               "--output-dir", str(out_dir)]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=_VELA_TIMEOUT_S)
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

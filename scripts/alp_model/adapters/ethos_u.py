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
pinning in `src/backends/inference/ethos_u_aen.cpp`.

THE MEMORY PROFILE (issue #2312, mirroring alp-sdk #1470 / tan-cli#789):
invoked with NEITHER `--memory-mode` nor `--system-config`, vela falls back to
its own DRAM-backed built-in profile (`Ethos_U85_SYS_DRAM_Mid` /
`Dedicated_Sram_384KB`) and reports the entire working set in DRAM --
`sram_memory_used = 0.0` on a part that has no DRAM at all -- and that zero
then satisfies alp-sdk's on-device fit gate
(`src/backends/inference/alp_model_select.c`) against ANY arena. compile()
therefore REFUSES to run without a memory mode at all (`@target` is None or
carries no `vela_memory_mode`) -- guessing a profile compiles a command
stream for memory the module may not have, and a customer's `.alpmodel`
build must fail loudly rather than ship a defaulted footprint. `--memory-mode`
comes from @target's `vela_memory_mode` (the SoC spec's own
`npu_toolchain.vela.memory_mode`, resolved once by
`alp_project_loader.resolve_targets` and threaded here -- never re-read from
metadata/ inside this adapter). `--system-config` is split by
`vela_system_config` (an Arm built-in, safe to pass alone) vs
`vela_vendor_system_config` (vendor-gated, passed only alongside `--config`
when a vendor vela `.ini` is actually available via `ALP_VELA_CONFIG`):
naming a vendor-only System_Config section with no `--config` is a hard vela
rc=1 ("Section System_Config.<name> not found in Vela config file"), so an
unresolvable vendor name is simply withheld rather than guessed at.

A clean vela exit is never trusted to mean the reported footprint is honest:
a compile that placed operators on the NPU but reports zero SRAM anywhere is
refused loudly (`_refuse_zero_sram_footprint`) rather than shipped as a zero
arena that fits any envelope. A FULL CPU fallback (vela placed nothing on the
NPU) legitimately reports zero SRAM and is not refused -- see
`_parse_vela_placement`."""
from __future__ import annotations
import csv
import math
import os
import re
import shutil
import subprocess
from collections.abc import Callable
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
from . import CompilerAdapter, Blob, TargetSpec

_VELA_TIMEOUT_S = 600        # vela compiles are minutes at most; never unbounded in CI

#: Where a licensed customer names their vendor vela config `.ini` -- an
#: ENVIRONMENT fact, never a hardware one, so it is read here and never from
#: board.yaml (same shape as `ALP_DRPAI_TVM_HOME` / `ALP_DEEPX_SDK_HOME` in
#: the sibling adapters). Unset, empty, or naming an unreadable file all mean
#: "no vendor config" -- degrade to withholding --system-config rather than
#: handing vela a path it cannot open.
_VELA_CONFIG_ENV = "ALP_VELA_CONFIG"

# ethosu.vela.stats_writer.print_performance_metrics_common (always printed to
# stdout) emits exactly one "CPU operators =" and one "NPU operators =" line
# per run, e.g. "NPU operators = 6 (40.0%)" -- present whether or not any
# operator actually landed on the NPU (a full CPU fallback prints "NPU
# operators = 0 (0.0%)" and still exits 0). Both kinds are read (not NPU
# alone) so a caller can tell "this run reported 0 NPU ops" apart from "this
# run's placement lines could not be found at all" using the same regex.
_PLACEMENT_RE = re.compile(r"^(CPU|NPU) operators = (\d+)", re.MULTILINE)

# A directory component built from an accel_config that comes out of SoM
# metadata; anything outside this set is folded to "_" so a malformed
# accel_config can never walk a compile's output directory out of @out_dir.
_UNSAFE_DIR_CHARS = re.compile(r"[^A-Za-z0-9._-]")


def _vela_version() -> str:
    try:
        return f"vela {version('ethos-u-vela')}"
    except PackageNotFoundError:
        return "vela"


def _vendor_config_path() -> Path | None:
    """The vendor vela `.ini` this host has, or None.

    None for unset/empty and for a value that does not name a readable FILE:
    vela would fail on a path it cannot open, and a stale env var must
    degrade to "no vendor config" rather than break a build that worked
    yesterday."""
    raw = os.environ.get(_VELA_CONFIG_ENV)
    if not raw:
        return None
    path = Path(raw).expanduser()
    return path if path.is_file() else None


def _run_dir(out_dir: Path, accel_config: str) -> Path:
    """A per-accel-config subdirectory of @out_dir for one vela run.

    `build_model` (scripts/alp_model/build.py) reuses one `out_dir` across
    every target a SoM declares, so a shared directory means one compile's
    summary CSV can silently answer for another's -- a u55 target's summary
    left in @out_dir got read back as a u85 target's footprint (issue #2312
    review). One subdirectory per accel_config makes that impossible: each
    run's `_parse_vela_summary` glob can only ever see its own output."""
    return out_dir / f"vela-{_UNSAFE_DIR_CHARS.sub('_', accel_config)}"


def _profile_flags(target: TargetSpec) -> list[str]:
    """The `--memory-mode`/`--config`/`--system-config` flags for @target's
    silicon. @target.vela_memory_mode is required by the caller before this
    is reached -- see compile()."""
    flags = ["--memory-mode", target.vela_memory_mode]
    if target.vela_system_config:
        # A built-in Arm System_Config: safe to pass alone, no vendor file
        # needed (see TargetSpec's split-field docstring).
        flags += ["--system-config", target.vela_system_config]
    elif target.vela_vendor_system_config:
        vendor_config = _vendor_config_path()
        if vendor_config is not None:
            flags += ["--config", str(vendor_config),
                     "--system-config", target.vela_vendor_system_config]
        # else: a vendor-gated system_config with no vendor .ini on hand is
        # withheld, never guessed at -- vela's own built-in system config is
        # used instead, and the memory mode above still fixes the placement
        # that matters (see the module docstring).
    return flags


def _parse_vela_summary(out_dir: Path, stem: str) -> tuple[int, int]:
    """Best-effort (arena_bytes, req_sram_kib) from vela's <stem>_summary_*.csv.

    vela's `sram_memory_used` column is ALREADY in KiB (not bytes, despite
    looking byte-scale at a glance -- e.g. `72.0`); this is the tensor ARENA
    only. req_sram_kib is rounded UP (ceil, never floor/truncate): the
    device-side fit gate (`src/backends/inference/alp_model_select.c`) must
    never under-report a model's requirement. arena_bytes mirrors it in bytes.
    on_chip_flash_memory_used (the const/weights region) is deliberately never
    read here -- it is carried in the model blob, sized by blob_len.

    Returns (0, 0) when the summary is missing, unparseable, or AMBIGUOUS
    (more than one `*_summary_*.csv` in @out_dir) -- picking
    `sorted(matches)[0]` out of several is only ever right by accident of
    sort order, and a footprint attributed to the wrong compile is worse than
    no footprint at all. compile() calls this against a per-accel-config
    subdirectory (`_run_dir`) precisely so ambiguity should never occur in
    production; a caller that still hits it composes with
    `_refuse_zero_sram_footprint` exactly as an unreadable placement line
    does, and (0, 0) is also the correct, legitimate reading for a full CPU
    fallback."""
    matches = sorted(out_dir.glob(f"{stem}_summary_*.csv"))
    if len(matches) != 1:
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


def _parse_vela_placement(stdout: str) -> int | None:
    """The NPU operator count vela's own stdout reports for this run, or None
    when the line is absent (an unexpected vela output shape -- never
    guessed at; the caller treats None the same as a nonzero count: "can't
    confirm this zero is a full CPU fallback, refuse it")."""
    found = {kind: int(n) for kind, n in _PLACEMENT_RE.findall(stdout)}
    return found.get("NPU")


def _refuse_zero_sram_footprint(*, accel_config: str, npu_ops: int | None) -> None:
    """A successful compile that reports no SRAM working set is a refusal,
    not a zero -- UNLESS it is a confirmed full CPU fallback (vela's own
    stdout named exactly 0 NPU operators placed).

    `req_sram_kib == 0` does not read as "this model needs no arena" on the
    device side: alp-sdk's selector reads it as *fits any envelope*
    (`e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib`,
    src/backends/inference/alp_model_select.c) and a board would trust that
    zero. Measured, real `ethos-u-vela`: a model that places operators on the
    NPU under vela's DRAM-backed built-in default reports its working set in
    `dram_memory_used` instead of `sram_memory_used` -- the footprint is
    real, just not expressed in a memory area this module has.

    Fails CLOSED on an unreadable placement line too (@npu_ops is None): an
    unexpected vela output shape means this module cannot tell a full CPU
    fallback apart from a defaulted DRAM profile, and shipping the zero on a
    guess is exactly the bug this refusal exists to prevent."""
    if npu_ops is None:
        placed = ("vela's own NPU-operator-placement line could not be read "
                  "from its stdout, so this zero cannot be confirmed as a "
                  "full CPU fallback")
    else:
        placed = f"placed {npu_ops} operator(s) on the NPU"
    raise RuntimeError(
        f"vela compiled cleanly for {accel_config} and {placed}, but "
        "reported zero SRAM. This is a defaulted/mismatched vela memory "
        "profile, not a model with no arena requirement -- alp-sdk's "
        "on-device fit gate reads req_sram_kib == 0 as fitting any "
        "envelope. Declare npu_toolchain.vela.memory_mode for this SoC "
        "(metadata/schemas/soc-spec-v1.schema.json) so vela is invoked "
        "with the memory model this part actually has.")


class VelaAdapter(CompilerAdapter):
    backend = "ethos_u"

    def is_available(self) -> bool:
        return shutil.which("vela") is not None

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None, target: TargetSpec | None = None) -> Blob:
        if target is None or not target.vela_memory_mode:
            # Never run vela flagless: it silently falls back to its own
            # DRAM-backed built-in profile and reports a working set on
            # memory this part may not have (see the module docstring).
            raise RuntimeError(
                f"no vela memory profile for {accel_config}: TargetSpec.vela_memory_mode "
                "is unset. Declare npu_toolchain.vela.memory_mode for this SoC "
                "(metadata/schemas/soc-spec-v1.schema.json) rather than compile "
                "against vela's own DRAM-backed default.")
        run_dir = _run_dir(out_dir, accel_config)
        run_dir.mkdir(parents=True, exist_ok=True)
        cmd = (["vela", str(source), "--accelerator-config", accel_config,
                "--output-dir", str(run_dir)] + _profile_flags(target))
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                  env={**os.environ, "PYTHONIOENCODING": "utf-8"}, timeout=_VELA_TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"vela timed out after {exc.timeout}s for {accel_config}") from exc
        if proc.returncode != 0:
            raise RuntimeError(f"vela failed for {accel_config}: {proc.stderr.strip()}")
        produced = run_dir / f"{source.stem}_vela.tflite"
        if not produced.is_file():
            raise RuntimeError(f"vela produced no output at {produced}")
        arena, sram_kib = _parse_vela_summary(run_dir, source.stem)
        if sram_kib == 0:
            npu_ops = _parse_vela_placement(proc.stdout)
            if npu_ops != 0:
                _refuse_zero_sram_footprint(accel_config=accel_config, npu_ops=npu_ops)
        return Blob(format="vela_tflite", payload=produced.read_bytes(),
                    arena_bytes=arena, compiler_version=_vela_version(),
                    req_sram_kib=sram_kib)

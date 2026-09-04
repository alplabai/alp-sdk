# scripts/alp_model/ondevice.py
"""On-device energy measurement: the IMPURE half (subprocess + I/O allowed --
see `alp_model.measure` for the PURE energy math, which this module never
duplicates, only calls).

Phase B milestone 2: build + RAM-run a real on-target energy-measurement app
on an Alif Ensemble E8 (AEN801), parse its console capture, and turn it into
a real, honestly-labelled `EnergyMeasurement` (source="measured",
scope="carrier-rail-delta" -- see measure.py's EnergyMeasurement for why
those labels are fixed). Never fabricates a result: a missing reservation, a
build/run failure, or a malformed capture all raise `OnDeviceError` rather
than silently returning `energy=None` (a null would read as "measured
nothing" when the truth is "the bench run failed").

Console protocol (owned by the on-target app, treated as fixed here -- see
examples/aen/aen-inference-energy/src/main.cpp for the printk calls that are
the actual source of truth):
  ENERGY-CFG {"rail":..., "power_lsb_w":..., "cycles_per_s":..., "windows":...,
              "npu_dispatched":...}                         -- one header line.
              There is NO "n_inferences" key here -- inference count is
              measured per window, not configured, so it lives in ENERGY-W.
  ENERGY-S <window> <phase> <cycles> <power_raw>             -- many sample lines
  ENERGY-W <window> <phase> <n_samples> <span_cycles> <span_ms> <inferences>
                                                              -- one per window/phase;
              <inferences> is that window's completed inference count (0 for idle)
              and the ONLY source of the per-inference divisor.
  ENERGY-RESULT {"value_mj_per_inference":..., "spread_mj":...}         -- one line, cross-check only
Tolerated diagnostic lines, never crashed on (surfaced in capture_diagnostics()
where useful, or simply ignored): ENERGY-SCAN, ENERGY-WARN, ENERGY-WERR. ENERGY-WPART is
the one exception -- it means the printed sample stream is shorter than what
the device integrated, so host re-integration of that window is invalid, and
parse_console() raises OnDeviceError for it rather than silently returning an
energy computed from a partial window.
Any other line (Zephyr banner, other printk) is ignored."""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, median, stdev
from typing import Any

from alp_model.measure import EnergyMeasurement, windowed_delta

# A labgrid reservation check is a quick coordinator RPC, not a board touch --
# it must never hang waiting on a shared/busy exporter.
_LABGRID_SHOW_TIMEOUT_S = 15
# A pristine AEN west build cross-compiles the whole ITCM image; mirrors the
# ceiling ethos_u.py's vela adapter uses for its own from-scratch compile.
_BUILD_TIMEOUT_S = 600
# JLink connect + halt + loadbin + the on-target app's own sample windows +
# mem8 read-back; generous because this wrapper has no visibility into how
# many windows/samples the flashed app is configured for.
_RAMRUN_TIMEOUT_S = 120

# The on-target app's final home is a concurrent WIP under examples/; an env
# var (not a guessed path) is what keeps this module correct once it lands.
_APP_DIR_ENV = "ALP_ENERGY_APP_DIR"
# bench place name + labgrid client binary: both host/bench-specific, so both
# come from the environment (check_local_paths.py forbids a hard-coded path here).
_PLACE_ENV = "ALP_BENCH_PLACE"
_CLIENT_ENV = "ALP_LABGRID_CLIENT"

# ponytail: naive +-5%% agreement band between the DT cycles_per_s constant
# and the ENERGY-W-measured rate; tighten (or swap for a proper stats test)
# if real boards start landing near the edge of this band.
_CYCLES_PER_S_TOLERANCE = 0.05


class OnDeviceError(Exception):
    """The labgrid reservation, the build/RAM-run, or the console capture was
    missing, unavailable, or malformed. Never silently swallowed into a None
    energy result -- see the module docstring."""


# ---------------------------------------------------------------------------
# Console-capture parsing (pure text -> data; still no I/O in this half)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ParsedCapture:
    """A tolerantly-parsed on-target console capture. `samples` and
    `uptime_spans` are keyed [window_index][phase] ("active"/"idle").
    `werr`/`warn` are the raw ENERGY-WERR/ENERGY-WARN lines seen, in capture
    order -- surfaced verbatim in capture_diagnostics() so a degraded run
    (I2C errors, a timed-out window, a too-long span) stays visible instead
    of silently vanishing into a clean-looking energy figure."""
    cfg: dict[str, Any]
    samples: dict[int, dict[str, list[tuple[int, int]]]]
    uptime_spans: dict[int, dict[str, tuple[int, int, float, int]]]  # (n, span_cycles, span_ms, inferences)
    device_result: dict[str, Any] | None
    werr: list[str]
    warn: list[str]


def parse_console(text: str) -> ParsedCapture:
    """Tolerant of interleaved noise (Zephyr banner, other printk lines,
    ENERGY-SCAN) -- only the recognised `ENERGY-*` prefixes are acted on.
    Raises OnDeviceError on a missing/malformed header, zero windows, any
    window/phase with fewer than 2 samples (no interval to integrate), an
    ENERGY-W line from an older 5-field firmware image (no per-window
    inference count), or an ENERGY-WPART line (the emitted sample stream is
    shorter than what the device integrated, so host re-integration of that
    window would be invalid)."""
    cfg: dict[str, Any] | None = None
    samples: dict[int, dict[str, list[tuple[int, int]]]] = {}
    uptime_spans: dict[int, dict[str, tuple[int, int, float, int]]] = {}
    device_result: dict[str, Any] | None = None
    werr: list[str] = []
    warn: list[str] = []

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("ENERGY-CFG "):
            try:
                cfg = json.loads(line[len("ENERGY-CFG "):])
            except json.JSONDecodeError as exc:
                raise OnDeviceError(f"malformed ENERGY-CFG line: {line!r}") from exc
        elif line.startswith("ENERGY-S "):
            parts = line.split()
            if len(parts) != 5 or parts[2] not in ("active", "idle"):
                raise OnDeviceError(f"malformed ENERGY-S line: {line!r}")
            try:
                w_i, cycles, power_raw = int(parts[1]), int(parts[3]), int(parts[4])
            except ValueError as exc:
                raise OnDeviceError(f"malformed ENERGY-S line: {line!r}") from exc
            samples.setdefault(w_i, {}).setdefault(parts[2], []).append((cycles, power_raw))
        elif line.startswith("ENERGY-W "):
            parts = line.split()
            if len(parts) == 6 and parts[2] in ("active", "idle"):
                raise OnDeviceError(
                    f"ENERGY-W line has 5 fields, no per-window inference count: {line!r} -- "
                    "this capture is from an older firmware image that predates per-window "
                    "ENERGY-W inference reporting; re-flash the current "
                    "aen-inference-energy build")
            if len(parts) != 7 or parts[2] not in ("active", "idle"):
                raise OnDeviceError(f"malformed ENERGY-W line: {line!r}")
            try:
                w_i = int(parts[1])
                span = (int(parts[3]), int(parts[4]), float(parts[5]), int(parts[6]))
            except ValueError as exc:
                raise OnDeviceError(f"malformed ENERGY-W line: {line!r}") from exc
            uptime_spans.setdefault(w_i, {})[parts[2]] = span
        elif line.startswith("ENERGY-WPART "):
            parts = line.split()
            if len(parts) < 3 or parts[2] not in ("active", "idle"):
                raise OnDeviceError(f"malformed ENERGY-WPART line: {line!r}")
            raise OnDeviceError(
                f"window {parts[1]} phase {parts[2]!r} emitted a partial sample stream "
                f"({line!r}) -- the device integral is authoritative for this build and "
                "--on-device cannot re-derive energy from a partial window")
        elif line.startswith("ENERGY-WERR "):
            werr.append(line)
        elif line.startswith("ENERGY-WARN "):
            warn.append(line)
        elif line.startswith("ENERGY-RESULT "):
            try:
                device_result = json.loads(line[len("ENERGY-RESULT "):])
            except json.JSONDecodeError as exc:
                raise OnDeviceError(f"malformed ENERGY-RESULT line: {line!r}") from exc
        # else: not a recognised prefix -- banner/printk noise (incl. ENERGY-SCAN), ignored.

    if cfg is None:
        raise OnDeviceError("no ENERGY-CFG header line found in capture")
    if not samples:
        raise OnDeviceError("no ENERGY-S sample lines found in capture (zero windows)")
    for w_i, phases in samples.items():
        for phase, points in phases.items():
            if len(points) < 2:
                raise OnDeviceError(
                    f"window {w_i} phase {phase!r} has {len(points)} sample(s); need >= 2")

    return ParsedCapture(cfg=cfg, samples=samples, uptime_spans=uptime_spans,
                         device_result=device_result, werr=werr, warn=warn)


# ---------------------------------------------------------------------------
# Capture -> EnergyMeasurement (still pure: math only, delegates to measure.py)
# ---------------------------------------------------------------------------

def _samples_to_power(points: list[tuple[int, int]], cycles_per_s: float,
                       power_lsb_w: float) -> list[tuple[float, float]]:
    """(cycles, power_raw) -> (t_seconds, power_watts). t is the unsigned
    delta from the window's FIRST sample's cycle count, mod 2**32 -- the
    DWT/k_cycle counter is a uint32 that may wrap once within a window, and
    an unsigned mod-2**32 difference is correct across exactly one such
    wrap. power_watts is a straight INA236 Power-register count * its LSB
    (hardware-computed V*I; no software multiply here, per measure.py)."""
    if not points:
        return []
    t0_cycles = points[0][0]
    return [(((cycles - t0_cycles) % (1 << 32)) / cycles_per_s, power_raw * power_lsb_w)
            for cycles, power_raw in points]


def _measured_cycles_per_s(parsed: ParsedCapture) -> float | None:
    """Median of span_cycles/span_ms*1000 across every ENERGY-W span (every
    window, both phases) -- the kernel-uptime cross-check against the DT
    cycles_per_s constant. None when there are no ENERGY-W lines at all."""
    rates = [span_cycles / span_ms * 1000.0
             for phases in parsed.uptime_spans.values()
             for (_n, span_cycles, span_ms, _inf) in phases.values() if span_ms > 0]
    return median(rates) if rates else None


def _effective_cycles_per_s(parsed: ParsedCapture) -> float:
    """The measured rate when it exists and every ENERGY-W span agrees with
    it within `_CYCLES_PER_S_TOLERANCE`; otherwise the DT constant."""
    dt = float(parsed.cfg["cycles_per_s"])
    measured = _measured_cycles_per_s(parsed)
    if measured is None:
        return dt
    rates = [span_cycles / span_ms * 1000.0
             for phases in parsed.uptime_spans.values()
             for (_n, span_cycles, span_ms, _inf) in phases.values() if span_ms > 0]
    if (max(rates) - min(rates)) / measured > _CYCLES_PER_S_TOLERANCE:
        return dt
    return measured


def measurement_from_capture(parsed: ParsedCapture) -> EnergyMeasurement:
    """Per (active, idle) window pair: convert samples to (t, watts), call
    the PURE `windowed_delta()` once, dividing by THAT WINDOW's own active
    inference count (ENERGY-W's 6th field -- inference count is measured
    per window, not a single config-wide constant; there is no
    `cfg["n_inferences"]`, ENERGY-CFG never carries one). `value_mj_per_inference`
    is the mean across windows, `spread_mj` the sample stdev (None for a
    single window). `window_ms` is the mean active-window duration;
    `sample_count` totals every sample (both phases) across every window
    used; `EnergyMeasurement.n_inferences` is the sum of every window's
    active-phase inference count actually used."""
    cfg = parsed.cfg
    cycles_per_s = _effective_cycles_per_s(parsed)
    power_lsb_w = float(cfg["power_lsb_w"])

    per_window_mj: list[float] = []
    window_ms_list: list[float] = []
    total_samples = 0
    total_inferences = 0
    for w_i in sorted(parsed.samples.keys()):
        phases = parsed.samples[w_i]
        if "active" not in phases or "idle" not in phases:
            raise OnDeviceError(f"window {w_i} is missing an active or idle phase")
        active_span = parsed.uptime_spans.get(w_i, {}).get("active")
        if active_span is None:
            raise OnDeviceError(
                f"window {w_i} has no ENERGY-W line for phase 'active' -- cannot recover "
                "its per-inference divisor")
        n_inferences = active_span[3]
        if n_inferences < 1:
            raise OnDeviceError(
                f"window {w_i} active phase completed 0 inferences (ENERGY-W reports "
                f"inferences={n_inferences}) -- no valid mJ/inference divisor for this pair")
        active = _samples_to_power(phases["active"], cycles_per_s, power_lsb_w)
        idle = _samples_to_power(phases["idle"], cycles_per_s, power_lsb_w)
        per_window_mj.append(windowed_delta(active, idle, n_inferences))
        window_ms_list.append((active[-1][0] - active[0][0]) * 1000.0)
        total_samples += len(active) + len(idle)
        total_inferences += n_inferences

    value = per_window_mj[0] if len(per_window_mj) == 1 else mean(per_window_mj)
    spread = None if len(per_window_mj) < 2 else stdev(per_window_mj)
    return EnergyMeasurement(
        value_mj_per_inference=value,
        rails=[cfg["rail"]],
        n_inferences=total_inferences,
        window_ms=mean(window_ms_list),
        sample_count=total_samples,
        spread_mj=spread,
    )


def capture_diagnostics(parsed: ParsedCapture) -> dict:
    """Everything `EnergyMeasurement` (frozen, fixed field set) can't carry:
    which cycles_per_s won the DT-vs-measured reconciliation (and both raw
    values), the device's own self-reported result as a cross-check, the
    host/device ratio, and any degraded-run evidence (ENERGY-WERR/ENERGY-WARN
    lines, `npu_dispatched: false`) so a degraded capture stays visible
    rather than looking identical to a clean one."""
    dt = float(parsed.cfg["cycles_per_s"])
    measured = _measured_cycles_per_s(parsed)
    used = _effective_cycles_per_s(parsed)
    device_value = (parsed.device_result.get("value_mj_per_inference")
                    if parsed.device_result is not None else None)
    host_value = measurement_from_capture(parsed).value_mj_per_inference
    ratio = round(host_value / device_value, 4) if device_value else None
    return {
        "cycles_per_s_used": used,
        "cycles_per_s_dt": dt,
        "cycles_per_s_measured": measured,
        "device_value_mj_per_inference": device_value,
        "host_vs_device_ratio": ratio,
        "npu_dispatched": bool(parsed.cfg.get("npu_dispatched", False)),
        "windows": sorted(parsed.samples.keys()),
        "werr_lines": list(parsed.werr),
        "warn_lines": list(parsed.warn),
    }


# ---------------------------------------------------------------------------
# The hardware driver (IMPURE: subprocess + labgrid reservation check)
# ---------------------------------------------------------------------------

def resolve_app_dir(app_dir: str | Path | None = None) -> Path:
    """An explicit argument wins; otherwise `ALP_ENERGY_APP_DIR`. Raises
    OnDeviceError rather than guessing a path under examples/ that may not
    exist yet (the on-target app is a concurrent WIP)."""
    resolved = app_dir or os.environ.get(_APP_DIR_ENV)
    if not resolved:
        raise OnDeviceError(
            f"no on-device app directory given -- pass app_dir or set {_APP_DIR_ENV} "
            "to the on-target energy-runner app's source directory")
    return Path(resolved)


def _labgrid_client_bin() -> str:
    return os.environ.get(_CLIENT_ENV, "labgrid-client")


def _default_place() -> str | None:
    return os.environ.get(_PLACE_ENV) or None


def _reservation_held(client: str, place: str) -> bool:
    """`labgrid-client -p <place> show` and look for a non-empty `acquired:`
    line. NEVER acquires or releases the reservation -- it belongs to the
    human/orchestrator running this session; silently grabbing a shared
    board here is exactly what the bench discipline forbids."""
    try:
        proc = subprocess.run([client, "-p", place, "show"], capture_output=True,
                              text=True, timeout=_LABGRID_SHOW_TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise OnDeviceError(
            f"labgrid-client show timed out after {exc.timeout}s for place {place!r}") from exc
    if proc.returncode != 0:
        raise OnDeviceError(
            f"labgrid-client show failed for place {place!r}: {proc.stderr.strip()}")
    for line in proc.stdout.splitlines():
        m = re.match(r"\s*acquired:\s*(.+)", line)
        if m:
            return m.group(1).strip().strip("'\"") not in ("", "None")
    return False


def bench_available(place: str | None = None) -> bool:
    """True only when `labgrid-client` is on PATH AND the place already shows
    a held reservation. Never probes the board itself, and intentionally
    never called at import time."""
    client = _labgrid_client_bin()
    if shutil.which(client) is None:
        return False
    place = place or _default_place()
    if not place:
        return False
    try:
        return _reservation_held(client, place)
    except OnDeviceError:
        return False


def run_on_device(app_dir: str | Path, *, board: str | None = None,
                   place: str | None = None, timeout_s: int = _RAMRUN_TIMEOUT_S,
                   env: dict[str, str] | None = None) -> tuple[EnergyMeasurement, dict]:
    """Build + RAM-run the on-target energy app on a real AEN E8 and return
    (EnergyMeasurement, diagnostics). Steps: verify the labgrid reservation
    is already HELD (never acquired/released here) -> `build.sh` -> `ram-run.sh`
    -> parse + convert the captured console output. Every host-specific value
    (place, client binary, board target, BENCH_ROOT) comes from `env`/the
    process environment or an explicit arg -- never hard-coded (check_local_paths.py)."""
    app_dir = Path(app_dir).resolve()
    client = _labgrid_client_bin()
    place = place or _default_place()
    run_env = dict(env if env is not None else os.environ)
    if board:
        run_env["AEN_BOARD"] = board

    if shutil.which(client) is None:
        raise OnDeviceError(f"'{client}' not on PATH -- install labgrid-client "
                            f"or set {_CLIENT_ENV}")
    if not place:
        raise OnDeviceError(f"no bench place given -- pass place= or set {_PLACE_ENV}")
    if not _reservation_held(client, place):
        raise OnDeviceError(
            f"bench place {place!r} has no held reservation -- acquire it yourself "
            "first (this runner never acquires/releases a shared board)")

    bench_dir = Path(__file__).resolve().parents[2] / "scripts" / "bench" / "aen"
    bench_root = Path(run_env.get("BENCH_ROOT", Path(__file__).resolve().parents[2]))
    build_dir = bench_root / "build" / app_dir.name

    try:
        build_proc = subprocess.run([str(bench_dir / "build.sh"), str(app_dir)],
                                    capture_output=True, text=True,
                                    timeout=_BUILD_TIMEOUT_S, env=run_env)
    except subprocess.TimeoutExpired as exc:
        raise OnDeviceError(f"build.sh timed out after {exc.timeout}s for {app_dir}") from exc
    if build_proc.returncode != 0 or "BUILD FAILED" in build_proc.stdout:
        raise OnDeviceError(
            f"build.sh failed for {app_dir}:\n{build_proc.stdout}\n{build_proc.stderr}")

    try:
        run_proc = subprocess.run([str(bench_dir / "ram-run.sh"), str(build_dir)],
                                  capture_output=True, text=True,
                                  timeout=timeout_s, env=run_env)
    except subprocess.TimeoutExpired as exc:
        raise OnDeviceError(f"ram-run.sh timed out after {exc.timeout}s for {build_dir}") from exc
    if run_proc.returncode != 0:
        raise OnDeviceError(f"ram-run.sh failed for {build_dir}: {run_proc.stderr.strip()}")

    parsed = parse_console(run_proc.stdout)
    measurement = measurement_from_capture(parsed)
    diagnostics = capture_diagnostics(parsed)
    return measurement, diagnostics

#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Design + print the Q15 anti-alias low-pass FIR coefficient tables for
<alp/dsp.h>'s alp_dsp_decimator_t (issue #2134).

Method: windowed-sinc low-pass, Kaiser window.  ONE fixed tap count
(ALP_DSP_DECIMATOR_TAPS = 135) and ONE fixed Kaiser beta are reused for
every supported ratio {2, 3, 4, 6}.

Passband edge (kept from the phase-1 design): a fixed normalised
transition width DF, computed once from a 40 dB/135-tap Kaiser reference
(Oppenheim/Schafer formula), sets `passband_edge = output_nyquist - DF`
per ratio (output_nyquist = fs_in / (2*ratio)).

Stopband edge (the fix): placed at `fs_out - passband_edge` (fs_out =
fs_in / ratio), NOT at output_nyquist -- the latter leaves only an
800 Hz-scale transition band and caps attenuation around ~40 dB.  Moving
the stopband edge out to fs_out - passband_edge doubles the transition
width, which at the same (N, beta) lands stopband attenuation around
70 dB instead.  Content between output_nyquist and fs_out - passband_edge
(ratio 3: 8.0-8.8 kHz) is therefore NOT stopband-attenuated -- it folds
into the transition band above the stated passband.  See
<alp/dsp.h>'s alp_dsp_decimator_init Doxygen for the per-ratio numbers
this trade-off buys.

Kaiser beta = 6.976 hits ~70 dB stopband / <0.01 dB ripple on the
shipped Q15 taps at N=135 (measured on the quantised taps below, not
the float design -- see `measure()`); ~71 dB is about the ceiling a
135-tap Q15 filter can reach at this transition width, so this is not
tuned further.

The Q15 quantiser rounds every tap to the nearest int16 and then nudges
ONLY the centre tap (self-symmetric for this odd tap count, so the
adjustment cannot break the FIR's even symmetry) by the residual so
each shipped table sums to exactly 32768 -- unity DC gain in Q15, not
just in the float design.

This is a leaf design tool (the coefficients do not derive from any other
committed source-of-truth file), so it is intentionally NOT wired into
test-all.sh's `stage_generated_files` regenerate-and-diff loop the way
gen_status_strings.py etc. are.  Run with `--check` instead to verify the
tables embedded in src/dsp_dispatch.c still match this design; re-run
without `--check` and paste the output into src/dsp_dispatch.c and
<alp/dsp.h>'s Doxygen whenever the design parameters above change.

Usage:
    python3 scripts/gen_dsp_decimator_coeffs.py           # print tables
    python3 scripts/gen_dsp_decimator_coeffs.py --check   # verify src/dsp_dispatch.c matches
"""
import re
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("gen_dsp_decimator_coeffs: numpy is required.  Install via `pip install numpy`.")

REPO = Path(__file__).resolve().parents[1]
DISPATCH_C = REPO / "src" / "dsp_dispatch.c"

N = 135
BETA = 6.976  # Kaiser beta; see module docstring -- measured to land ~70 dB on the Q15 taps.
RATIOS = (2, 3, 4, 6)
# fs_in per ratio, for reporting Hz alongside the ratio's normalised specs.
FS_IN_HZ = {2: 32000.0, 3: 48000.0, 4: 32000.0, 6: 48000.0}

# Normalised half-transition width that fixes the PASSBAND edge only (kept
# from the phase-1 design); the stopband edge is derived separately below.
_A_DB_PASSBAND_DESIGN = 40.0
DF = (_A_DB_PASSBAND_DESIGN - 8.0) / (2.285 * 2.0 * np.pi * (N - 1))


def design(ratio):
    """Float taps + the per-ratio passband/stopband edges (normalised,
    cycles/sample of fs_in)."""
    passband_edge = 0.5 / ratio - DF  # output_nyquist - DF
    stopband_edge = 1.0 / ratio - passband_edge  # fs_out - passband_edge
    fc = (passband_edge + stopband_edge) / 2.0
    m = (N - 1) // 2
    n = np.arange(N) - m
    h = 2.0 * fc * np.sinc(2.0 * fc * n)
    h = h * np.kaiser(N, BETA)
    h = h / np.sum(h)  # unity DC gain (float)
    return h, passband_edge, stopband_edge


def q15(h):
    """Round to Q15 and force an EXACT unity-DC sum by nudging only the
    centre tap (index (N-1)/2, its own symmetric partner for odd N)."""
    q = np.round(h * 32768.0)
    q = np.clip(q, -32768, 32767).astype(np.int64)
    centre = (N - 1) // 2
    q[centre] += 32768 - int(np.sum(q))
    return q


def measure(q, fs_in, passband_edge_norm, stopband_edge_norm):
    """Ripple + stopband attenuation measured on the shipped Q15 taps
    (q15(h)/32768), not the float design -- so the printed figures match
    what actually runs on-target."""
    h = q.astype(np.float64) / 32768.0
    n = np.arange(len(h))

    def resp(f):
        w = 2.0 * np.pi * f / fs_in
        return np.sum(h * np.exp(-1j * w * n))

    pb_hz = passband_edge_norm * fs_in
    sb_hz = stopband_edge_norm * fs_in
    mag_pb = np.array([abs(resp(f)) for f in np.linspace(1.0, pb_hz, 6000)])
    mag_sb = np.array([abs(resp(f)) for f in np.linspace(sb_hz, fs_in / 2.0, 12000)])
    ripple_db = 20.0 * np.log10(mag_pb.max() / mag_pb.min())
    stopband_db = -20.0 * np.log10(mag_sb.max())
    return pb_hz, sb_hz, ripple_db, stopband_db


def _build_tables():
    """Return {ratio: (q15_array, pb_hz, sb_hz, ripple_db, stopband_db)}."""
    out = {}
    for ratio in RATIOS:
        h, pb_edge, sb_edge = design(ratio)
        q = q15(h)
        fs_in = FS_IN_HZ[ratio]
        pb_hz, sb_hz, ripple_db, stopband_db = measure(q, fs_in, pb_edge, sb_edge)
        out[ratio] = (q, pb_hz, sb_hz, ripple_db, stopband_db)
    return out


def _format_table(ratio, q):
    lines = [f"static const int16_t _alp_dsp_decim_coeffs_r{ratio}[ALP_DSP_DECIMATOR_TAPS] = {{"]
    for i in range(0, N, 8):
        row = ", ".join(str(int(v)) for v in q[i:i + 8])
        lines.append(f"\t{row},")
    lines.append("};")
    return "\n".join(lines)


def main():
    m = (N - 1) // 2
    tables = _build_tables()
    print(f"/* N={N} taps, Kaiser beta={BETA}, group delay={m} input samples */")
    for ratio in RATIOS:
        q, pb_hz, sb_hz, ripple_db, stopband_db = tables[ratio]
        fs_in = FS_IN_HZ[ratio]
        print(f"\n/* ratio={ratio}  fs_in={fs_in:.0f} Hz  passband_edge={pb_hz:.1f} Hz  "
              f"stopband_edge={sb_hz:.1f} Hz")
        print(f"   achieved: ripple={ripple_db:.4f} dB  stopband_attenuation={stopband_db:.2f} dB"
              f"  group_delay={m / ratio:.2f} output samples */")
        print(_format_table(ratio, q))


def _parse_dispatch_c_tables(text):
    """Extract {ratio: [int, ...]} from the `_alp_dsp_decim_coeffs_r<N>`
    arrays embedded in src/dsp_dispatch.c."""
    found = {}
    pattern = re.compile(
        r"_alp_dsp_decim_coeffs_r(\d+)\[ALP_DSP_DECIMATOR_TAPS\]\s*=\s*\{(.*?)\};", re.DOTALL)
    for m in pattern.finditer(text):
        ratio = int(m.group(1))
        vals = [int(v) for v in re.findall(r"-?\d+", m.group(2))]
        found[ratio] = vals
    return found


def check():
    """Re-derive the tables from the design parameters above and compare,
    tap-for-tap, against what is actually embedded in src/dsp_dispatch.c.
    Exits 1 on any mismatch (missing ratio, wrong length, or a differing
    tap)."""
    if not DISPATCH_C.exists():
        print(f"gen_dsp_decimator_coeffs --check: {DISPATCH_C} not found", file=sys.stderr)
        return 1
    embedded = _parse_dispatch_c_tables(DISPATCH_C.read_text(encoding="utf-8"))
    tables = _build_tables()
    ok = True
    for ratio in RATIOS:
        want = [int(v) for v in tables[ratio][0]]
        got = embedded.get(ratio)
        if got is None:
            print(f"MISMATCH ratio={ratio}: no table found in {DISPATCH_C}", file=sys.stderr)
            ok = False
            continue
        if got != want:
            print(f"MISMATCH ratio={ratio}: embedded table differs from the design "
                  f"(N={len(got)} vs {len(want)} expected)", file=sys.stderr)
            ok = False
    if ok:
        print("gen_dsp_decimator_coeffs --check: OK -- "
              f"{DISPATCH_C} matches the design for ratios {RATIOS}")
        return 0
    return 1


if __name__ == "__main__":
    if "--check" in sys.argv[1:]:
        sys.exit(check())
    main()

#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Design + print the Q15 anti-alias low-pass FIR coefficient tables for
<alp/dsp.h>'s alp_dsp_decimator_t (issue #2134).

Method: windowed-sinc low-pass, Kaiser window.  ONE fixed tap count
(ALP_DSP_DECIMATOR_TAPS = 135) and ONE fixed Kaiser beta are reused for
every supported ratio {2, 3, 4, 6} by holding the normalised transition
width (df, cycles/sample) constant across ratios instead of the passband
edge -- so every ratio gets the same ~40 dB stopband / <0.2 dB ripple
budget from the SAME (N, beta) pair (a fixed passband-edge FRACTION of
the ratio's output Nyquist instead would shrink df, and therefore blow
the spec, at the largest ratio -- see the #2134 PR discussion).

Design parameters (all four ratios):
    N (taps)          = 135                          (odd -> integer group delay)
    target stopband A = 40 dB  ->  Kaiser beta        = 3.395 (Oppenheim/Schafer table)
    transition width df (normalised, cycles/sample)   = (A-8)/(2.285*2*pi*(N-1))
    passband edge      = output_nyquist - df           (per ratio)
    cutoff fc (Kaiser window centre)                   = (passband_edge + output_nyquist)/2

output_nyquist = 0.5/ratio cycles/sample (i.e. fs_in/(2*ratio) Hz) -- the
new Nyquist rate after keep-1-in-`ratio` decimation.

This is a leaf design tool (the coefficients do not derive from any other
committed source-of-truth file), so it is intentionally NOT wired into
test-all.sh's `stage_generated_files` regenerate-and-diff loop the way
gen_status_strings.py etc. are.  Re-run it and diff its output against
the tables embedded in src/dsp_dispatch.c whenever the design parameters
above change; the achieved specs printed here must then be copied into
both src/dsp_dispatch.c's header comment and <alp/dsp.h>'s Doxygen.

Usage:
    python3 scripts/gen_dsp_decimator_coeffs.py
"""
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("gen_dsp_decimator_coeffs: numpy is required.  Install via `pip install numpy`.")

N = 135
A_DB = 40.0
BETA = 3.395  # Kaiser beta for ~40 dB stopband (Oppenheim & Schafer table 7.2)
RATIOS = (2, 3, 4, 6)
# fs_in per ratio, for reporting Hz alongside the ratio's normalised specs.
FS_IN_HZ = {2: 32000.0, 3: 48000.0, 4: 32000.0, 6: 48000.0}

DF = (A_DB - 8.0) / (2.285 * 2.0 * np.pi * (N - 1))  # normalised transition width


def design(ratio):
    out_nyq = 0.5 / ratio
    passband_edge = out_nyq - DF
    fc = (passband_edge + out_nyq) / 2.0
    m = (N - 1) // 2
    n = np.arange(N) - m
    h = 2.0 * fc * np.sinc(2.0 * fc * n)
    h = h * np.kaiser(N, BETA)
    h = h / np.sum(h)  # unity DC gain
    return h, passband_edge, out_nyq


def measure(h, fs_in, passband_edge_norm, out_nyq_norm):
    n = np.arange(len(h))

    def resp(f):
        w = 2.0 * np.pi * f / fs_in
        return np.sum(h * np.exp(-1j * w * n))

    pb_hz = passband_edge_norm * fs_in
    sb_hz = out_nyq_norm * fs_in
    mag_pb = np.array([abs(resp(f)) for f in np.linspace(1.0, pb_hz, 400)])
    mag_sb = np.array([abs(resp(f)) for f in np.linspace(sb_hz, fs_in / 2.0, 800)])
    ripple_db = 20.0 * np.log10(mag_pb.max() / mag_pb.min())
    stopband_db = -20.0 * np.log10(mag_sb.max())
    return pb_hz, sb_hz, ripple_db, stopband_db


def q15(h):
    q = np.round(h * 32768.0)
    return np.clip(q, -32768, 32767).astype(np.int64)


def main():
    m = (N - 1) // 2
    print(f"/* N={N} taps, Kaiser beta={BETA}, target stopband={A_DB} dB, "
          f"df(normalised)={DF:.6f} cycles/sample, group delay={m} input samples */")
    for ratio in RATIOS:
        h, pb_edge, out_nyq = design(ratio)
        fs_in = FS_IN_HZ[ratio]
        pb_hz, sb_hz, ripple_db, stopband_db = measure(h, fs_in, pb_edge, out_nyq)
        c = q15(h)
        print(f"\n/* ratio={ratio}  fs_in={fs_in:.0f} Hz  passband_edge={pb_hz:.1f} Hz  "
              f"output_nyquist={sb_hz:.1f} Hz")
        print(f"   achieved: ripple={ripple_db:.4f} dB  stopband_attenuation={stopband_db:.2f} dB"
              f"  group_delay={m / ratio:.2f} output samples */")
        print(f"static const int16_t _alp_dsp_decim_coeffs_r{ratio}[ALP_DSP_DECIMATOR_TAPS] = {{")
        for i in range(0, N, 8):
            row = ", ".join(str(int(v)) for v in c[i:i + 8])
            print(f"\t{row},")
        print("};")


if __name__ == "__main__":
    main()

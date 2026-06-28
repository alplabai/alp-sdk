# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic baseline
fallback runs without one. To deploy a real detector, train a small
**autoencoder** on YOUR turbine's healthy baseline — anomaly scores are
per-turbine, so a shipped model would not transfer.

## Pipeline

1. **Record a healthy baseline.** Capture nacelle audio across the normal
   operating envelope (cut-in..rated RPM, varied wind/load), tagged with rotor
   RPM from SCADA/tacho. Hours, not minutes.
2. **Extract the feature vector** the device uses: per-frame `acoustic_features`
   (16 floats) averaged over each ~4 s report window, concatenated with the
   `bpf_modulation` order features (5 floats) and the RPM (1) →
   `ANOMALY_INPUT_DIM = 22`.
3. **Train an autoencoder** (e.g. 22→16→8→16→22 dense) on the healthy vectors;
   the per-window **reconstruction error** is the anomaly score. RPM is an input
   so the baseline is speed-aware.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop the result in this folder and point
   `alp_inference_open` at it.
5. **Set the alarm threshold** from the healthy reconstruction-error
   distribution (e.g. 99th percentile), not a guess.

Honest scope: this detects drivetrain tonals + gross blade aero-anomalies. Early
internal cracks / delamination need contact Acoustic-Emission sensing and are out
of scope for an airborne mic.

# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic 5-state
classifier + anomaly fallback run without one. The named states
(OFF/NORMAL/INRUSH/OVERLOAD/STALL) are threshold rules; the AI adds an anomaly
score for **off-taxonomy** faults (early bearing wear, intermittent brush
arcing) that the thresholds miss.

1. **Record a healthy baseline** of the motor's current/voltage/power across its
   duty cycle, at the device window (256 samples @ 200 Hz). Tag with load.
2. **Extract the 7-feature `current_features` vector** per window.
3. **Train an autoencoder** (e.g. 7→4→2→4→7) on the healthy vectors; the
   reconstruction error is the anomaly score.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it in this folder and point `alp_inference_open` at it.

Tune the `curr_config` thresholds (`off_a`, `overload_a`, `ripple_min_a`,
`inrush_slope_a`) to your motor's nameplate current.

Honest scope: DC current-signature monitoring (the INA236's domain), NOT AC-mains
energy disaggregation. Sensorless RPM from commutation ripple needs a higher I2C
readout rate than 200 Hz (Nyquist 100 Hz) and is bench-gated.

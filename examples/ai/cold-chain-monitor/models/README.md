# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic 4-state
classifier + anomaly fallback run without one. The named states are threshold
rules; the AI adds an anomaly score for **subtle multi-variable drift** the
thresholds miss — a slowly degrading compressor, a door-seal leak that nudges
humidity and the warming slope together before any single limit trips.

1. **Record a healthy baseline** of the cabinet's T/RH/P across its normal duty
   cycle (defrost cycles, door openings), at the device window
   (`CC_WINDOW_N` samples per report).
2. **Extract the 8-feature `cc_features` vector** per report window.
3. **Train an autoencoder** (e.g. 8→4→2→4→8) on the healthy vectors; the
   reconstruction error is the anomaly score.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Tune the `cc_config` thresholds (band, MKT limit, excursion-minute limit,
dewpoint margin) to your product's stability data.

Honest scope: a reference logger, NOT a certified GxP / 21-CFR-Part-11 data
logger (audit trail, calibration traceability, tamper-proof storage).

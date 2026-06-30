# Activity model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic
idle/walk/run fallback runs without one. The fall detector is rule-based and
needs no model at all. To get the full taxonomy (incl. STAIRS), train a small
**HAR classifier**:

1. **Collect labelled windows** at the device's 100 Hz / 256-sample window, with
   the 12-feature `motion_features` vector as input and the activity label as
   target. Public starting datasets: **UCI-HAR**, **WISDM** (re-window to match).
2. **Train** a small dense or 1D-CNN classifier over the 12-D feature vector
   (4 classes: idle/walk/run/stairs). Keep it tiny — this is an always-on path.
3. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop the result here and point `alp_inference_open` at it.

The fall detector's thresholds (`FALL_FREEFALL_G`, `FALL_IMPACT_G`, the phase
windows) should be tuned per mounting position (wrist vs belt) on real data.

Honest scope: coarse activity + fall detection; NOT medical-grade and not a
certified fall alarm.

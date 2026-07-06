# Autoencoder model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic statistical
fallback (`defect_score_stat`) runs without one. The fallback flags tiles whose
{mean, variance, gradient-energy} stray from a clean baseline — good for obvious
defects, but a trained autoencoder catches subtle texture anomalies a fixed
statistic misses.

1. **Collect good-surface images only** — many crops of defect-free product
   under production lighting. No defect labels are needed (that is the point of
   the unsupervised approach).
2. **Train a small convolutional autoencoder** to reconstruct the 64×64 luma
   inspection grid, minimising reconstruction MSE on the good-surface set.
3. **Set the threshold** from the clean validation set's per-tile error
   distribution (e.g. mean + k·σ), so good surface passes and real defects —
   which reconstruct far worse — exceed it. Feed it to `defect_classify`.
4. **Quantise + compile:** TFLite (uint8/int8 in/out) → **Vela** for Ethos-U
   (AEN) or the **DX-M1** toolchain for V2N. Drop it here and point
   `alp_inference_open` at it; `main.c` then takes the reconstruction path.

Retune the `defect_baseline` (nominal + tolerance per statistic) to the clean
surface — the single most important calibration for the fallback path.

Honest scope: reference inspection logic; the per-tile statistics are
lightweight and the threshold is a calibrated constant (no runtime auto-tuning).

# Fused model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic cross-modal
fusion rule (`fusion_assess`) runs without one. The named hypotheses are
threshold + corroboration rules; a trained model can learn subtler fused
signatures (a fault that splits its energy across modalities below any single
threshold, or a machine-specific cross-correlation).

1. **Collect labelled runs** of the machine across its fault modes, logging the
   `fusion_input` 6-field summary (or the 9-element `fusion_pack` vector) per
   report window, tagged with the ground-truth fault.
2. **Train a small classifier** over the `FUSION_FEATURE_DIM` (9) fused vector
   → `FUSION_FAULT_COUNT` (5) classes, or an autoencoder for an anomaly score.
3. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Retune the `fusion_baseline` (nominal + tolerance per field) to the machine's
healthy operating point — this is the single most important calibration step.

Honest scope: reference fusion logic; the per-modality summaries are lightweight
(the dedicated single-modality examples do the richer DSP).

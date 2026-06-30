# Event model -- training recipe

This example ships **no model** (a 1-byte stub); the deterministic threshold
classifier runs without one. For robust field detection, train a small
**4-class classifier** (ambient / glass-break / alarm / scream):

1. **Collect labelled clips** and window them to the device's 512-sample @ 16 kHz
   frame. Public starting datasets: **UrbanSound8K**, **ESC-50**, and AudioSet
   glass-break / scream subsets. Augment with your deployment's background noise.
2. **Extract the `ASE_FEATURE_DIM` feature vector** per frame (8 bands + centroid,
   flatness, rolloff, crest, ZCR, RMS) -- or train directly on a log-mel
   spectrogram if you prefer a CNN.
3. **Train** a small dense/CNN classifier; calibrate the decision threshold on a
   held-out set to control the false-alarm rate.
4. **Quantise + compile:** TFLite -> **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Honest scope: detects loud, acoustically-distinct events; NOT a certified
security or life-safety sensor. Confounders (music, TV, clattering dishes) drive
false positives -- real deployment needs the trained model + noise augmentation.

# visual-defect-detection

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `defect_map` core
> is host-unit-tested on `native_sim/native/64`; the full app runs end-to-end on
> native_sim with synthetic clean + defect frames. HiL with a real CSI camera +
> a trained autoencoder is bench-gated.

A camera-fed surface-inspection station that flags manufacturing defects as
**unsupervised anomalies**: an autoencoder reconstructs the "normal" surface,
and a region it reconstructs poorly (high error) is a defect — including defect
types never seen in training.

## Why unsupervised

Good product is abundant and uniform; defects are rare, varied, and costly to
label. An autoencoder trained only on good surface flags anything it cannot
reconstruct, so one model needs no defect labels and catches novel defects — a
supervised classifier must see every defect class up front.

## Pipeline

```
CSI camera --RGB565--> 64x64 luma grid --> autoencoder (reconstruction)
  -> per-tile anomaly score (recon error, or statistical fallback)
  -> worst tile (tx,ty) + coverage % + severity + PASS/FAIL
```

## Output

```
# DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score
DEFECT,1,PASS,0.00,0.0,0,0,0.33
DEFECT,2,FAIL,0.98,1.6,5,3,1.98
```

`worst_tx/worst_ty` are in [0,7] and locate the worst tile in the 8x8 grid;
`coverage_pct` is the fraction of tiles flagged; `severity` is in [0,1].

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/visual-defect-detection
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the V2N accelerator path.

## Model

No model is shipped (stub + statistical fallback). See `models/README.md` for
the autoencoder training recipe. The most important calibration is the clean
`defect_baseline` (and, with a model, the per-tile error threshold).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/defect_map
twister -p native_sim/native/64 -T examples/ai/visual-defect-detection
```

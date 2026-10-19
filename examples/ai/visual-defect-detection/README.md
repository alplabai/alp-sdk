# visual-defect-detection

> **`[UNTESTED]` on hardware -- paper-correct.** The `defect_map` core
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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/ai/visual-defect-detection`)

board.yaml declares only `m55_hp`, so tan also builds the SDK's idle alp-stock-shim image for `m55_he` (`build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/ai/visual-defect-detection/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/ai/visual-defect-detection
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/ai/visual-defect-detection -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Model

No model is shipped (stub + statistical fallback). See `models/README.md` for
the autoencoder training recipe. The most important calibration is the clean
`defect_baseline` (and, with a model, the per-tile error threshold).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/defect_map
python3 scripts/gen_example_alp_conf.py examples/ai/visual-defect-detection   # writes generated/alp.conf (#866)
twister -p native_sim/native/64 -T examples/ai/visual-defect-detection
```

# multimodal-fusion-pdm

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `fusion_health`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic per-modality data covering every
> scenario. HiL with the three real sensors + a trained model is bench-gated.

A multi-sensor motor-health monitor that **fuses** vibration (ICM-42670),
current (INA236), and temperature (BME280) into one fault hypothesis + a
confidence-weighted health score via cross-modal corroboration.

## Why fusion

A real fault corroborates across modalities -- bearing wear raises vibration AND
temperature; an overload raises all three. An isolated single-modality blip (a
sensor knock, a transient) does not corroborate, so it is flagged
`UNCORROBORATED` at low confidence instead of crying wolf. That corroboration is
how fusion suppresses the false positives a bank of single-sensor thresholds
would raise.

## Fault hypotheses

| Pattern (vibration / current / temperature anomalous) | Hypothesis |
|---|---|
| none | `HEALTHY` |
| vibration + temperature | `BEARING_WEAR` |
| current (vibration normal) | `ELECTRICAL_FAULT` |
| all three | `MECHANICAL_OVERLOAD` |
| a single modality | `UNCORROBORATED` (low confidence) |

## Pipeline

```
ICM-42670 + INA236 + BME280 --summary--> fusion_health (sub-scores ->
  corroboration -> hypothesis + health) -> <alp/inference.h> fused model
  (deterministic fusion-rule fallback) -> FUSE record
```

## Output

```
# FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration
FUSE,2.0,BEARING_WEAR,1.00,3.0,0.0,3.0,2
FUSE,5.0,UNCORROBORATED,0.50,3.0,0.0,0.0,1
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/multimodal-fusion-pdm
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fusion rule). See `models/README.md`
for the training recipe. The most important calibration is the per-machine
`fusion_baseline`.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/fusion_health
```

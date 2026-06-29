# cold-chain-monitor

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `cold_chain` core
> is host-unit-tested on `native_sim/native/64`; the full app runs end-to-end on
> native_sim with synthetic environment data covering the states. HiL with a
> real BME280 + a trained model is bench-gated.

A pharma/food **cold-chain integrity monitor**: sample a BME280 T/RH/P sensor on
a slow logging cadence, compute the standards-backed metrics (temperature
excursion time, Mean Kinetic Temperature, dewpoint), classify the integrity
state, and emit an AI anomaly score. The *environmental* edge-AI vertical.

## Honest scope

A reference cold-chain logger. MKT, dewpoint, and excursion-time are the real,
recognized metrics; thresholds are configurable per product. **NOT** a certified
GxP / 21-CFR-Part-11 data logger -- no validated audit trail, no calibration
traceability, no tamper-proof storage.

## Standards-backed metrics

- **MKT** (Mean Kinetic Temperature, ICH Q1A / USP <1079>): the single
  temperature delivering the same cumulative thermal stress as the fluctuating
  profile. `MKT >= arithmetic mean`, so a brief hot spike counts for more than
  its duration -- which is the whole point.
- **Dewpoint** (Magnus): condensation / mould risk when ambient T nears it.

## Pipeline

```
BME280 (I2C) --window--> cold_chain (mean/min/max, MKT, dewpoint, excursion)
  -> cc_classify (OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK)
  -> <alp/inference.h> anomaly score (deterministic fallback) -> CC record
```

## Output

```
# CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min
CC,256.0,OK,5.0,50.0,-4.4,5.0,0.0
CC,768.0,CONDENSATION_RISK,5.0,95.0,4.3,5.0,0.0
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/cold-chain-monitor
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic classifier/fallback). See
`models/README.md` for the autoencoder training recipe.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/cold_chain
```

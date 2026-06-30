# motor-current-signature

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `current_features`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic current covering all five states. HiL
> on a real motor + a trained model is bench-gated.

A DC motor / load **current-signature health monitor**: sample an INA236
high-side current/voltage/power monitor, extract a feature window, classify the
operating **state** (OFF/NORMAL/INRUSH/OVERLOAD/STALL), and emit an AI **anomaly
score** for off-taxonomy faults. The *electrical* PdM modality, complementing the
vibration and acoustic examples.

## Honest scope

The INA236 is a **DC** high-side shunt monitor. This is DC-rail / brushed-DC-motor
current-signature analysis (MCSA-style) -- it is **NOT** AC-mains NILM / energy
disaggregation (that needs a dedicated AC energy-metering front-end). Monitor-only
(pairing with a `drv8833`/`a4988` driver to spin the motor is a noted extension).
At 200 Hz, resolvable ripple is < 100 Hz (Nyquist); sensorless RPM from faster
commutation ripple is bench-gated.

## Key discriminator

`rms_ac_a` (AC ripple magnitude) separates **STALL** (high current, *no*
commutation ripple -> rotor not turning) from **OVERLOAD** (high current *with*
ripple -> turning under load).

## Pipeline

```
INA236 (I2C, ~200 Hz) --window--> current_features (mean/ripple/crest/slope/power)
  -> current_classify (OFF/NORMAL/INRUSH/OVERLOAD/STALL)
  -> <alp/inference.h> anomaly score (deterministic fallback) -> CURR record
```

## Output

```
# CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score
CURR,1.28,NORMAL,1.02,12.3,39.8,0.08
CURR,5.12,STALL,3.10,37.2,0.0,0.90
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/motor-current-signature
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic classifier/fallback). See
`models/README.md` for the autoencoder training recipe.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/current_features
```

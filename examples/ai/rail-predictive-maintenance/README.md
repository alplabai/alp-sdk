# rail-predictive-maintenance

> **`[UNTESTED]` on hardware -- paper-correct.** The DSP cores
> (`rail_features`, `rail_position`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> a canned NMEA track + synthetic vibration. HiL on a real bogie with a
> customer-trained model is bench-gated.

Train-mounted **rail-condition survey**: read axlebox/carbody vibration
from the on-board ICM-42670, extract DSP features, classify the rail
defect, and **geotag** each verdict to a track position (GNSS lat/lon +
along-track chainage). One CSV record per 25 m segment, ready for the
customer's GIS / MQTT gateway.

This monitors the **rail (infrastructure)**, complementing
`ai-anomaly-detection-vibration`, which monitors a **stationary asset**
(motor/pump bearing). Wheel-side defects (wheel flats) are out of scope.

## Pipeline

```
ICM-42670 (I2C) --window--> rail_features (RMS, crest, kurtosis, 8 FFT
  bands, dom-freq, rail wavelength = speed/freq) --feature vector-->
  <alp/inference.h> classifier (deterministic fallback if no model)
NEO-M9N GNSS (UART NMEA) --> rail_position (haversine chainage + segments)
  --> CSV record per segment
```

`λ = v / f` makes corrugation detection speed-invariant: corrugation is
fixed in *wavelength*, not frequency, so the speed-normalised wavelength
is the feature the classifier keys on.

## Defect taxonomy

| Class | Signature |
|-------|-----------|
| `HEALTHY` | baseline |
| `CORRUGATION` | periodic rail roughness (narrowband, stable wavelength) |
| `JOINT_WELD` | impulsive transient (high crest + kurtosis) |
| `ROUGH_RCF` | broadband roughness / rolling-contact fatigue |

## Output

```
# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,rail_wavelength_m,fix
RAIL,100.0,59.334591,18.062400,6.2,CORRUGATION,0.81,120.0,0.0517,1
```

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/ai/rail-predictive-maintenance`)

board.yaml declares only `m55_hp`, so tan also builds the SDK's idle alp-stock-shim image for `m55_he` (`build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/ai/rail-predictive-maintenance/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/ai/rail-predictive-maintenance
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/ai/rail-predictive-maintenance -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Drop in your own model

Place a Vela-compiled (AEN / Ethos-U) or DX-M1 (V2M101/102/103) `.tflite` and point
`alp_inference_open` at it. Train a 4-class classifier
(`HEALTHY/CORRUGATION/JOINT_WELD/ROUGH_RCF`) over the
`RAIL_FEATURE_DIM`-float feature vector at the same 256-sample @ 800 Hz
window the app uses. With no model the deterministic fallback runs.

A real model needs a tensor arena: either pass a static `arena`/`arena_bytes`
in the `alp_inference_config_t` (see the sibling `ai-anomaly-detection-vibration`,
which uses a 128 KiB static arena) or set `CONFIG_HEAP_MEM_POOL_SIZE` so the
backend can heap-allocate; with the stub model the deterministic fallback runs
and no arena is needed.

## Tests

```sh
# host unit tests for the DSP cores
twister -p native_sim/native/64 -T tests/unit/rail_features -T tests/unit/rail_position
```

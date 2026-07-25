# aen-inference-energy — millijoules per inference, measured on silicon

Runs a Vela-compiled model on the Ethos-U85 while sampling one of the EVK's
INA236 rail monitors, and reports the **incremental energy per inference** as a
carrier-rail delta. Board target
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`.

The measured result, the full method, the whole-board cross-check and the error
budget live in [`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md).
Read the "What this is NOT" section there before quoting any number this app
prints: it is **not** NPU energy, **not** silicon energy, and **not** comparable
to a vendor datasheet figure.

## What it does

1. Probes all six EVK INA236 monitors and picks the compute rail from the board —
   the rail whose shunt voltage steps significantly (>= 3 standard errors)
   between inferring and quiet. If nothing steps significantly it falls back to
   the highest-current rail and says so, instead of ranking rails by noise.
2. Picks the finer +/-20.48 mV ADC range when the observed swing fits with
   headroom, else stays on +/-81.92 mV.
3. Runs M pairs of equal-length (active, idle) windows, sampling on the INA236
   conversion-ready flag so each conversion is consumed exactly once, and
   timestamping with the M55 DWT cycle counter.
4. Prints every raw sample so the host re-integrates independently, plus its own
   on-target integral as a cross-check, plus a `RESULT PASS/FAIL` line.

It reports `RESULT FAIL` — deliberately — when the delta is not resolvable above
the rail's noise. A tiny model does exactly that; see "Model choice" below.

## Model choice: a trivial model cannot be measured

Bench-confirmed. With the hermetic `tiny_int8.tflite` fixture (8 MACs) the app
reports:

```
RESULT FAIL: delta not resolvable -- mean=-0.000003 spread=0.000000 ...
             (inference load below this rail's noise, or wrong rail)
```

That is the correct outcome, not a bug: 8 MACs of NPU work is far below the
resolution of every shunt on the board, and the idle baseline (CPU spinning, NPU
quiet) correctly cancels the CPU work that dominates such a call. Measuring
energy needs a model that does real work — `person_detect` is 7,077,252 MACs,
100 % NPU, and produces a clean 190 uV step on the +5V rail.

`person_detect` Vela's to ~237 KiB, which does not fit the 256 KiB ITCM, so the
real-model build is MRAM-resident (Flow D). The tiny-model ITCM build (Flow C)
remains useful as a fast, non-destructive check that the pipeline is intact.

## Build + run — real model, Flow D (the measuring configuration)

```sh
export PATH="$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin:$PATH"
export JLINK_SN=603000869          # REQUIRED on a multi-probe host; AEN E8 answers SW-DP 0x4C013477
export SETOOLS_DIR=<...>/app-release-exec-linux
A=$PWD/examples/aen/aen-inference-energy

scripts/bench/aen/build.sh "$A" \
  -DEXTRA_DTC_OVERLAY_FILE="$A/flowd/mram-slot0.overlay" \
  -DEXTRA_CONF_FILE="$A/flowd/mram-slot0.conf" \
  -DAEN_NPU_MODEL=<tflite-micro>/tensorflow/lite/micro/models/person_detect.tflite \
  -DAEN_NPU_MODEL_NAME=person_detect_u85

scripts/bench/aen/flash-jlink-mramxip.sh "$BENCH_ROOT/build/aen-inference-energy"
```

**Then let it run undisturbed and read afterwards.** The flash helper ends with a
console read, and a J-Link `qc` leaves the core HALTED — so that read freezes the
app part-way through, and the truncated console looks exactly like a crash. Reset
it and wait out the whole run before reading:

```sh
printf 'connect\nRSetType 2\nr\ng\nqc\n' > /tmp/rst.jlink
JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1 \
         -SelectEmuBySN "$JLINK_SN" -CommanderScript /tmp/rst.jlink
sleep 45                                                  # default build runs ~8 s; 45 s is ample
scripts/bench/aen/reread.sh "$BENCH_ROOT/build/aen-inference-energy" 0x14000
```

The `0x14000` read size matters: this app's console buffer is 80 KB because it
prints one line per conversion, and the default read size would truncate the
capture mid-window.

## Fast iteration — tiny model, Flow C (no MRAM write)

```sh
scripts/bench/aen/build.sh "$A"
scripts/bench/aen/ram-run.sh "$BENCH_ROOT/build/aen-inference-energy" 20000 0x14000
```

Expect `RESULT FAIL: delta not resolvable` — see "Model choice". This build
proves the I2C bus, the rail scan, the NPU dispatch and the sampling loop, which
is what you want when iterating on the app rather than on a measurement.

## Knobs

All are CMake cache variables forwarded to the compiler by `CMakeLists.txt`
(a bare `-D` reaches CMake but **not** the compiler — the forwarding is
explicit, because without it an override is accepted silently and the build is
byte-identical while measuring something else):

| Knob | Default | Why you would change it |
|---|---|---|
| `AEN_ENERGY_SAMPLES_PER_WINDOW` | 250 | Window length = this x 4.48 ms. Raise for a slow external instrument to resolve each phase. |
| `AEN_ENERGY_WINDOW_PAIRS` | 3 | Fewer than 2 yields no spread; the app then reports the spread as negative to mark "not measured". |
| `AEN_ENERGY_EMIT_SAMPLES` | 1 | Set 0 for long windows: energy is integrated in flight, so window length stops being bounded by the console buffer. That build is summary-only and NOT parseable by the host re-integrator. |
| `AEN_ENERGY_RAIL_ADDR` | auto | Pin a monitor by 7-bit address (e.g. `0x4A`) when you know which rail feeds the module and the workload does not move it measurably. |
| `AEN_NPU_MODEL` / `AEN_NPU_MODEL_NAME` | tiny_int8 fixture | Swap the model. |
| `AEN_NPU_VELA_CONFIG` | unset | The Alif proprietary `ensemble_vela.ini` (from `alp-sdk-internal`). Unset still runs on the NPU — this app pins every region to the SRAM AXI port — but the command stream is not the bench-matched one. |

Whole-board cross-check configuration (~17.9 s per phase, one pair -- kept
under the 26.8 s cycle-counter wrap, above which `span_cycles` is meaningless):

```sh
  -DAEN_ENERGY_SAMPLES_PER_WINDOW=4000 -DAEN_ENERGY_WINDOW_PAIRS=1 \
  -DAEN_ENERGY_EMIT_SAMPLES=0 -DAEN_ENERGY_RAIL_ADDR=0x4A
```

## Console protocol

Machine-readable, consumed by `scripts/alp_model/ondevice.py`:

| Line | Meaning |
|---|---|
| `ENERGY-SCAN <rail> <addr> active_uv=.. idle_uv=.. delta_uv=.. se_uv=.. significant=..` | one per probed monitor |
| `ENERGY-CFG {json}` | the calibration + timing the run used (scaling factors, rail, ADC range, sample period, cycles/s, baseline, selection criterion) |
| `ENERGY-PHASE <i> <phase> begin\|end uptime_ms=..` | phase boundaries, for aligning an external instrument's trace |
| `ENERGY-S <i> <phase> <cycles> <power_raw>` | one per conversion: cycle timestamp + raw POWER count |
| `ENERGY-W <i> <phase> <n> <span_cycles> <span_ms>` | window summary; the millisecond span is an independent clock, so the host can verify cycles-per-second rather than trust it |
| `ENERGY-WPART` / `ENERGY-WERR` | the emitted stream is shorter than what was integrated / the window hit its deadline or saw I2C errors |
| `ENERGY-PAIR <i> active_mj=.. idle_mj=.. n=.. mj_per_inference=..` | per-pair result |
| `ENERGY-RESULT {json}` | the device's own answer in the host's `EnergyMeasurement` schema |

`source` and `scope` in `ENERGY-RESULT` are the labels the host contract
validates (`scripts/alp_model/measure.py`). They are emitted verbatim and must
never be "upgraded" to an NPU or silicon scope by any consumer.

## Related

- [`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md) — result, method, error budget, caveats
- `examples/aen/aen-npu-inference-alif` — the silicon-proven NPU dispatch this app
  reuses (its `gen_model.py` is shared, not copied, and its two strong Ethos-U
  overrides are repeated here because they must be strong in the app image)
- `chips/ina236` — the driver, with its SBOSA81D citations
- `scripts/alp_model/ondevice.py` — the host runner that parses this console

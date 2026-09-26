# aen-inference-energy — millijoules per inference, measured on silicon

Runs a Vela-compiled model on the Ethos-U85 while sampling one of the EVK's
INA236 rail monitors, and reports the **incremental energy per inference** as a
carrier-rail delta. Board target `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same silicon variant + PCB as `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he`,
also supported — `boards/`).

The measured result, the full method, the whole-board cross-check and the error
budget live in [`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md).
Read the "What this is NOT" section there before quoting any number this app
prints: it is **not** NPU energy, **not** silicon energy, and **not** comparable
to a vendor datasheet figure. **Hardware fact:** the EVK's default rail, U30 @
I2C `0x4A`, measures the WHOLE +5V rail — SoM + LCD + carrier together — not
module-isolated power.

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
the rail's noise. A tiny model sits right at that noise floor; see "Model
choice" below.

## Model choice: a trivial model is too small to measure reliably

With the hermetic `tiny_int8.tflite` fixture, 8 MACs of NPU work sits right at
the resolution floor of every shunt on the board, so the app's `RESULT` can
come out either way depending on board load. A clean HE run on an
E1M-AEN803 EVK on 2026-09-26 printed:

```
RESULT PASS: 0.000138 mJ/inference (+/-0.000010) ... pairs=3/3
```

— resolvable under the 3-standard-error bar. A warm run with HP contending for
the same rail was not resolvable and reported `RESULT FAIL: delta not
resolvable`. Neither is a bug: at this MAC count the idle baseline (CPU
spinning, NPU quiet) correctly cancels the CPU work that dominates such a
call, and whether the remaining NPU delta clears the noise floor depends on
how quiet the rail happens to be during that run. `person_detect` is the
meaningful measurement, at 7,077,252 MACs,
100 % NPU, and produces a clean, several-hundred-microvolt step on the +5V
rail (190 uV in an earlier bench pass; 172.5 uV in the primary run recorded in
[`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md)
— different runs, not a contradiction).

`person_detect` Vela's to ~237 KiB, which does not fit the 256 KiB ITCM, so the
real-model build is MRAM-resident (Flow D). The tiny-model ITCM build (Flow C)
remains useful as a fast, non-destructive check that the pipeline is intact.

## Build + run — real model, Flow D (the measuring configuration)

```sh
export PATH="$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin:$PATH"
export LG_PLACE=<your-bench-place>       # you must already hold this place's reservation
export LG_COORDINATOR=<host>:<port>
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
it and wait out the whole run before reading — routed through the SAME
`LG_PLACE`-resolved `bench_jlink_run()` every other helper in this directory uses
(never a raw `JLinkExe -SelectEmuBySN <serial>`: a hardcoded probe serial in a
public doc can reset whichever OTHER bench place happens to share that cloned
serial, not just this one — see `scripts/bench/aen/bench-env.sh`'s DP-ID safety
gate comment):

```sh
source scripts/bench/aen/bench-env.sh
cat > /tmp/rst.jlink <<'EOF'
connect
RSetType 2
r
g
qc
EOF
bench_jlink_run -device Cortex-M55 -if SWD -speed 4000 -nogui 1 -CommanderScript /tmp/rst.jlink
sleep 45                                                  # default build runs ~8 s; 45 s is ample
scripts/bench/aen/reread.sh "$BENCH_ROOT/build/aen-inference-energy" 0x10000
```

The read size you pass to `reread.sh`/`ram-run.sh` and this app's
`CONFIG_RAM_CONSOLE_BUFFER_SIZE` (`examples/aen/aen-inference-energy/prj.conf`)
are two different things: the read size is how much the bench script asks
JLinkExe to read back; the buffer size is how much the firmware's RAM console
actually holds. This app's default buffer is 65536 bytes (`0x10000`), with
headroom for the default knobs, and the default `reread.sh`/`ram-run.sh` read
size matches it. Raise `AEN_ENERGY_SAMPLES_PER_WINDOW` or
`AEN_ENERGY_WINDOW_PAIRS` past the documented default and the capture can
exceed that buffer — raise `CONFIG_RAM_CONSOLE_BUFFER_SIZE` to match (there is
no cap on the firmware side), then pass the same larger size as the read: both
scripts chunk any read above `0x10000` automatically (alp-sdk#2313, via
`bench_mem8_chunks()` in `bench-env.sh`), so JLinkExe's own
`NumBytes > 0x10000` rejection no longer applies at this layer, and the
read comes back complete.

## Fast iteration — tiny model, Flow C (no MRAM write)

```sh
scripts/bench/aen/build.sh "$A"
scripts/bench/aen/ram-run.sh "$BENCH_ROOT/build/aen-inference-energy" 20000 0x10000
```

Expect `RESULT PASS` or `RESULT FAIL: delta not resolvable` depending on board
load — see "Model choice". This build
proves the I2C bus, the rail scan, the NPU dispatch and the sampling loop, which
is what you want when iterating on the app rather than on a measurement.

**Caveat -- a resident ATOC can steer `ram-run.sh` onto the wrong core.**
`ram-run.sh` attaches by AP index, and on a board whose flash already carries
an ATOC that boots `HP_APP` (seen on an E1M-AEN803 EVK), that AP can resolve to the
M55-HP rather than the M55-HE this app targets. Before trusting a RAM-run
result:

- Confirm the target core first: in the attached context, HE ITCM at
  `0x58000000` must be readable and `0x50000000` must NOT be (that address is
  HP's ITCM, not HE's). If it comes back the other way round, the wrong core
  is attached.
- A clean HE run under a resident ATOC needs a real core reset, not just a
  RAM load: halt HE, set DEMCR (`0xE000EDFC`) = `0x01100001` (VC_CORERESET),
  set AIRCR (`0xE000ED0C`) = `0x05FA0004`, reload the image, zero
  MSPLIM/PSPLIM, set MSP/PC/xPSR from the vector table, then resume. Also
  clear the NVIC enable/pending state first — the resident app can leave an
  IRQ enabled (IRQ 333 / CDC_SCANLINE0 was seen left enabled), which fires
  into the freshly loaded image before it has installed its own handlers.
- Simplest fix: run from a board or ATOC slot with no resident image, which
  sidesteps the whole class of trap.

This is a caveat on the *procedure*, not a change to
`scripts/bench/aen/ram-run.sh` itself — that script is unchanged.

**Rail baseline is per-carrier, not universal.** On AEN803 + the LCD panel,
the idle +5V rail read ~2.2 W (POWER register mean 2.2060 W), versus ~0.45 W
implied by [`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md)'s
"The measured result" idle window (~495.6 mJ / ~1.10 s) on the bare AEN801
bench. That is an observation about THIS carrier's load (SoM + LCD + carrier
all on one +5V rail — see "Hardware fact" above), not a new headline number:
the LCD, not the SoC, is most of the difference.

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
| `AEN_ENERGY_RAIL_ADDR` | auto | Pin a monitor by 7-bit address (e.g. `0x4A`, the EVK's whole +5V rail — SoM + LCD + carrier, not module-isolated) when you know which rail actually carries the workload's current and it does not move measurably. |
| `AEN_NPU_MODEL` / `AEN_NPU_MODEL_NAME` | tiny_int8 fixture | Swap the model. |
| `AEN_NPU_VELA_CONFIG` | unset | The Alif proprietary `ensemble_vela.ini` (from `alp-sdk-internal`). Unset still runs on the NPU — this app pins every region to the SRAM AXI port — but the command stream is not the bench-matched one. |

Whole-board cross-check configuration (~17.9 s per phase, one pair -- kept
under the 26.8 s cycle-counter wrap, above which `span_cycles` is meaningless):

```sh
  -DAEN_ENERGY_SAMPLES_PER_WINDOW=4000 -DAEN_ENERGY_WINDOW_PAIRS=1 \
  -DAEN_ENERGY_EMIT_SAMPLES=0 -DAEN_ENERGY_RAIL_ADDR=0x4A
```

## Console protocol

Machine-readable, consumed by a host-side runner. That runner lives in
`tan-cli` (see alp-sdk#1470 / ADR-0028), not in this repo:

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

`source` and `scope` in `ENERGY-RESULT` are the labels the host-side contract
validates. They are emitted verbatim and must never be "upgraded" to an NPU or
silicon scope by any consumer.

## Related

- [`docs/measuring-inference-energy.md`](../../../docs/measuring-inference-energy.md) — result, method, error budget, caveats
- `examples/aen/aen-npu-inference-alif` — the silicon-proven NPU dispatch this app
  reuses (its `gen_model.py` is shared, not copied, and its two strong Ethos-U
  overrides are repeated here because they must be strong in the app image)
- `chips/ina236` — the driver, with its SBOSA81D citations
- This app ships only the on-device firmware and the console protocol above;
  the host-side runner that parses it (formerly `scripts/alp_model/ondevice.py`
  and `measure.py`) lives in `tan-cli` (see alp-sdk#1470 / ADR-0028) and is
  not part of alp-sdk

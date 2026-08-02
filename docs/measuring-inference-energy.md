# Measuring on-device inference energy (E1M-AEN801 / Ensemble E8)

How many millijoules does one inference cost? This page records the measured
answer for one model on one board, the method that produced it, an error budget
built from measurements rather than assumptions, and — most importantly — the
list of things the number is **not**.

Everything below was measured on real silicon on an E1M-AEN801 (Alif Ensemble
E8, Cortex-M55 HE core at 160 MHz) with the Ethos-U85 NPU dispatching a real
model. Nothing here is estimated or scaled from a datasheet.

## The measured result

```
0.017 mJ per inference  (17 microjoules)
  model      person_detect int8 MobileNet, Vela-compiled for ethos-u85-256
             242848 bytes, 7,077,252 MACs/inference, 100 % NPU operators
             input 9216 B (96x96x1), output 2 B
  rail       +5V, INA236B U30 @ I2C 0x4A, 20 mOhm shunt
  scope      carrier-rail-delta   (see "What this is NOT")
  n          2886 inferences per active window
  window     1120 ms nominal per window (measured 1101-1105 ms active / 1102-1107 ms idle)
  samples    1500 INA236 conversions (250 per window x 2 phases x 3 pairs)
  scaling    CURRENT_LSB 31.25 uA, POWER LSB 1.0 mW (ADCRANGE=1, matched -- below)
  spread     +/-0.000200 mJ across the 3 window pairs  (1.2 % of the value)
  per pair   0.017285 / 0.017019 / 0.017409 mJ
  windows    active ~545.3 mJ, idle ~495.6 mJ -> delta ~49.7 mJ per 1.10 s
  latency    383 us per inference (derived: 1101 ms / 2886)
```

**Two significant figures is the supportable precision** — see the error budget.
Five independent runs across different build configurations and a 24x range of
window length agree within +/-9 %:

| Run | Window | POWER LSB | mJ/inference |
|---|---|---|---|
| cycle-counter deadline | 1.12 s x 3 pairs | 3.906 mW | 0.018406 |
| ms deadline | 1.12 s x 3 pairs | 3.906 mW | 0.017177 |
| long window | 26.6 s x 1 pair | 3.906 mW | 0.016832 |
| matched CURRENT_LSB | 1.12 s x 3 pairs | 1.0 mW | 0.016955 |
| **all review fixes (primary)** | 1.12 s x 3 pairs | **1.0 mW** | **0.017237** |

The last row is the one to quote. It is the only run with the corrected
statistics (see the scan table's note), the rounded `SHUNT_CAL`, and the
current-based rail ranking; the earlier rows are kept because their agreement
across five different builds is itself the evidence for the +/-9 % figure.

### Why the POWER LSB is 1.0 mW and not 3.906 mW

CURRENT_LSB sets the *reporting* scale of the CURRENT and POWER registers; it
cannot create resolution the shunt ADC does not have. The ADC's real step is the
shunt LSB over the shunt resistance, and it saturates at the ADCRANGE full
scale. For this rail on the +/-20.48 mV range those are 625 nV / 20 mOhm =
31.25 uA and 20.48 mV / 20 mOhm = 1.024 A.

The board data rates the +5V rail at 4.0 A (correctly — 4.0 A x 20 mOhm =
80 mV, essentially the +/-81.92 mV coarse-range full scale). Deriving
CURRENT_LSB from that rating while running on the *fine* range gave 122.07 uA:
**3.91x coarser than the ADC had resolved, on a reporting scale 3.91x wider
than the shunt can physically reach.** `chips/ina236` now clamps the derivation
to the range's own full-scale current, which yields exactly the matched
31.25 uA / 1.0 mW.

Measured effect of that 3.91x: the answer moved from 0.017177 to 0.016955
mJ/inference, **1.3 % — inside the run-to-run spread.** Quantisation was
therefore never a limiting term here, which is why it does not appear in the
error budget. It is fixed because it was free and because a future measurement
of a smaller delta would need it.

### Would bigger shunt resistors help?

Increasing the shunt is the only lever that raises actual signal (microvolts per
milliamp) rather than just reporting scale. It is not limiting here. The
measured load is 102 mA, which is 2.04 mV across 20 mOhm — **10 % of the fine
range** — so the headroom is already unused rather than exhausted:

| R_shunt | V at 102 mA | % of FS | Max measurable I | 8.6 mA delta | counts @625 nV | I^2R | series drop |
|---|---|---|---|---|---|---|---|
| 20 mOhm (fitted) | 2.04 mV | 10.0 % | 1.024 A | 172 uV | 276 | 0.21 mW | 2.0 mV |
| 50 mOhm | 5.10 mV | 24.9 % | 409.6 mA | 431 uV | 690 | 0.52 mW | 5.1 mV |
| 100 mOhm | 10.2 mV | 49.8 % | 204.8 mA | 863 uV | 1380 | 1.04 mW | 10.2 mV |

Three constraints bound the choice: max measurable current is
`ADCRANGE full scale / R_shunt` (100 mOhm would cap this rail at 204.8 mA and
saturate silently if a camera or USB load spiked it); I^2R self-heating shifts R
and appears as gain error; and the series drop is subtracted from the rail being
powered. A larger shunt also uniquely improves the one floor no software knob
reaches — the 5 uV maximum offset voltage, which is 250 uA of offset error on
20 mOhm versus 50 uA on 100 mOhm — but that offset largely cancels in an
active-minus-idle subtraction, so it does not bind this measurement either.

Conclusion: keep the fitted 20 mOhm. A respin would improve a term that is not
limiting, while the dominant +38 % baseline-definition term would be untouched.
Revisit only for a materially smaller target delta, and only after the rail's
true peak current is characterised — the app reports it as `peak_uv`.

Reproduce with `examples/aen/aen-inference-energy`; see that example's README for
the exact build and flash commands.

## Method

1. **Pick the rail from the board, not from an assumption.** All six EVK INA236
   monitors are probed, and each is watched through a short inferring window and
   a short quiet one. A rail is only eligible if its step clears three standard
   errors; the largest significant step wins. If no rail shows a significant
   step, the app falls back to the rail carrying the most power and says so
   (`rail_selected_by`), because ranking rails by a difference that is all noise
   picks whichever rail's dither was largest — bench-observed: with a trivial
   model it chose a 0.6 mA housekeeping rail over the 93 mA compute rail.

   Rails are ranked in **milliamps**, never in shunt microvolts: this board
   mixes 20 mOhm and 50 mOhm shunts, so microvolts are not comparable between
   rails (172.5 uV is 8.6 mA on 20 mOhm but 3.5 mA on 50 mOhm). The fallback
   ranks by power. From the primary run:

   | Rail | Addr | Active | Idle | Delta | Delta | Bus | Power | Significant |
   |---|---|---|---|---|---|---|---|---|
   | +3V3 | 0x40 | 205.0 uV | 210.0 uV | -5.0 uV | -0.250 mA | 3302 mV | 33.8 mW | no |
   | +1V8 | 0x41 | 0.0 uV | 0.0 uV | 0.0 uV | 0.000 mA | 1792 mV | 0.0 mW | no |
   | +VIO | 0x42 | 37.5 uV | 22.5 uV | +15.0 uV | +0.300 mA | 1792 mV | 1.3 mW | no |
   | +V_CAM0 | 0x4B | absent | — | — | — | — | — | — |
   | +V_CAM1 | 0x49 | 0.0 uV | 0.0 uV | 0.0 uV | 0.000 mA | 0 mV | 0.0 mW | no |
   | **+5V** | **0x4A** | **2042.5 uV** | **1870.0 uV** | **+172.5 uV** | **+8.625 mA** | **4736 mV** | **483.7 mW** | **yes, 10.1 sigma** |

   (+V_CAM0 does not answer at 0x4B on this board: pre-respin units strap it to
   0x48, which collides with the TAS2563 broadcast address and is unreadable
   there. +V_CAM1 reads 0 mV bus — no camera fitted.)

   **The significance figure is 10.1 sigma, and an earlier version of this page
   said 12.** That was not a different measurement; it was an artefact of a
   defect. The spread was accumulated as integer `sum`/`sum_of_squares` with the
   mean truncated before squaring, which inflated sigma by up to `2*|mean|` — on
   a ~2050 uV rail, a true 3.5 uV sigma was reported as 53 uV. Because the
   3-sigma gate is a multiple of that sigma, the gate was also 3-15x too strict
   and would have pushed a genuine load step into the fallback. It now uses
   Welford's algorithm in float. Separately, sigma is floored at one shunt ADC
   count (2.5 uV on the coarse range the scan uses): a rail whose 16 samples are
   bit-identical — +1V8 and +V_CAM1 above — computes a spread of exactly zero,
   and a zero-spread gate declares any one-count offset significant, which is
   vacuous on precisely the quiet rails the gate exists to reject.

2. **Range picked from the observed swing.** With the +5V shunt peaking at
   ~2.05 mV, the app selects the INA236's finer +/-20.48 mV ADC range (625 nV
   per shunt count instead of 2.5 uV) rather than the +/-81.92 mV default. It
   stays on the coarse range whenever the observed peak would risk clipping —
   saturating the range would silently clip the peaks the measurement is about.

3. **Sample every conversion exactly once.** The INA236 is configured for 16
   averaged conversions of 140 us shunt + 140 us bus = 4.48 ms per reported
   sample (223.2 samples/s), and each sample is taken only when the
   conversion-ready flag (CVRF, Mask/Enable bit 3) says a fresh conversion
   completed. Reading Mask/Enable clears CVRF, so the flag is a consume-once
   handshake: blind-rate polling would double-count a slow conversion and miss a
   fast one. Power comes from the INA236's own Power register — the device
   computes P = V x I in hardware, so there is no software multiply.

4. **Integrate, then subtract.** Energy is the trapezoidal integral of
   (timestamp, power) over each window, timestamped with the Cortex-M55 DWT
   cycle counter at 160 MHz. The reported figure is
   `(E_active - E_idle) / n_inferences`, repeated over three window pairs for a
   spread. The device integrates on-target AND emits every raw sample so the
   host re-integrates independently with its own unit-tested trapezoidal
   integrator (`scripts/alp_model/measure.py`); two implementations agreeing is
   evidence, one number is a claim.

5. **Windows must match.** `windowed_delta()` subtracts two integrals, so
   mismatched durations bias the baseline — a short idle window subtracts too
   little and inflates the result. Pairs whose spans differ by more than 10 %
   are discarded rather than averaged in. Measured spans agreed to 0.2 %
   (1100/1101 ms active vs 1102 ms idle).

### What the baseline is

The idle window keeps the CPU spinning with the NPU quiet. It is **not** a
WFI/low-power baseline. This is deliberate: subtracting a spin-idle baseline
removes the CPU's own always-on draw and leaves the inference work, whereas a
WFI baseline would fold "CPU awake at all" into the per-inference figure. The
app reports this as `baseline: "cpu-spin-npu-idle"` so the number cannot be read
as something else.

The choice is worth a lot, and it was measured (see the error budget): at the
board input, spin-idle draws 76 mA and post-run WFI draws 75 mA, so a WFI
baseline would report roughly 38 % more energy per inference for identical work.

## Cross-check: whole-board input power (DPS-150)

The bench has no power analyser, so the only independent instrument is the
DPS-150 programmable supply that is the carrier's sole power source
(Vin = 16.0 V).

**These two instruments do not measure the same thing, and the comparison is
only ever a bound.** The DPS-150 sees the *total board input* at 16.0 V. The
INA236 sees *one individual downstream rail* (+5V), which the carrier generates
from that 16 V input along with every other rail. So the input delta necessarily
contains three things the rail delta does not:

- the +5V rail's own delta divided by the efficiency of the 16 V -> 5 V
  regulator (a step-down converter never passes the increase through for free);
- any concurrent change on **every other** rail (+3V3, +1V8, +VIO, the camera
  rails);
- any change in the regulators' own losses at the higher load.

All three are non-negative, so physics requires
`input_delta >= rail_delta / efficiency > rail_delta`. The input delta is the
**upper** bound; it can never legitimately come out below the single-rail delta.
Keep that direction in mind reading the numbers.

During the long-window run the DPS input power was logged at 5 Hz and split by
the app's own phase markers:

| Phase | Input current | Input power | Distinct current values seen |
|---|---|---|---|
| Active (inferring) | 0.07861 A (sd 0.00049) | 1.2571 W | 0.078, 0.079 |
| Idle (spin baseline) | 0.07600 A (sd 0.00000) | 1.2166 W | 0.076 |
| After the run (WFI) | 0.07500 A (sd 0.00000) | 1.2062 W | 0.075 |

```
whole-board input delta (16.0 V)   40.5 mW  ->  0.015497 mJ/inference
+5V single-rail delta              44.0 mW  ->  0.016832 mJ/inference
ratio rail / input                 1.086
```

**The bound came out inverted, and that is the interesting part.** Per the
direction argued above, the whole-board input delta must EXCEED the single-rail
delta — it carries the rail increase grossed up by regulator efficiency, plus
every other rail. As measured it is 8.6 % SMALLER, which is physically
impossible. So one of the two numbers is being reported outside its accuracy,
and the resolution figures say which:

- The DPS-150 reports current in **1 mA steps = 16.0 mW at 16.0 V.** The entire
  step being measured is **2.6 mA — 2.61 LSB.** Its own quantisation is
  +/-16 mW, i.e. roughly **+/-40 % of the delta it is being asked to resolve.**
  The idle phase read a single quantised value (0.076 A, sd 0.00000) and the
  active phase alternated between just two (0.078/0.079 A) — the instrument is
  visibly at its floor.
- The INA236 side is nowhere near its floor: 44.0 mW on the +5V rail is a
  172.5 uV shunt step resolved at 2.5 uV per count on the coarse range the scan
  uses (69 counts), against a 17.1 uV standard error, and the resulting energy
  repeated to +/-1.2 % across windows.

44.0 mW sits well inside 40.5 +/- 16 mW, so the inversion is fully explained by
DPS quantisation and carries no information about the rail measurement. It also
means the intended inequality **cannot be tested on this bench**: an instrument
whose error bar is 40 % of the quantity cannot bound a number that differs by
9 %. Stating otherwise would be dressing up a null result.

What the cross-check DID establish, which is worth having:

- **Order of magnitude, and no gross error.** A wrong rail, a factor-of-10 or
  625x scaling mistake, or an inverted sign would have shown up as a wild
  mismatch instead of a 9 % one. None did.
- **The step is real and correctly located in time.** The input current stepped
  down exactly at the app's active->idle phase-marker boundary (uptime
  27351 ms), so the two instruments agree about *when* the load was present.
- **The baseline choice, quantified.** Post-run WFI drew 0.07500 A against
  spin-idle's 0.07600 A — a 1 mA (16 mW) gap that is the measurement of the
  baseline-definition term in the error budget, and the only reason that +38 %
  figure is a measurement rather than a guess.

### No precise error bound is published here

There is **no bench power analyser or Joulescope on this bench.** The attached
instruments are two DPS-150 supplies, three FTDI USB-serial bridges and one TI
XDS110 — checked, not assumed. A shunt-level analyser comparison over identical
windows, and therefore a +/-% accuracy figure for this measurement, is
**not available and is not stated.** What is stated below is an error budget
built only from quantities that were actually measured.

## Error budget (measured terms only)

| Term | Size | How it was obtained |
|---|---|---|
| Within-run repeatability | +/-1.2 % | Sample std dev across 3 window pairs (+/-0.000200 of 0.017237 mJ) |
| Quantisation | 1.3 %, NOT limiting | A 3.91x finer POWER LSB (3.906 -> 1.0 mW) moved the result 0.017177 -> 0.016955 mJ, inside the run-to-run spread |
| Window-length sensitivity | 2 % | 250-sample (1.1 s) vs 6000-sample (26.6 s) windows: 0.017177 vs 0.016832 mJ (both on the pre-fix build) |
| Baseline-loop composition | ~7 % | Adding one clock read per iteration of the shared sampling loop raised the idle floor 491.4 -> 496.5 mJ and moved the result 0.018406 -> 0.017177 mJ |
| Baseline **definition** | +38 % | Spin-idle (76 mA) vs WFI (75 mA) at the board input; a WFI baseline would report ~38 % more per inference |
| Whole-board agreement | within 9 %, bound NOT testable | 44.0 mW single-rail vs 40.5 mW total board input — different measurement points; the input figure carries +/-40 % from 1 mA quantisation, so it bounds nothing at this scale |
| Rail-vs-die | +10-20 %, NOT measured | The rail is upstream of the module regulators, so it overstates die energy by 1/efficiency. No efficiency measurement was made; this term is an unquantified upward bias |
| Analyser-verified accuracy | **unavailable** | No power analyser on this bench |

The dominant term is not instrumental. It is the **definitional** choice of
baseline (+38 %), followed by the unmeasured rail-vs-die bias. Quoting this
number to better than about two significant figures is not supportable.

## What this is NOT

These are the caveats that make the figure honest. They are not hedging; each
one blocks a specific wrong reading.

- **NOT NPU energy.** The delta includes the CPU work that drives each
  inference — the TFLM interpreter, the Ethos-U driver, the command-stream
  setup, the interrupt wait — not just the NPU. There is no separate NPU rail
  on this board to isolate.
- **NOT silicon energy.** The measurement is taken at a carrier rail that is
  upstream of the module's own regulators, so it includes their conversion loss
  and overstates what the die consumes by roughly 1/efficiency.
- **NOT total board energy.** The idle baseline is subtracted, which removes the
  static draw of the rest of the board. This is an *incremental* figure.
- **NOT comparable to a vendor datasheet figure.** Vendor energy-per-inference
  and TOPS/W numbers are die-level, on a different baseline, usually on a
  different model. Comparing them to this number compares two different
  quantities.
- **NOT a per-model constant.** It is this model, at this Vela configuration, on
  this silicon, at this clock. 7,077,252 MACs at 383 us per inference.

## What it CAN claim

The incremental energy drawn from the named carrier rail (+5V) per inference of
this model, idle-subtracted against a stated CPU-spin baseline, averaged over
2886 inferences per window and repeated over 3 windows, with a measured spread
of +/-1.2 % and an error budget dominated by a +38 % baseline-definition choice:

**0.017 mJ/inference (+/-0.0002 measured spread), carrier-rail-delta scope.**

The `source: "measured"` / `scope: "carrier-rail-delta"` labels are validated in
code (`scripts/alp_model/measure.py`, `EnergyMeasurement.__post_init__`) and
must never be relabelled downstream — that guard exists so this number cannot be
reported as NPU or silicon energy by a later consumer.

## Bench notes that cost time

- **A J-Link `qc` leaves the core halted.** Every console read
  (`reread.sh`, and the read at the end of the flash helpers) halts the core and
  does not resume it. Reading a long-running app mid-run therefore *freezes it*,
  and the truncated console looks exactly like a crash. Let the app finish, or
  reset with `RSetType 2; r; g` and wait, before reading.
- **`JLinkExe mem8` refuses a read larger than 0x10000** ("NumBytes should be
  <= 0x10000") and returns nothing, so a large `CONFIG_RAM_CONSOLE_BUFFER_SIZE`
  reads back empty. `scripts/bench/aen/bench-env.sh` now chunks the read.
- **`JLinkExe` selects a probe only by serial.** With more than one J-Link
  attached, no selector means every command fails with "Cannot connect to the
  probe/programmer" — which again presents as an empty console. Export
  `JLINK_SN`; the AEN E8 answers SW-DP ID `0x4C013477`.
- **A `-D` on the west command line is a CMake cache variable, not a compiler
  define.** Without explicit forwarding, measurement knobs are accepted silently
  and the build is byte-identical, so the run appears to honour them while
  measuring something else.

# aen-qenc-readout

Measure a **quadrature encoder** on the E1M-AEN801 (M55-HE) through TWO
independent paths, neither bench-verified yet:

1. **Hardware**: the Ensemble **E8 UTIMER** QEC0 channel, through the
   vendored `alif,utimer-qdec` sensor driver, using the standard Zephyr
   **sensor API** (`sensor_sample_fetch` / `sensor_channel_get`) on
   `DT_ALIAS(alp_qenc0)` → `SENSOR_CHAN_ROTATION`. **MEASURED (#2037,
   2026-09-13) to be an unqualified edge counter, NOT a quadrature decoder**
   — see "Decode status" below before trusting any number from this path.
2. **Software**: Zephyr's `gpio-qdec` input driver, a debounced Gray-code
   state machine over the same pads, reached through the SoC's GPIO
   controller instead of the UTIMER. Added this round in direct response to
   (1)'s measured defect; not yet run on hardware.

A portable `<alp/counter.h>` incremental-encoder API (`alp_qenc_open()`)
already exists in alp-sdk and would be equally affected by (1) if anything
wired it to this hardware — see "Portable API" below.

## The block

The E8 UTIMER's QEC0 channel takes two phase inputs (`P3_0`/`P3_1`, muxed as
`QEC0_X_A`/`QEC0_Y_A`) as `QEC_TRIGGER0`/`QEC_TRIGGER1` on its `SRC_0`
trigger-source registers -- **not** "channel input A/B" (that `SRC_1` naming
only applies to the `lputimer0/1/2` instances Alif's own tree binds this same
driver to). **This channel does NOT decode quadrature**: it counts
`QEC_TRIGGER0` rising edges up and `QEC_TRIGGER1` rising edges down with no
qualification of the other input, measured on real hardware (#2037,
2026-09-13) -- see "Decode status" below. `zephyr/drivers/sensor/qdec_alif/
qdec_alif_utimer.c` carries the full register/vendor-source evidence, plus
the `SRC_0`-vs-`SRC_1` fix that made this channel count at all (it read stuck
at zero before). Upstream Zephyr v4.4 and `hal_alif` ship **no** Alif QDEC
driver, so the sensor-class driver is carried in-tree as an **ADR 0017
Tier-2** fork-driver copy with local fixes (it started as a verbatim copy of
the Apache-2.0 `zephyr_alif` fork's `drivers/sensor/qdec_alif/
qdec_alif_utimer.c`). It is built on the `hal_alif` UTIMER register helpers
(`USE_ALIF_HAL_UTIMER`, the same library the Tier-1.5 counter/PWM drivers use) and
binds the `alif,utimer-qdec` child of an `alif,utimer` parent.

A **software** decoder, added this round, reaches the same `P3_0`/`P3_1`
pads through the SoC's GPIO controller instead -- see "Software decoder"
below.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-qenc-readout
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

The board overlay declares a full `utimer@4800d000` parent (UTIMER channel 12 =
the QEC0 quadrature block, per the DFP IRQ table — see the overlay's own header
comment) with the `qdec` child on it, wires `pinctrl_qec0`, and aliases
`alp-qenc0 → &utimer12_qdec`.

## What it shows

1. `DEVICE_DT_GET(DT_ALIAS(alp_qenc0))` → UTIMER QDEC (hardware path);
   `DEVICE_DT_GET(DT_NODELABEL(qdec_sw))` → `gpio-qdec` (software path).
   `device_is_ready` on both -- the software path not being ready is not
   fatal to the run (see "Software decoder" below).
2. Echo the compiled `counts-per-revolution`/filter config straight off the
   hardware qdec DT node — the only "is it configured right" state the
   sensor API exposes.
3. Echo the **pad-control** (mux + electrical) register value for `P3_0` and
   `P3_1` once at start-up — a missing input-enable (bit 16, REN) is visible
   here without a debugger:
   ```
   [qenc] pad-config P3_0@0x1a603060=0x00210005 P3_1@0x1a603064=0x00210005 (REN=bit16)
   ```
4. Prompt the operator to turn the shaft, then poll for 200 samples over
   ~60 seconds — long enough to read the prompt, reach the bench and turn a
   shaft. Every sample reads THREE independent signals: the hardware
   channel's edge count (`sensor_sample_fetch` + `sensor_channel_get
   (SENSOR_CHAN_ROTATION)`, printed as `hw_edges`, explicitly labelled NOT a
   position), the **raw pad levels** of `P3_0`/`P3_1` off the GPIO3
   controller's external-port register (bypassing both decoders entirely),
   and the software decoder's accumulated tick count (`sw_ticks`, updated
   asynchronously by a Zephyr input-subsystem callback):
   ```
   [qenc] hw_edges[42]=0 (mod 96, NOT degrees/position -- #2037)  P3_0=1 P3_1=0  sw_ticks=3  [55 s left]
   ```
5. Report a verdict keyed **only on the software decoder** — the hardware
   edge count is diagnostic-only and can never produce `PASS`, because it is
   measured to be an edge counter, not a decoder (#2037; an earlier version
   of this app treated "pads moved and the hardware angle moved" as `PASS`,
   which the bounce measurement below refutes):
   - `RESULT PASS` — the raw `P3_0`/`P3_1` pad levels changed AND the
     software decoder registered net ticks with them: a debounced
     quadrature decode observed real motion.
   - `RESULT FAIL`, pads changed but the software decoder never ticked —
     either its GPIO3 interrupt path did not fire (see "Software decoder"
     below for the unverified assumption this depends on) or the motion
     never cleared one x4 step. The hardware edge count is NOT evidence
     either way here.
   - `RESULT SKIPPED`, neither the pads nor the software decoder ever
     changed — nothing reached the pads. This does **not** distinguish an
     absent/unfitted encoder from a broken or disconnected one; it only says
     no edges arrived at `P3_0`/`P3_1`.
   - `RESULT FAIL`, any hardware sensor-API read errored — the driver itself
     is failing calls it should not, regardless of the pad/tick correlation
     above.

## Decode status (hardware path) — MEASURED, not a decoder

**2026-09-13, attended, E1M-AEN803 `2026W36-0002`: 120000 unaliased `CNTR`
reads over 25 s of continuous hand motion.** Up `+2385`, down `-1731`, net
`+654`, unwrapped range `-33..+710`, 293 of 734 non-zero steps `|step| >= 2`
inside a single 0.21 ms sample window. For a genuine quadrature pair, this
trigger mapping counts 24 `X`-rising and 24 `Y`-rising edges per revolution
in EITHER direction, so a clean signal must net 0 per revolution at any speed
and stay within ±1 of its start forever — the measured drift is contact
bounce accumulating on an unqualified, undirected edge counter, not motion.

Confirmed against the register map, not just the count: AE822 SVD
`UTIMER_UP_0_SRC`/`UTIMER_DOWN_0_SRC` (`:29960-29967`, `:30254-30261`)
describe bits `[23:0]` only as "Rising/Falling edge of `QEC_TRIGGERn` causes
counter to increment/decrement" — no level qualification of the OTHER
trigger input anywhere in that description. The only level-qualified matrix
in the whole register map ("input A rising AND input B = 0") is `SRC_1`,
measured not to reach these channels at all. Alif's own `demo_qec.c`, lines
306/317/328, drives X, Y and Z as three **independent** GPIOs and describes
counting their edges — an edge-count test, not a quadrature test, and
nothing in the DFP's utimer sources ever mentions "decode" or "quadrature".

**Gap, recorded not resolved**: whether `FILTER_CTRL_A`/`FILTER_CTRL_B`
(`0x84`/`0x88`) affect the `QEC_TRIGGER` inputs on these channels at all is
unproven — if not, the filter this driver programs is inert here and fully
explains the bounce; if so, but asymmetrically, that is a second bias
candidate. See the driver comment.

The `RESULT PASS` this app reported before this round (pads moved, hardware
angle moved) is **withdrawn** as evidence of a working decoder.

## Software decoder

Zephyr's `gpio-qdec` input driver (`zephyr/drivers/input/
input_gpio_qdec.c` upstream) is a debounced Gray-code state machine, bound
in the board overlay to the SAME `P3_0`/`P3_1` pads via `&gpio3` (the SoC's
GPIO controller) instead of the UTIMER — with **no pinctrl change**:
`pinctrl_qec0` keeps muxing the pads to `QEC0_X_A`/`QEC0_Y_A`, not GPIO. It
samples through `gpio_pin_get_dt` → `gpio_dw_port_get_raw` → `EXT_PORTA`
(`0x49003050`) — the SAME register this example's own raw-pad read already
measures live under the QEC0 mux, so reaching the pad through `&gpio3` at
all is bench-proven, not an assumption. `steps-per-period = 4` is a real x4
quadrature decode (unlike the hardware channel's unqualified edge count):
it posts one signed `INPUT_REL_WHEEL` event per mechanical detent on this
24-PPR part, consumed in `main.c` via `INPUT_CALLBACK_DEFINE()` into an
atomic tick accumulator — the first use of Zephyr's input subsystem
anywhere in alp-sdk.

**`idle-poll-time-us` is required, not a tuning knob.** Without it this
driver runs interrupt-driven: `gpio_qdec_irq_setup()` requests
`GPIO_INT_EDGE_BOTH`, and `snps,designware-gpio` (`gpio3`'s compatible)
returns `-ENOTSUP` for that exact combination — a driver-capability
mismatch readable from two in-tree files, not a question only a bench can
answer. `gpio_qdec_irq_setup()` returns `void` and only `LOG_ERR`s the
failure (this example has no `CONFIG_LOG=y`), so nothing prints, the sample
timer that would call `gpio_pin_get_dt()` never starts, `device_is_ready()`
still reads true, and `sw_ticks` stays 0 forever — `RESULT PASS` would be
unreachable. With `idle-poll-time-us` set, `gpio_qdec_irq_setup()` is never
called at all; the driver polls through `gpio_pin_get_dt()` on a `k_timer`
instead, the same read path already proven live above.

## Portable API

alp-sdk already has a portable incremental-encoder surface —
`<alp/counter.h>`'s `alp_qenc_open()` / `alp_qenc_get_position()`, resolving
`cfg->encoder_id` via the same `alp-qenc0` devicetree alias this board
overlay declares (`aliases { alp-qenc0 = &utimer12_qdec; };`), backed by
`src/backends/qenc/zephyr_drv.c`, which binds `_devs[0]` from exactly that
alias. That backend is the SAME `sensor_sample_fetch`/`SENSOR_CHAN_ROTATION`
pair measured broken above, so it would be equally affected on this SoM if
anything called it here — the alias and the backend both already resolve to
the real hardware, it is specifically `alp_qenc_open()` that nothing in this
repo calls against this board (this example binds the raw Zephyr sensor
device directly instead, and the portable-API demo
`examples/peripheral-io/qenc-readout` has no AEN801 overlay of its own).
`zephyr_drv.c`'s own comment already anticipates a "v0.3 input-subsystem
fast-path"; whether to build a `gpio-qdec`-backed variant of it, and have
this example migrate onto `alp_qenc_open()` once it exists, is tracked in
[alplabai/alp-sdk#2095](https://github.com/alplabai/alp-sdk/issues/2095),
not decided here.

## Hardware liveness ratio (diagnostic build only)

`counts-per-revolution` stays **96** in the board overlay — that value is NOT
changed here: there is no reload value that turns the hardware channel's
measured unqualified edge count into a real per-revolution wraparound (see
"Decode status" above), so 96 is kept as the pre-existing baseline rather
than replaced with a new guess. The `QDEC_ALIF_UTIMER_SRC0_X_EDGES`
diagnostic build (both edges of `QEC_TRIGGER0` only, `DOWN_0_SRC` unarmed) is
a **liveness check only** — it answers "does `SRC_0` count at all", which is
already answered yes. Its own prediction, unsigned since the count can only
increase: one detent +2, one full revolution +48, in EITHER shaft direction,
inflated by the same contact bounce measured on the default mapping. It does
NOT predict a position, default or diagnostic build alike — see "Decode
status" above and "Software decoder" for the path that might.

## Status

**Root cause identified AND measured resolved-differently than hoped
(2026-09-13, same day, attended, E1M-AEN803 `2026W36-0002`).** Three runs,
in order:

1. **`RESULT SKIPPED`, pre-fix build.** All 200 samples read `0 deg`,
   200/200 clean reads. Live SWD reads across the window confirmed the
   decoder's own register state is the correct resting state for a
   trigger-counting channel (`UTIMER_CNTR_CTRL` `0x00000021`), and
   `UTIMER_CNTR` itself read `0x00000000` at every point checked, including
   under motion — see `changelog.d/2037.md` for the full register dump. This
   run could not tell a live decode from an unwired shaft.
2. **`RESULT FAIL`, same pre-fix build, raw pad sampling added: pads
   toggled, angle never did.** GPIO3 `EXT_PORTA` (`0x49003050`, bits 0/1)
   showed all four quadrature states across 189 sample lines (128× `11`, 44×
   `10`, 15× `01`, 2× `00`), confirming the encoder is fitted, wired, and
   live. `UTIMER_CNTR` read `0x00000000` at four points spread across the
   motion window and once after — the signal reaches the SoC and the
   channel never counts it. This settled run 1's open question: the encoder
   IS wired and live; the decode path itself was the defect.
3. **Root cause identified from those two runs and vendor evidence**:
   `UTIMER_UP_1_SRC`/`UTIMER_DOWN_1_SRC` ("channel input A/B") are the
   `lputimer0/1/2` input path in Alif's own tree, not the QEC channels'
   (12-15); Alif's own CMSIS driver refuses that source on a QEC channel
   (`ARM_DRIVER_ERROR_PARAMETER` unless `triggerSrc == ARM_UTIMER_SRC_0`),
   and their own QEC reference flow programs `SRC_0` instead.
   `zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c` now does the same
   for `timer_id >= 12`. **A same-day follow-up attended run confirmed the
   fix makes the channel count** — but also confirmed, with 120000 unaliased
   `CNTR` reads, that what it counts is bounce, not quadrature (see "Decode
   status" above). **So "fixed" was the wrong word for what this round
   produced: the stuck-at-zero symptom that opened this issue is resolved,
   and a new, more precise defect (no direction qualification) replaced it.**

Per the 2626-R2 carrier netlist: `ENC0_X` → E2 pad `A10`, `ENC0_Y` → E2 pad
`B10`, and the encoder's push switch → `AG16` (`IO4`) — the part **is**
populated and **is** wired to the QEC0 pads this overlay uses.

One more thing the netlist surfaced (RESOLVED by issue #2065):
`metadata/boards/e1m-evk.yaml` used to call the fitted part
`PEC12R-4222F-S0024`, disagreeing with the netlist's `PEC11R-4215K-S0024` for
the same position `E1`. Both are 24-PPR Bourns encoders (`S0024` = 24
pulses/rev in Bourns's own part numbering, which is why `counts-per-revolution`
above is derived from 24 PPR either way), so the mismatch never affected this
example's PPR math, but the family/suffix disagreement (detent torque, switch
debounce spec) was real. `metadata/boards/e1m-evk.yaml` now agrees with the
netlist: `PEC11R-4215K-S0024`.

**Open, going into the next round**: whether the software decoder's
unverified GPIO3-interrupt-while-muxed-to-QEC0 assumption holds — see
"Software decoder" above. Nobody has turned the shaft against this branch's
build; the next attended run settles it, reporting `RESULT PASS` (the
software decoder works), `RESULT FAIL` citing "the software gpio-qdec
decoder never registered a net tick" (the assumption was wrong, or motion
was too small), or `RESULT SKIPPED` (nothing reached the pads at all — would
itself be surprising given runs 1-2 above). See #2037 and #2095.

Tier-2 retires onto the opt-in fork once a real encoder is decoded.

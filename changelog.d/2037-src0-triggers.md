### Fixed — the UTIMER QEC0 channel was armed on the wrong trigger-source registers; measured to be an edge counter, not a quadrature decoder (#2037)

**Round 1 (2026-09-13, pre-fix build): signal reaches the SoC, the channel
never counts it.** An attended run on E1M-AEN803 serial 2026W36-0002,
maintainer turning the shaft continuously for the whole 60 s
window: GPIO3 `EXT_PORTA` (`0x49003050`, bits 0/1) showed all four quadrature
states across 189 sample lines (128x `11`, 44x `10`, 15x `01`, 2x `00`) -- the
encoder is fitted, wired, and live -- while `UTIMER_CNTR` (`0x4800D0A0`) read
`0x00000000` at four points spread across the motion window and once after.

**Root cause: `qdec_alif_utimer_init()` armed `SRC_1` ("channel input A/B"),
not `SRC_0`.** hal_alif's `alif_utimer_config_qdec_triggers()` (`drivers/
utimer/src/utimer.c`) only ever programs `UTIMER_UP_1_SRC`/`UTIMER_DOWN_1_SRC`.
That is correct for the `lputimer0/1/2` instances Alif's own tree binds this
same qdec driver to, which have a real channel input A/B -- it is not correct
for QEC channels (UTIMER 12-15), which do not. Two independent vendor sources
confirm this:

- Alif's own CMSIS driver **refuses** `SRC_1` on a QEC channel: vendor
  `Alif_CMSIS/Source/Driver_UTIMER.c`, lines 612-618, returns
  `ARM_DRIVER_ERROR_PARAMETER` unless `triggerSrc == ARM_UTIMER_SRC_0` when
  `utimer_mode == QEC_MODE_ENABLE`.
- Alif's own QEC reference flow, `qec0_app()` in vendor
  `Boards/Templates/Baremetal/demo_qec.c`, lines 218-228, programs channel 12
  with `ARM_UTIMER_SRC_0`: `TRIG0_RISING` for up-count, `TRIG1_RISING` for
  down-count, `TRIG2_RISING` for capture.
- The AE822 SVD, peripheral UTIMER, `UTIMER_UP_0_SRC`/`UTIMER_DOWN_0_SRC` bits
  `[23:0]`: "For QEC channels: Rising/Falling edge of `QEC_TRIGGER0..11`
  causes counter to increment" -- `SRC_0` is a QEC-channel input path, and
  reading the SVD's separate `[31:24]` "Reserved, not used" annotations as
  covering the whole register (an earlier reading in this issue's history) was
  the mistake that rejected this fix once already.

`zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c` now branches on
`timer_id`: QEC channels (`>= 12`) write `UTIMER_UP_0_SRC` /
`UTIMER_DOWN_0_SRC` with `TRIG0_RISING` up / `TRIG1_RISING` down, matching
Alif's own demo; channels below 12 keep calling
`alif_utimer_config_qdec_triggers()` (`SRC_1`) unchanged -- that path is
correct for the `lputimer0/1/2` instances, and is not touched by this fix.

**Round 2 (2026-09-13, same day, attended, same board): the SRC_0 arming is
live, and it is NOT a quadrature decode.** 120000 unaliased `CNTR` reads over
25 s of continuous hand motion: up `+2385`, down `-1731`, net `+654`,
unwrapped range `-33..+710`, and 293 of 734 non-zero steps were `|step| >= 2`
inside a single 0.21 ms sample window. `UTIMER_UP_0_SRC`/`UTIMER_DOWN_0_SRC`
bits `[23:0]` are described in the AE822 SVD (`:29960-29967`, `:30254-30261`)
only as "Rising/Falling edge of `QEC_TRIGGERn` causes counter to
increment/decrement" -- no level qualification of the other trigger input
anywhere in that description. The only level-qualified matrix in the whole
register map ("input A rising AND input B = 0") is `SRC_1`, which round 1
measured not to reach these channels at all. Alif's own `demo_qec.c`, lines
306/317/328, drives X, Y and Z as three **independent** GPIOs and describes
counting their edges -- it is an edge-count test, not a quadrature test, and
nothing in the DFP's utimer sources ever mentions "decode" or "quadrature".
For a genuine quadrature pair, this trigger mapping counts 24 X-rising and 24
Y-rising edges per revolution in EITHER direction, so a clean signal must net
0 per revolution at any speed and stay within +-1 of its start forever -- the
measured drift is contact bounce accumulating on an unqualified, undirected
edge counter, not motion. **The round-1 `RESULT PASS` this branch's example
reported (pads moved, hardware "angle" moved) is withdrawn as evidence of a
working decoder**; see below for how the example's verdict changed.

**`counts-per-revolution` (96 in `examples/aen/aen-qenc-readout`'s board
overlay) is left UNCHANGED, deliberately** -- there is no reload value that
turns an unqualified, undirected edge counter into a position, so 96 is kept
as the pre-existing baseline rather than replaced with a new guess. The round
1 predicted raw-count deltas ("+24 one direction, back down the other" for
the default mapping) are **withdrawn**: they assumed a working decode, which
round 2 refutes. The `QDEC_ALIF_UTIMER_SRC0_X_EDGES` diagnostic build's own
prediction (one detent +2, one revolution +48, unsigned since `DOWN_0_SRC` is
left unarmed) still holds AS A LIVENESS CHECK -- it was never a decode claim,
only "does SRC_0 count at all", which round 2 also confirms, inflated by the
same bounce.

**Gap, recorded not resolved**: whether `FILTER_CTRL_A`/`FILTER_CTRL_B`
(`0x84`/`0x88`, "input A"/"input B") affect the `QEC_TRIGGER` inputs at all on
these channels is unproven. If they do not, the `0x00100101` filter this
driver programs is inert on channel 12 and fully explains the bounce (raw
contact bounce hitting an unqualified edge counter); if they do, but
asymmetrically between the two inputs, that is a second bias candidate. Not
chased further this round -- see the driver comment.

**Software decoder added; the decode itself is not yet bench-run, but its
read path is bench-proven.** Zephyr's `gpio-qdec` input driver
(`zephyr/drivers/input/input_gpio_qdec.c`), a debounced Gray-code state
machine, is now bound to the SAME `P3_0`/`P3_1` pads via `&gpio3`
(`examples/aen/aen-qenc-readout`'s board overlay adds a `qdec-sw` node and
enables `gpio3`) instead of the hardware UTIMER channel -- no pinctrl change,
the pads stay muxed to `QEC0_X_A`/`QEC0_Y_A`. **Fixed in review: the node
must set `idle-poll-time-us`, or it never fires at all.** Without it,
`gpio-qdec` runs interrupt-driven and requests `GPIO_INT_EDGE_BOTH` (vendor
`input_gpio_qdec.c`, line 67), which `snps,designware-gpio` (`gpio3`'s
compatible) refuses outright (vendor `gpio_dw.c`, lines 204-208, `-ENOTSUP`,
"Does not support both edges") -- silently: `gpio_qdec_irq_setup()` returns `void`
and only `LOG_ERR`s (this example has no `CONFIG_LOG=y`), `gpio_qdec_init()`
still returns 0, `device_is_ready()` still reads true, and `sw_ticks` would
have stayed 0 forever, making `RESULT PASS` unreachable and any bench slot
a statically predetermined `FAIL`. With `idle-poll-time-us` set, the driver
polls through `gpio_pin_get_dt()` -> `gpio_dw_port_get_raw()` -> `EXT_PORTA`
(`0x49003050`) instead -- the SAME register this example's own raw-pad read
already measures live under the QEC0 mux, so that read path is bench-proven,
not an assumption. What remains unverified is the decode itself, under a
real hand on the shaft. The example's `main.c` consumes the driver's events
via `INPUT_CALLBACK_DEFINE()` -- the first use of Zephyr's input subsystem
anywhere in alp-sdk -- and the verdict is now keyed ONLY on this software
path: `RESULT PASS` requires the raw pads AND the software decoder's tick
count to both CHANGE (not a nonzero final net -- the bench prompt's
detent+CW+CCW sequence nets back toward zero by design); the hardware
UTIMER edge count is printed for reference but can no longer produce `PASS`
on its own.

**A portable `<alp/counter.h>` incremental-encoder surface (`alp_qenc_open()`
et al.) already exists**, backed by `src/backends/qenc/zephyr_drv.c`, which
resolves this same example's own `alp-qenc0` alias and calls the exact same
`sensor_sample_fetch`/`SENSOR_CHAN_ROTATION` pair this issue measures
broken, so it would be equally affected on this SoM if anything called
`alp_qenc_open()` against it -- nothing in this repo currently does (this
example binds the raw Zephyr sensor device directly instead). That
backend's own comment already anticipates a "v0.3 input-subsystem
fast-path"; whether to build a `gpio-qdec`-backed variant of it, and have
this example migrate onto `alp_qenc_open()` once it exists, is a design
decision this branch does not make -- filed as
[alplabai/alp-sdk#2095](https://github.com/alplabai/alp-sdk/issues/2095).

`tests/scripts/test_qdec_alif_utimer_start.py` gained
`QdecUsesSrc0ForQecChannels`, asserting the QEC path writes
`UTIMER_UP_0_SRC` (and that the `SRC_1` path stays reachable for
`timer_id < 12`); its module docstring no longer headlines the withdrawn
"never started" theory.

**What this fragment does NOT claim**: that the software decoder works. It
builds (`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`, zero warnings); nobody has
run it on the bench yet -- the operator left before this path existed. See
`changelog.d/2037.md` for the full history of the withdrawn counter-start
"fix" (#2038) and the filter-register hygiene change, both unaffected by this
fragment, and `changelog.d/2037-raw-pad-sample.md` for the raw-pad-sampling
groundwork this fragment builds on.

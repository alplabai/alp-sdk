# aen-qenc-readout

Read a **quadrature encoder** on the Ensemble **E8 UTIMER** on the E1M-AEN801
(M55-HE), through the vendored `alif,utimer-qdec` sensor driver, using the
standard Zephyr **sensor API** (`sensor_sample_fetch` / `sensor_channel_get`) on
`DT_ALIAS(alp_qenc0)` → `SENSOR_CHAN_ROTATION`.

## The block

The E8 UTIMER can run a timer in quadrature-decoder mode: two phase inputs
(`X` = channel A, `Y` = channel B) advance/retreat the counter, wrapping at
`counts-per-revolution`. Upstream Zephyr v4.4 and `hal_alif` ship **no** Alif QDEC
driver, so the sensor-class driver is vendored **verbatim** from the Apache-2.0
`zephyr_alif` fork (`drivers/sensor/qdec_alif/qdec_alif_utimer.c`) as an **ADR
0017 Tier-2** copy. It is built on the `hal_alif` UTIMER register helpers
(`USE_ALIF_HAL_UTIMER`, the same library the Tier-1.5 counter/PWM drivers use) and
binds the `alif,utimer-qdec` child of an `alif,utimer` parent.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-qenc-readout
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

The board overlay declares a full `utimer@4800d000` parent (UTIMER channel 12 =
the QEC0 quadrature block, per the DFP IRQ table — see the overlay's own header
comment) with the `qdec` child on it, wires `pinctrl_qec0`, and aliases
`alp-qenc0 → &utimer12_qdec`.

## What it shows

1. `DEVICE_DT_GET(DT_ALIAS(alp_qenc0))` → UTIMER QDEC; `device_is_ready`.
2. Echo the compiled `counts-per-revolution`/filter config straight off the DT
   node — the only "is it configured right" state the sensor API exposes.
3. Echo the **pad-control** (mux + electrical) register value for `P3_0` and
   `P3_1` once at start-up — a missing input-enable (bit 16, REN) is visible
   here without a debugger:
   ```
   [qenc] pad-config P3_0@0x1a603060=0x00210005 P3_1@0x1a603064=0x00210005 (REN=bit16)
   ```
4. Prompt the operator to turn the shaft, then poll
   `sensor_sample_fetch` + `sensor_channel_get(SENSOR_CHAN_ROTATION)` 200×
   over ~60 seconds — long enough to read the prompt, reach the bench and
   turn a shaft. Alongside every decoded-angle sample, read the **raw pad
   levels** of `P3_0`/`P3_1` straight off the GPIO3 controller's external-port
   register, bypassing the qdec driver and the sensor API entirely:
   ```
   [qenc] angle[42] = 0 deg (0-359)  P3_0=1 P3_1=0  [55 s left]
   ```
5. Report a **three-way** verdict, now discriminated by whether the raw pad
   levels ever moved as well as whether the decoded angle did — a stuck
   angle alone cannot tell a dead/unfitted/unconnected encoder from one that
   is wired correctly but not counted, because every line on this path
   carries a pull-up and both read identically at rest:
   - `RESULT PASS` — the raw `P3_0`/`P3_1` pad levels changed AND the decoded
     angle changed with them: the decoder works, edges reach the pad and the
     channel counts them.
   - `RESULT FAIL`, pads changed but the angle never did — signal reaches the
     SoC pins and the UTIMER QEC0 channel does not count it. This is the
     **#2037 defect**, reproduced under a hand on the shaft.
   - `RESULT SKIPPED`, neither the pads nor the angle ever changed — nothing
     reached the pads. This does **not** distinguish an absent/unfitted
     encoder from a broken or disconnected one; it only says no edges arrived
     at `P3_0`/`P3_1`.
   - `RESULT FAIL`, the angle changed but the pads never did — spurious
     counts internal to the UTIMER channel, not real quadrature edges (a
     `GLB_CNTR_START` write caused exactly this and was withdrawn, #2038).
   - `RESULT FAIL`, any read errored — the driver itself is failing calls it
     should not, regardless of the pad/angle correlation above.

## Status

**Attended bench run on `e1m-aen-evk-03` (E1M-AEN803), maintainer turning the
shaft continuously for the whole 60 s window: `RESULT SKIPPED`.** All 200
samples read `0 deg`, 200/200 clean reads. Live SWD reads across the window
confirmed the decoder's own register state is the correct resting state for a
trigger-counting channel (`UTIMER_CNTR_CTRL` `0x00000021`), and `UTIMER_CNTR`
itself read `0x00000000` at every point checked, including under motion — see
`changelog.d/2037.md` for the full register dump. This is the run that could
not previously be told apart from an unwired shaft; the raw pad sampling added
above exists to settle that on the next run.

Per the 2626-R2 carrier netlist: `ENC0_X` → E2 pad `A10`, `ENC0_Y` → E2 pad
`B10`, and the encoder's push switch → `AG16` (`IO4`) — the part **is**
populated and **is** wired to the QEC0 pads this overlay uses. So a static
angle reading alone is not proof of an unwired shaft, but it is also not proof
of a wired one: both read identically at rest, which is exactly the gap the
raw `P3_0`/`P3_1` pad read closes.

One more thing the netlist surfaced (RESOLVED by issue #2065):
`metadata/boards/e1m-evk.yaml` used to call the fitted part
`PEC12R-4222F-S0024`, disagreeing with the netlist's `PEC11R-4215K-S0024` for
the same position `E1`. Both are 24-PPR Bourns encoders (`S0024` = 24
pulses/rev in Bourns's own part numbering, which is why `counts-per-revolution`
above is derived from 24 PPR either way), so the mismatch never affected this
example's PPR math, but the family/suffix disagreement (detent torque, switch
debounce spec) was real. `metadata/boards/e1m-evk.yaml` now agrees with the
netlist: `PEC11R-4215K-S0024`.

**Open**: whether the encoder's edges physically reach `P3_0`/`P3_1` at all.
The next attended run with this build reports `RESULT PASS` (decoder works),
`RESULT FAIL` citing "#2037 defect" (signal reaches the pins, the channel
doesn't count it), or `RESULT SKIPPED` citing "nothing reached the pads"
(cannot tell an absent encoder from a broken one) — see #2037.

Tier-2 retires onto the opt-in fork once a real encoder is decoded.

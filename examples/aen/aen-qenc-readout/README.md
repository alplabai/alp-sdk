# aen-qenc-readout

Read a **quadrature encoder** on the Ensemble **E8 UTIMER** on the E1M-AEN801
(M55-HE), through the vendored `alif,utimer-qdec` sensor driver, using the
standard Zephyr **sensor API** (`sensor_sample_fetch` / `sensor_channel_get`) on
`DT_ALIAS(alp_qenc0)` → `SENSOR_CHAN_ROTATION`.

## The block

The E8 UTIMER can run a timer in quadrature-decoder mode: two phase inputs
(`P3_0`/`P3_1`, muxed as `QEC0_X_A`/`QEC0_Y_A`) advance/retreat the counter,
wrapping at `counts-per-revolution`. **Not** "channel input A/B" -- that SRC_1
naming only applies to the `lputimer0/1/2` instances Alif's own tree binds this
same driver to. QEC channels (UTIMER 12-15) arrive on SRC_0 as
`QEC_TRIGGER0/1/2` instead; see `zephyr/drivers/sensor/qdec_alif/
qdec_alif_utimer.c` for the fix and its SVD/DFP citations (#2037). Upstream
Zephyr v4.4 and `hal_alif` ship **no** Alif QDEC
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

## Decode ratio per build (#2037 root-cause fix, not yet bench-confirmed)

`counts-per-revolution` stays **96** in the board overlay — that value is NOT
changed by this fix, because the decode ratio under the new SRC_0 trigger path
is itself unproven; 96 was derived from the OLD (wrong) SRC_1 x4 matrix and is
being kept as the baseline the next bench run compares against, not a new
guess. Two images exist (see the overlay's own comment for the full citation
trail); this table is what a bench run should see under each, for the carrier's
24-PPR encoder:

| Build | One detent (raw counts) | One full revolution (raw counts) |
|---|---|---|
| Default (DFP up/down mapping: `TRIG0_RISING` up, `TRIG1_RISING` down) | not separately predicted | +24 one direction; back down to 0 (−24) the other direction |
| Diagnostic (`QDEC_ALIF_UTIMER_SRC0_X_EDGES`: both edges of `QEC_TRIGGER0` only, `DOWN_0_SRC` unarmed) | +2 | +48, in **either** shaft direction (the count can only increase — there is no armed down-trigger) |

Neither of these matches the shipped `counts-per-revolution=96`, which assumed
x4 decode over both phases. Whichever prediction the bench confirms, `96` will
need revisiting in a follow-up change — deliberately not done here, so this
change stays a decode-ratio fix, not a decode-ratio fix bundled with an
unverified reload-value guess.

The diagnostic build's job is narrower than the default build's: it answers
only "does SRC_0 count at all", independent of whether the up/down trigger
*assignment* (which of `TRIG0`/`TRIG1`/`TRIG2` means up vs. down, and whether
`P3_0`/`P3_1` really are this channel's `QEC_TRIGGER0`/`QEC_TRIGGER1` inputs at
all) is correct. A diagnostic-build run that still reads `CNTR == 0` under a
turning shaft means SRC_0 itself is not the fix; a run that counts (one
direction only, since `DOWN_0_SRC` is unarmed) confirms SRC_0 is live and only
the up/down mapping needs revisiting.

## Status

**Root cause identified (2026-09-13), fix not yet bench-verified.** The
2026-09-13 attended run (below) proved the driver was arming the wrong
trigger-source registers: `UTIMER_UP_1_SRC`/`UTIMER_DOWN_1_SRC` ("channel
input A/B") are the `lputimer0/1/2` input path in Alif's own tree, not the QEC
channels' (12-15). Alif's own CMSIS driver refuses that source on a QEC
channel (`Driver_UTIMER.c:612-618`, `ARM_DRIVER_ERROR_PARAMETER` unless
`triggerSrc == ARM_UTIMER_SRC_0`), and their own QEC reference flow
(`demo_qec.c`) programs `SRC_0` instead. `zephyr/drivers/sensor/qdec_alif/
qdec_alif_utimer.c` now does the same for `timer_id >= 12`. **This has not yet
been run on the bench** — the next attended session is what turns "root cause
identified" into "fixed".

**Attended bench run on `e1m-aen-evk-03` (E1M-AEN803), maintainer turning the
shaft continuously for the whole 60 s window, 2026-09-13, pre-fix build:
`RESULT FAIL` — pads toggled, angle never did.** GPIO3 `EXT_PORTA`
(`0x49003050`, bits 0/1) showed all four quadrature states across 189 sample
lines (128× `11`, 44× `10`, 15× `01`, 2× `00`), confirming the encoder is
fitted, wired, and live. `UTIMER_CNTR` (`0x4800D0A0`) read `0x00000000` at four
points spread across the motion window and once after — the signal reaches the
SoC and the channel never counts it. This superseded an earlier run (also
below) that could not tell a live decode from an unwired shaft.

**Earlier attended bench run, same board, same window length:
`RESULT SKIPPED`.** All 200 samples read `0 deg`, 200/200 clean reads. Live
SWD reads across the window confirmed the decoder's own register state is the
correct resting state for a trigger-counting channel (`UTIMER_CNTR_CTRL`
`0x00000021`), and `UTIMER_CNTR` itself read `0x00000000` at every point
checked, including under motion — see `changelog.d/2037.md` for the full
register dump. This is the run that could not previously be told apart from an
unwired shaft; the raw pad sampling added afterward (and confirmed by the run
above) settled that.

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

**RESOLVED**: whether the encoder's edges physically reach `P3_0`/`P3_1` at
all — yes (2026-09-13 run above). **Open**: whether the SRC_0 trigger-source
fix makes the channel count them. The next attended run, on the default
(DFP-mapping) build, reports `RESULT PASS` (decoder now works — check the
raw count against the "Decode ratio per build" table above) or `RESULT FAIL`
citing "#2037 defect" again (SRC_0 was not the whole fix; see the
diagnostic build and the driver's `STILL OPEN` `CNTR_TYPE` note). See #2037.

Tier-2 retires onto the opt-in fork once a real encoder is decoded.

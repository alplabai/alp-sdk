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
3. Prompt the operator to turn the shaft, then poll
   `sensor_sample_fetch` + `sensor_channel_get(SENSOR_CHAN_ROTATION)` 30× over
   ~9 seconds — long enough to actually reach for the knob.
4. Report a **three-way** verdict instead of a binary one — a static reading
   only means something once you know whether a human was actually asked to
   turn the shaft AND whether the driver's own reads stayed clean while it sat
   idle:
   - `RESULT PASS` — the count **changed** during the poll window (live decode).
   - `RESULT SKIPPED` — the count never changed, but every read was clean (the
     decoder is armed as configured). This is what an idle, WORKING decoder
     looks like — it means "nobody turned the knob," not "it's broken."
   - `RESULT FAIL` — the count never changed AND some read errored, i.e. the
     armed state itself is in doubt.

## Status

**Driver path PROVEN on E8, decoder ARMED (RESULT SKIPPED on the prior idle
run):** device ready; `sensor_sample_fetch` + `sensor_channel_get` return 0
for **20/20** reads; count stayed 0 for that run.

That prior run's README claimed *"no encoder shaft is wired to the QEC0 pads on
this bench"* — that claim was never measured, and the carrier's own netlist
disproves it. Per the 2626-R2 carrier netlist: `ENC0_X` → E2 pad `A10`,
`ENC0_Y` → E2 pad `B10`, and the encoder's push switch → `AG16` (`IO4`) — the
part **is** populated and **is** wired to the QEC0 pads this overlay uses. So a
static reading here is not proof of an unwired shaft; per the app's own verdict
logic it is exactly the `SKIPPED` case (idle, not proven dead) until someone
turns it by hand.

One more thing the netlist surfaced: it calls the fitted part
`PEC11R-4215K-S0024`, while `metadata/boards/e1m-evk.yaml` calls the same
position `PEC12R-4222F-S0024`. Both are 24-PPR Bourns encoders (`S0024` = 24
pulses/rev in Bourns's own part numbering, which is why `counts-per-revolution`
above is derived from 24 PPR either way) — but the family/suffix disagreement
between the as-built netlist and our metadata is unresolved and worth a look
before trusting anything finer than PPR count (e.g. detent torque, switch
debounce spec). Not fixed here — metadata is out of scope for this change.

> **The real open question, now that wiring is confirmed:** if a hand-on-the-knob
> bench run still reports `SKIPPED`/`FAIL` instead of `PASS`, the next suspect
> is software, not hardware — `zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c`
> programs the quadrature edge triggers on `UP_1_SRC`/`DOWN_1_SRC` (SRC_1),
> where Alif's own QEC reference design uses the global `UP_0_SRC`/`DOWN_0_SRC`
> (SRC_0) triggers. That's a live, unproven suspicion — deliberately not changed
> here, so this bench run is the measurement that settles it instead of erasing it.

Tier-2 retires onto the opt-in fork once a real encoder is decoded.

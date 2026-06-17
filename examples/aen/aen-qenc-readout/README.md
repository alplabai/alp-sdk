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

The board overlay adds the `qdec` child to the existing `utimer0` parent (gives it
a `clocks` phandle the driver needs), wires `pinctrl_qec0`, and aliases
`alp-qenc0 → &utimer0_qdec`.

## What it shows

1. `DEVICE_DT_GET(DT_ALIAS(alp_qenc0))` → UTIMER QDEC; `device_is_ready`.
2. Poll `sensor_sample_fetch` + `sensor_channel_get(SENSOR_CHAN_ROTATION)` 20×.
3. Report the count and whether it changed (turn the shaft to see live decode).

`RESULT PASS` requires the count to **change** (the shaft was turned). Clean reads
with a static count are reported **PARTIAL** — driver path proven, but no motion.

## Status

**Driver path PROVEN on E8 (RESULT PARTIAL):** device ready;
`sensor_sample_fetch` + `sensor_channel_get` return 0 for **20/20** reads. Count
stays 0 because no encoder shaft is wired to the QEC0 pads on this bench.

> **BENCH-PENDING (for a live count), two HW items + one pad note — no driver
> bugs:**
> 1. **QEC↔UTIMER routing + encoder wiring:** this overlay pairs **QEC0**
>    (X=P3_0/A, Y=P3_1/B, Z=P3_2/index) with **utimer0** (timer-id 0) as a
>    documented default. The exact `QECn`→UTIMER-instance mapping is a TRM detail,
>    and which pads the encoder connects to is a schematic detail — confirm both
>    ([[project_pending_hw_configs]], [[project_som_pad_map_lives_in_alp_sdk]]).
>    (The QEC0 X/Y input pads now carry `input-enable` — the upstream pad
>    read-enable bit — so the input buffers sense; that part is no longer a gap.)

Tier-2 retires onto the opt-in fork once a real encoder is decoded (task #21).

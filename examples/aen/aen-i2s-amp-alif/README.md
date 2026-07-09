# aen-i2s-amp-alif

Drive a tone out of the Ensemble **E8 audio I2S** (the SoC's **`i2s3`**) on the
E1M-AEN801 (M55-HE) through the vendored DesignWare I2S driver, using the standard
Zephyr **I2S API** (`i2s_configure` / `i2s_write` / `i2s_trigger`) on
`DT_ALIAS(alp_i2s0)`.

## The block

The EVK audio I2S is the SoC's **`i2s3@49017000`** (IRQ 144, clock
`ALIF_I2S3_76M8_CLK`) — per the authoritative SoM pinout
`metadata/e1m_modules/aen/from-alif.tsv`, the board's `I2S0_*` audio signals route
to `I2S3_*_A` on **P9_2/P9_3/P9_4/P9_5** (not `i2s0`, which the first cut wrongly
used). Upstream Zephyr v4.4 + `hal_alif` ship **no** DesignWare I2S driver, so
`snps,designware-i2s` is vendored from the Apache-2.0 fork (`drivers/i2s/i2s_dw.c`)
as an **ADR 0017 Tier-2** copy — FIFO/interrupt-driven (no DMA subsystem needed).

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-i2s-amp-alif
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

The overlay creates `i2s3@49017000`, wires `pinctrl_i2s3` (SDO=P9_3, SCLK=P9_4,
WS=P9_5), gates `ALIF_I2S3_76M8_CLK`, and aliases `alp-i2s0 → &i2s3`.

## Status

**TX path WORKING on E8 (RESULT PASS):** `i2s_configure(TX)` / `i2s_write` ×4 /
`i2s_trigger(START)` / `i2s_trigger(DRAIN)` all return 0 and the FIFO drains **with
the 76.8 MHz audio clock ON** — the controller genuinely generates SCLK/WS/SDO and
clocks the tone out (`i2s3`, P9_3/4/5).

The load-bearing fix (shared with the now-working PDM mics): the **76.8 MHz audio
source (HFOSCx2)** must be enabled at the CGU — the upstream Alif clockctrl only
sets per-peripheral gates and never enables this master source. The CGU 76.8 MHz
enable **and** the `I2Sx_CTRL` bit-clock divider are now handled entirely by the
Tier-1.5 clockctrl west-patch
(`zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch`):
`i2s_configure()` → the `i2s_dw` driver calls `clock_control_on()` /
`clock_control_set_rate()`, and the patched clockctrl enables the master source and
divides it down to SCLK — **this example does no raw register poke**. The driver's
`clock_control` calls still tolerate `-ENOSYS`/`-ENOTSUP` so the same source builds
on SoCs whose clockctrl lacks `.set_rate` (e.g. `native_sim`).

> **For AUDIBLE amplifier output (bench-pending — not driver bugs):**
> 1. **74LVC157 mux → TAS2563:** the I2S3 signal reaches the two TAS2563 amps
>    through a 2:1 mux. `/E` = IO8 → **Alif P7.1** (drivable via GPIO); `S` = IO13 →
>    **CC3501E GPIO13**, over the inter-chip SPI bridge whose v0.1 firmware doesn't
>    implement the GPIO-proxy opcode (same block as the SD card / I2S SELECT). The
>    mux must route to the amps.
> 2. **TAS2563 config:** the amps need their I2C ACTIVE-mode config (done by
>    `examples/peripheral-io/i2c-device-hub`) + a speaker on the output.
> 3. **Exact sample rate:** the bit-clock divider (`CLKCTL_PER_SLV` `I2S3_CTRL`) is
>    now programmed by the Tier-1.5 clockctrl `.set_rate` patch, but the *achieved*
>    SCLK rate has not yet been measured on the bench — verify it against
>    `SAMPLE_RATE_HZ` with a scope before trusting the audio pitch.

See [[project_pending_hw_configs]]. Folding the CGU 76.8 MHz enable + the divider
`.set_rate` into a Tier-1.5 clockctrl patch is the clean follow-up; Tier-2 retires
onto the opt-in fork once the amp chain is acoustically validated.

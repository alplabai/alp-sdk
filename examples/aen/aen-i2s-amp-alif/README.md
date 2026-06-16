# aen-i2s-amp-alif

Drive a tone out of the Ensemble **E8 I2S0** controller on the E1M-AEN801 (M55-HE)
through the vendored DesignWare I2S driver, using the standard Zephyr **I2S API**
(`i2s_configure` / `i2s_write` / `i2s_trigger`) on `DT_ALIAS(alp_i2s0)`.

## The block

I2S0 is the E8's `i2s0@49014000` (IRQ 141, clock `ALIF_I2S0_76M8_CLK`). Upstream
Zephyr v4.4 and `hal_alif` ship **no** DesignWare I2S driver, so the
`snps,designware-i2s` controller driver is vendored **verbatim** from the
Apache-2.0 `zephyr_alif` fork (`drivers/i2s/i2s_dw.c` + `i2s_dw.h`) as an **ADR
0017 Tier-2** copy. It implements the standard Zephyr I2S API and is
**FIFO/interrupt-driven** — the `dma_config` struct fields exist but no `dma_*()`
is ever called, so it runs without bringing up the Alif DMA subsystem.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-i2s-amp-alif
# flash + run per docs/aen-bench-bringup.md (SETOOLS ATOC -> SES RAM-load),
# then read ram_console_buf over SWD.
```

The board overlay (`boards/alp_e1m_aen801_m55_he.overlay`) creates the
`i2s0@49014000` node (the alp-sdk E8 SoC dtsi doesn't carry it), supplies
`pinctrl_i2s0` for the TX pads, gates `ALIF_I2S0_76M8_CLK`, and aliases
`alp-i2s0 → &i2s0`.

## What it shows

1. `DEVICE_DT_GET(DT_ALIAS(alp_i2s0))` → I2S0; `device_is_ready`.
2. `i2s_configure(TX)` for 48 kHz, 16-bit, stereo, 256-frame blocks.
3. Queue four triangle-wave blocks, `i2s_trigger(START)`, then
   `i2s_trigger(DRAIN)` — the controller clocks every block out of the FIFO.

`RESULT PASS` = the device configured and the TX FIFO drained cleanly (the
controller generated SCLK/WS and clocked the whole tone buffer out without an
underrun/error).

## Status

**Driver path PROVEN on E8 (RESULT PASS):** `i2s_configure(TX)` /
`i2s_write` ×4 / `i2s_trigger(START)` / `i2s_trigger(DRAIN)` all return 0 — the
I2S0 controller clocked the tone out.

Two `clock_control` calls in `i2s_dw.c` are patched to tolerate `-ENOSYS`/
`-ENOTSUP` (our upstream Alif clockctrl implements only `.on`/`.off`/`.get_rate`,
no `.configure`/`.set_rate`).

> **For AUDIBLE amp output (BENCH-PENDING), three HW/bridge items remain — none
> are driver bugs:**
> 1. **74LVC157 mux** (`<alp/boards/alp_e1m_evk.h>`): I2S0 reaches the TAS2563
>    amps through a 2:1 mux. `/E` (enable, active-low) = **IO8 → Alif P7.1** (drive
>    low); `S` (select) = **IO13 → CC3501E GPIO13** (low = amps), which is on the
>    CC3501E side and must be driven over the inter-chip SPI bridge
>    (`ALP_CC3501E_CMD_GPIO_WRITE`). This example does not toggle the mux.
> 2. **I2S0 pad route (A vs B):** the SoC offers I2S0 on P1_6/P1_7/P3_0/P3_1 (**A**,
>    used here) or P4_1..P4_4 (**B**). Which the E1M-AEN801 SoM routes is not yet
>    declared in `metadata/e1m_modules/E1M-AEN801.yaml` — confirm against the
>    schematic ([[project_pending_hw_configs]], [[project_som_pad_map_lives_in_alp_sdk]]).
> 3. **Exact sample rate:** the bit-clock divider (`CLKCTL_PER_SLV` `I2S0_CTRL`) is
>    not programmed because the upstream clockctrl has no `.set_rate` — the TX
>    drains, but the achieved SCLK is unverified until the clockctrl gains a real
>    I2S divider `.set_rate`.

Tier-2 retires onto the opt-in fork once the amp chain is acoustically validated
(task #21).

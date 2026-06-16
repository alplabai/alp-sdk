# aen-pdm-mic-alif

Capture PCM audio from the EVK's **PDM microphones** (4× MP34DT05) on the
E1M-AEN801 (Alif Ensemble **E8**, M55-HE), through the Ensemble **LPPDM** block and
the vendored `alif,alif-pdm` DMIC driver. Drives the standard Zephyr **DMIC API**
(`dmic_configure` / `dmic_trigger` / `dmic_read`) on `DT_ALIAS(alp_pdm0)`.

## The block

The E8 has two PDM instances: a high-power `pdm@4902d000` (on the A32/M55-HP side)
and the low-power **`lppdm@43002000`** in the M55-HE local domain. This example
uses **LPPDM** (HE-core local), clocked at **76.8 MHz** via `ALIF_LPPDM_76M8_CLK`.

Upstream Zephyr v4.4 and `hal_alif` ship **no** Alif PDM driver, so the class
driver is vendored **verbatim** from the Apache-2.0 `zephyr_alif` fork
(`drivers/audio/alif_pdm.c`) as an **ADR 0017 Tier-2** copy — pure CMSIS + Zephyr
`clock_control`/`pinctrl`, no `hal_alif` library (like `flash_mram_alif`). It
implements the Zephyr DMIC API, so application code stays portable.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-pdm-mic-alif
# flash + run per docs/aen-bench-bringup.md (SETOOLS ATOC -> SES RAM-load),
# then read ram_console_buf over SWD.
```

The board overlay (`boards/alp_e1m_aen801_m55_he.overlay`) creates the
`lppdm@43002000` node (the alp-sdk E8 SoC dtsi doesn't carry it), supplies
`pinctrl_lppdm` for the C0..C3 clock/data pads (P3_4 / P3_6 / P11_2 / P7_6, from
the upstream `alif-ensemble-pinctrl.h`, matching the fork's E8 group), gates
`ALIF_LPPDM_76M8_CLK`, and aliases `alp-pdm0 → &lppdm`.

## What it shows

1. `DEVICE_DT_GET(DT_ALIAS(alp_pdm0))` → LPPDM; `device_is_ready`.
2. `dmic_configure` for 4 channels, 16 kHz, 16-bit PCM, 100 ms blocks.
3. `dmic_trigger(START)`, then `dmic_read` four blocks, counting non-zero /
   non-constant samples; `dmic_trigger(STOP)`.

`RESULT PASS` requires **varying** PCM (live acoustic energy — tap or speak near
the mics). A clean configure + read that returns only silence/constant data (or
`-EAGAIN`) is reported **PARTIAL**: the driver path is proven, but no audio was
captured — check mic routing / gain rather than the driver.

## Status

**Driver path PROVEN on E8:** the device is ready and `dmic_configure` /
`dmic_trigger(START)` / `dmic_read` all execute (`rc=0` on configure + start).
Reads currently return `-EAGAIN` (no acoustic data) → reported **PARTIAL**.

> **BENCH-UNVERIFIED (acoustic):** confirm the E1M-AEN801 EVK routes its 4×
> MP34DT05 mics to the **LPPDM** C0..C3 pads above (vs the HP `pdm@4902d000`), and
> that the mic supply / `LR` select are correct. The pads are the SoC option, not
> the EVK wiring — see [[project_pending_hw_configs]]. Tier-2 retires onto the
> opt-in fork once acoustically validated (task #21).

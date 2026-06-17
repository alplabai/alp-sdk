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

**Software path COMPLETE and register-verified on E8 (RESULT PARTIAL on acoustic
capture).** The driver + clock + channel config are proven correct over SWD:

| Register (SWD) | Value | Meaning |
|---|---|---|
| `HE_CLK_ENA` (`0x43007010`) bit 8 | `1` | LPPDM 76.8 MHz clock gated **on** |
| `PDM_CONFIG` (`0x43002000`) low byte | `0x0F` | channels 0–3 **enabled** |
| `PDM_CONFIG` bit 16 (`PDM_CLK_MODE`) | `1` | clock mode `STANDARD_VOICE_512` (**not** sleep) |
| `PDM_FIFO_STATUS` (`0x4300200C`) | `0` | FIFO empty → no PCM accumulated |

Three real software bugs were fixed to get here (the first cut was missing all of
these, which is why it was silent):
1. the overlay wired only LPPDM **clock** pads and **no data** pads — now wires
   `CO/C1` clocks + `DO/D1` data on the `_B` route (P3_4..P3_7);
2. the data pads lacked `input-enable` (the upstream pad REN bit) — now set, so the
   input buffers sense;
3. the app never called `pdm_mode()` / per-channel `pdm_channel_config()`, so the
   block sat in `MICROPHONE_SLEEP` — now configures each channel's FIR/IIR/gain
   (from the Alif reference) and selects `STANDARD_VOICE_512` before START.

> **Remaining (acoustic) — a SCHEMATIC fact, not a software bug:** with the clock
> on and the config register-verified, `FIFO=0` means no mic bitstream reaches the
> `DO/D1` pads (P3_5/P3_7). The E1M-AEN801 EVK must route its 4× MP34DT05 mics to
> these LPPDM data pads (vs other `Dx` pads or the HP `pdm@4902d000`), with the mic
> supply on. Confirm against the schematic ([[project_pending_hw_configs]]); only
> then can a non-zero, varying PCM (RESULT PASS) be captured. Tier-2 retires onto
> the opt-in fork once acoustically validated (task #21).

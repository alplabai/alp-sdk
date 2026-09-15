# aen-pdm-mic-alif

Capture PCM audio from the EVK's **PDM microphones** (4× MP34DT05) on the
E1M-AEN801 (Alif Ensemble **E8**, M55-HE), through the Ensemble **HP PDM** block
(`pdm@4902d000`) and the vendored `alif,alif-pdm` DMIC driver. Drives the standard
Zephyr **DMIC API** (`dmic_configure` / `dmic_trigger` / `dmic_read`) on
`DT_ALIAS(alp_pdm0)`.

## The block

The E8 has two PDM instances: the **HP `pdm@4902d000`** (main / EXPMST0 domain) and
the low-power `lppdm@43002000` (M55-HE local). The **E1M-AEN801 routes its mics to
the HP PDM** — per the authoritative SoM pinout
`metadata/e1m_modules/aen/from-alif.tsv`:

| Mic signal | SoC function | Pad | HP-PDM channel |
|---|---|---|---|
| `PDM_C0` | `PDM_C0_C` | P6_1 | clock 0 |
| `PDM_D0` | `PDM_D0_C` | P6_0 | data → ch 0/1 |
| `PDM_C1` | `PDM_C2_B` | P11_4 | clock 2 |
| `PDM_D1` | `PDM_D2_B` | P5_4 | data → ch 4/5 |

So two stereo data lines (D0→ch0/1, D2→ch4/5) = the 4 mics. Upstream Zephyr v4.4 +
`hal_alif` ship no Alif PDM driver, so it's vendored from the Apache-2.0 fork
(`drivers/audio/alif_pdm.c`) as an **ADR 0017 Tier-2** copy.

## Rates

Two `SAMPLE_RATE_HZ` builds are supported, both keyed in
`zephyr/drivers/audio/alif_pdm.c`'s `pdm_clock_modes` table:

| `SAMPLE_RATE_HZ` | PDM mode | Status |
|---|---|---|
| `8000` (build default is `16000`; override with `-DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=8000"`) | `PDM_MODE_STANDARD_VOICE_512_CLK_FRQ` (512 kHz clk, decim 64) | **BENCH-PROVEN** on `e1m-aen-evk-03` |
| `16000` (default) | `PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ` (1024 kHz clk, decim 64 — same ratio) | vendor-sourced FIR reuse, **not yet itself bench-verified** |

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-pdm-mic-alif -- -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=8000"
```

## Status

**WORKING on E8 — RESULT PASS: live PCM captured from the mics at
`SAMPLE_RATE_HZ=8000`.** The 6300/6400-nonzero-of-12800-byte-block reading
below is the round-1 bench run, on the driver as it stood before round 2 added
the `measured_rate_hz` check (its own read log is still accurate; only the
"RESULT" line's rate qualifier and the mode-1-is-8kHz correction below are
new):

```
[pdm] read[0] size=12800 nonzero=6288 first=0
[pdm] read[1] size=12800 nonzero=6400 first=5
...
[pdm] RESULT PASS: varying PCM captured = live audio
```

Issue #2133 round 1 shipped `PDM_MODE_STANDARD_VOICE_512_CLK_FRQ` mis-keyed to
16 kHz; it is actually **8 kHz** (HWRM Table 15-118: 512 kHz clock / decimation
64), confirmed by the round-1 bench's own 1600-frame-per-~200ms cadence (a
100ms-block cadence would mean 16 kHz; ~200ms means 8 kHz). Round 2 added a
`measured_rate_hz` print + a ±5% pass gate so a future rate mislabel fails
loudly instead of only "PCM varies" passing regardless of rate — its own
printed reading is pending the next bench run. The 16 kHz build
(`PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ`) is not yet bench-verified at all — see
the Rates table above.

Getting here required finding a chain of real issues (the first cut had all of
them):
1. **Wrong PDM instance** — the mics are on the **HP `pdm@4902d000`**, not the
   LPPDM (per `from-alif.tsv`); with the wrong instance/pads nothing samples.
2. **Wrong/missing pads** — now the SoM-TSV mic route (D0=P6_0/C0=P6_1,
   D2=P5_4/C2=P11_4); data pads carry `input-enable` (pad REN).
3. **`MICROPHONE_SLEEP`** — as of issue #2133 the driver itself primes every
   enabled channel's FIR/IIR/gain (from the Alif reference) and selects the
   real clock mode as part of `dmic_trigger(DMIC_TRIGGER_START)` (round 2:
   moved out of `dmic_configure()` so the block doesn't start sampling, and
   `DMIC_TRIGGER_STOP` doesn't leave it sampling, outside the app's
   start/stop calls); the app supplies only the standard
   `dmic_build_channel_map()` channel map (not a raw PDM bitmask) and calls
   no Alif-specific setup function. `pdm_mode()` / `pdm_channel_config()`
   stay available for an app that wants to override the driver's defaults.
4. **EXPMST0 IP clock not forced** — set `EXPMST0_CTRL` (`0x4902F000`) bits 30/31
   (PCLK/IPCLK force), not just the bit-8 gate.
5. **The 76.8 MHz audio source was OFF** — the load-bearing fix. The upstream Alif
   clockctrl only sets per-peripheral gates; it never enables the **HFOSCx2**
   master source. Enabling it is a single CGU write: `CGU_CLK_ENA` (`0x1A602014`,
   = CGU base `0x1A602000` + `0x14`) **bit 24** (`CLK76P8M`) — reg + bit from the
   fork `clock_control_alif_ensemble.c` GEN2 path, not invented. (So it is *not*
   SE-only as first feared; a direct register write suffices.) SWD before the
   fix: `CGU_CLK_ENA=0xFE33FFF1` (bit24=0, source off); after: the FIFO fills.

The example pokes the CGU 76.8 MHz enable + the EXPMST0 force directly (with
grounded reg/bit references) because the upstream clockctrl driver does neither;
folding both into a Tier-1.5 clockctrl patch is the clean follow-up.
[[project_pending_hw_configs]]

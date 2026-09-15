# aen-pdm-mic-alif

Capture PCM audio from the EVK's **PDM microphones** (4× MP34DT05TR-A,
`E1M-EVK-2626-R2_components.csv`) on the E1M-AEN801 (Alif Ensemble **E8**,
M55-HE), through the Ensemble **HP PDM** block (`pdm@4902d000`) and the
vendored `alif,alif-pdm` DMIC driver. Drives the standard Zephyr **DMIC API**
(`dmic_configure` / `dmic_trigger` / `dmic_read`) on `DT_ALIAS(alp_pdm0)`.

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

`zephyr/drivers/audio/alif_pdm.c`'s `pdm_clock_modes` table has four entries,
but only two are **in spec for the fitted mics** on this board. The
MP34DT05TR-A's PDM clock spec, per ST's own in-tree Zephyr driver
(`drivers/audio/mpxxdtyy.h`: `MPXXDTYY_MIN_PDM_FREQ 1200000`,
`MPXXDTYY_MAX_PDM_FREQ 3250000`), is **1.2-3.25 MHz** — the board overlay
declares that range (`clk-frequency-min`/`clk-frequency-max`), and
`dmic_alif_pdm_configure()` now rejects anything outside it with `-EINVAL`:

| `SAMPLE_RATE_HZ` | PDM mode | On THIS board | FIR-reuse basis |
|---|---|---|---|
| `48000` (**default**) | `PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ` (3072 kHz clk, decim 64) | **in spec, mode confirmed programmed** (`PDM_CONFIG_REGISTER=0x00070033` held throughout capture) -- but the app's measured rate on that silicon run was **~32 kHz, not 48 kHz**; round 4c traces that to the app's OWN (now-fixed) read-loop pacing rather than the PDM clock, see Status | same decimation ratio as the register-proven mode 1 -- direct |
| `32000` | `PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ` (1536 kHz clk, decim 48) | in spec, not yet bench-run | different decimation ratio (48 vs 64) -- less direct |
| `16000` | `PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ` (1024 kHz clk, decim 64) | **REJECTED on silicon** -- `dmic_configure -> -22`, register left untouched (confirmed on `e1m-aen-evk-03`) | n/a on this board |
| `8000` | `PDM_MODE_STANDARD_VOICE_512_CLK_FRQ` (512 kHz clk, decim 64) | **REJECTED** -- below the 1.2 MHz minimum | n/a on this board |

**Do not read the 48 kHz row as "48 kHz works"** -- the mode select is right,
the measured PCM rate on silicon is not; see Status.

```sh
# default (48 kHz, in spec):
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-pdm-mic-alif
# prove the out-of-spec guard (expect RESULT FAIL: configure rc=-22):
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-pdm-mic-alif -- -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=16000"
```

## Status

**Round-1's PASS is DOWNGRADED, not confirmed.** The 6300/6400-nonzero-of-
12800-byte-block reading below was captured at 8 kHz (`PDM_MODE_STANDARD_
VOICE_512_CLK_FRQ`, 512 kHz clk) -- **below the fitted MP34DT05TR-A mics'
1.2 MHz minimum**. An under-clocked PDM mic can output non-constant, non-zero
noise; round 1's gate ("samples aren't all equal") cannot distinguish that
from real acoustic energy:

```
[pdm] read[0] size=12800 nonzero=6288 first=0
[pdm] read[1] size=12800 nonzero=6400 first=5
...
[pdm] RESULT PASS: varying PCM captured = live audio
```

Round 3 changes what this example proves two ways: (1) it now defaults to
**48 kHz** (`PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ`), the only table
entry both in spec for these mics and sharing the register-proven mode's
decimation ratio; (2) it adds a per-channel **RMS / peak-to-peak / DC-offset**
print and requires at least one channel to clear a documented floor (a full
order of magnitude above a dead channel's "±1-2 LSB flat noise") for `PASS` --
a clean read at the right rate with no channel over that floor now reports
`INCONCLUSIVE (no acoustic signal)` instead.

**Round 4a silicon results on `e1m-aen-evk-03`:**
- The 16 kHz build's guard is CONFIRMED: `dmic_configure -> -22`
  (`-EINVAL`), and `PDM_CONFIG_REGISTER` was never written.
- The 48 kHz build's mode select is CONFIRMED CORRECT and held:
  `PDM_CONFIG_REGISTER = 0x00070033` throughout capture, and blocks arrived
  at the configured 38400 B (4800 frames x 4 ch x 2 B). But the app's own
  `measured_rate_hz` read **~32 kHz in two runs**, not 48 kHz. Per-channel
  `rms_ac` 6-7, `peak_to_peak` 1011-1126 (sparse spikes, not a clean tone).

**Round 4c, restated conservatively:** the ~32 kHz reading is this app's OWN
rate measurement -- frames delivered per elapsed wall-clock time in ITS read
loop -- so it measures how fast the loop pulled blocks, not the PDM sample
clock. Round 4a shipped this example with a double-precision per-sample
stats loop (soft-float, ~150-180 ms/block against a 100 ms block period,
no `CONFIG_FPU`); a 1 ms timing model of that loop reproduces ~32-36 kHz
readings on its own. Whether the driver's 4-block slab ALSO exhausted on
that specific run was not separately measured at the time -- the driver had
no way to report a drop until this round added one (`dmic_read()` now
returns `-EIO` on any dropped burst, including a genuine hardware FIFO
overflow). This example's stats loop is now integer-only so it cannot
reproduce that pacing artifact. Do not treat 48 kHz as verified -- only the
clock-mode selection and the 16 kHz rejection are proven on silicon so far;
a fresh bench run with both fixes in place is the next step, not done in
this round.

Getting here required finding a chain of real issues (the first cut had all of
them):
1. **Wrong PDM instance** — the mics are on the **HP `pdm@4902d000`**, not the
   LPPDM (per `from-alif.tsv`); with the wrong instance/pads nothing samples.
2. **Wrong/missing pads** — now the SoM-TSV mic route (D0=P6_0/C0=P6_1,
   D2=P5_4/C2=P11_4); data pads carry `input-enable` (pad REN).
3. **`MICROPHONE_SLEEP`** — as of issue #2133 the driver itself primes every
   enabled channel's FIR/IIR/gain (from the Alif reference) as part of
   `dmic_configure()`, and selects the real clock mode as part of
   `dmic_trigger(DMIC_TRIGGER_START)` (round 2: the clock-mode write moved
   out of `dmic_configure()` so the block doesn't start sampling, and
   `DMIC_TRIGGER_STOP` doesn't leave it sampling, outside the app's
   start/stop calls); the app supplies only the standard
   `dmic_build_channel_map()` channel map (not a raw PDM bitmask) and calls
   no Alif-specific setup function. `pdm_mode()` / `pdm_channel_config()`
   stay available for an app that wants to override the driver's defaults.
   Round 3: the driver also rejects a mode outside the board overlay's
   declared mic clock range -- see Rates above.
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

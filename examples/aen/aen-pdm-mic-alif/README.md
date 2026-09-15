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
| `48000` (**default**) | `PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ` (3072 kHz clk, decim 64) | **CONFIRMED on silicon** (commit `68a169977`, `e1m-aen-evk-03`): `measured_rate_hz=48000` exactly, `slab_missed=0`, `overrun=0`, no `-EIO` for the full run | same decimation ratio as the register-proven mode 1 -- direct |
| `32000` | `PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ` (1536 kHz clk, decim 48) | in spec, not yet bench-run | different decimation ratio (48 vs 64) -- less direct |
| `16000` | `PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ` (1024 kHz clk, decim 64) | **REJECTED on silicon** -- `dmic_configure -> -22`, register left untouched (confirmed on `e1m-aen-evk-03`) | n/a on this board |
| `8000` | `PDM_MODE_STANDARD_VOICE_512_CLK_FRQ` (512 kHz clk, decim 64) | **REJECTED** -- below the 1.2 MHz minimum | n/a on this board |

**48 kHz rate is now bench-confirmed with no drops.** Acoustic capture at
this rate, on mic ch0/ch1 (PDM controller 0) only, is now confirmed too, by
a speaker-to-mic loopback on silicon (issue #2133 round 4f, `e1m-aen-evk-03`,
2026-09-15 -- see Status). The D2 pair (HW 4/5) is register-level verified
only, never acoustically tested. Full-scale headroom at the provisional
gain default is unmeasured; see issue #2143.

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
print and requires at least one channel to clear a documented floor -- a full
order of magnitude above a dead channel's "±1-2 LSB flat noise" AT THE GAIN
THAT FLOOR WAS MEASURED AT (`0x0D`, issue #2143). This is an above-idle-noise
check, not a claim of identified acoustic content by itself; see Status below
for the actual acoustic verification -- a clean read at the right rate with
no channel over that floor reports `INCONCLUSIVE` instead of `PASS`.

**Signal-level gate: AC RMS, not peak-to-peak (issue #2133).** Peak-to-peak
is transient-prone: a probe loopback silence window (`PROBE_LOOPBACK` mode
of `examples/aen/aen-i2s-tas2563-probe` on branch
`test/u46-i2s-tas2563-on-reworked-mux`, commit `56631094d`, issue #2143,
`e1m-aen-evk-03`, 2026-09-15 14:49Z) measured peak-to-peak 545 on one
channel with the room silent -- above the prior 512 floor, from a single
sample excursion, not sustained signal. AC RMS averages over the whole
capture and does not share that failure mode; peak-to-peak is still printed
per channel but no longer gated.

The floor is derived directly from this example's OWN idle measurement
(`e1m-aen-evk-03`, 2026-09-15 19:51Z, 48 kHz, `channel-gain` `0x200`, raw
`dmic_read()`, 3 runs / 12 channel readings over blocks 1-3,
n=14400 samples/channel): `rms_ac` read 15 on every channel in every run
(one earlier interrupted attempt read 18-19). Floor = 4 x 15 = 60 at
`0x200` (`examples/aen/aen-pdm-mic-alif/src/main.c`'s
`MIN_SIGNAL_RMS_AC_LSB`), scaled proportionally with `channel-gain`.

**All idle runs behind this derivation read `INCONCLUSIVE`** -- ambient
sound during those runs is unknown, and this example still has no
controlled stimulus of its own. A `PASS` at a real sound level is a
**PREDICTION**, not yet measured on this example: the same probe loopback,
via `alp_audio_in_read()` (`dc_block_s16()`'s ~38 Hz high-pass, 960 ms
windows, mic ch0/ch1 only), measured peak/rms/dc of 128/98/-97 and
128/111/-110 (ch0/ch1) in silence, 266/120/-99 and 292/120/-98 at 1 kHz/
volume 16, and 651/220/-101 and 652/224/-101 at 1 kHz/volume 48. That
probe RMS includes DC; AC RMS ~= sqrt(rms^2 - dc^2), ESTIMATED (a
different capture path, not this example) at roughly 20 in silence, 68 at
volume 16, and 195 at volume 48 -- all comfortably above the 60 floor.
Predicted stimulus: a steady tone or continuous speech at approximately
volume 16 or louder held near U19/U20 while resetting is expected to
clear the floor and report `PASS` on this example -- **not yet confirmed
by a run of this example itself.**

The D2 pair (HW 4/5) idle-matched ch0/1 in the idle measurement above, but
has never itself been acoustically tested -- the loopback above drives
only ch0/ch1. The any-channel `PASS` rule therefore still includes a pair
this floor has not been acoustically proven against.

**Round 4a silicon results on `e1m-aen-evk-03`:**
- The 16 kHz build's guard is CONFIRMED: `dmic_configure -> -22`
  (`-EINVAL`), and `PDM_CONFIG_REGISTER` was never written.
- The 48 kHz build's mode select is CONFIRMED CORRECT and held:
  `PDM_CONFIG_REGISTER = 0x00070033` throughout capture, and blocks arrived
  at the configured 38400 B (4800 frames x 4 ch x 2 B). But the app's own
  `measured_rate_hz` read **~32 kHz in two runs**, not 48 kHz. Per-channel
  `rms_ac` 6-7, `peak_to_peak` 1011-1126 (sparse spikes, not a clean tone).

**Round 4c** traced the ~32 kHz reading to this app's OWN rate measurement --
frames delivered per elapsed wall-clock time in ITS read loop, paced by a
double-precision per-sample stats loop (soft-float, ~150-180 ms/block
against a 100 ms block period, no `CONFIG_FPU`) -- not the PDM sample clock.
The stats loop is now integer-only.

**Round 4d silicon results on `e1m-aen-evk-03`** (commit `68a169977`, fixed
consumer): `measured_rate_hz=48000` exactly, `slab_missed=0`, `overrun=0`,
no `-EIO` for the full run -- **48 kHz is now confirmed with no drops.** A
separate 30 s capture (same driver, patched read loop) was believed at the
time to be an attended clap test; **round 4e correction: nobody actually
clapped during that capture**, so the bursts it recorded (peak p2p ch0=34
ch1=46 ch2=48 ch3=48 at t=14000 ms; quiet windows 2-3 LSB) are unidentified
room sound or interference, not proof of acoustic liveness on their own. A
mid-capture register readback during that same session found
`CTL0=0x00070033`, `CH0 GAIN=0x0000000D`, `PHASE=0x0000001F`,
`FIR[0]=0x00000001`. `PDM_CH_GAIN` is bits [11:0], unsigned 8.4 fixed-point
(Alif SVD `AE822FA0E5597BS0_CM55_HP_View.svd`, `PDM_CH_GAIN` register), so
`0x0D` = 0.8125x gain (-1.8 dB vs unity), 31.9 dB below the provisional
`0x200`. Alif's `alif_kws`
sample (`AudioBackend.cpp`, `0xF00`) is NOT a precedent for this mode: its
shipped model runs at 16 kHz (mode 4), not the 48 kHz mode 7 this driver
defaults to. The 0x0D value itself is Alif's register-level driver test
value, not audio-tuned. **Round 4e silicon at gain `0x800`** (quiet room,
no controlled stimulus) found every unclipped sample a multiple of `0x80`
(the gain multiply is saturating and runs after the datapath quantizes) and
the start-of-capture / post-restart window pinned at full scale (decimator
settling, not signal) -- `0x800` was too high. Fixed by issue #2143 with a
new, DT-configurable `channel-gain` property, provisional default `0x200`.
Full-scale headroom at `0x200` is UNMEASURED -- it is NOT "clearly below
the clip point" as an earlier round claimed, nor is it a documented
"voice-tuned" value: `0x140` is BLE Audio's Kconfig
DEFAULT (20, `ALIF_BLE_AUDIO_PDM_MICROPHONE_GAIN`), `0x320` is that same
subsystem's `#ifndef` FALLBACK (50, `audio_source_pdm.c`), and
`unicast_initiator`'s `prj.conf` ships 100 (`0x640`) -- three different
values from the same codebase, none documented as tuned for anything in
particular. `0x200` is provisional pending a calibrated full-scale
measurement.

**Round 4f: acoustic capture on mic ch0/ch1 (PDM controller 0) is now
VERIFIED by a speaker-to-mic loopback** on `e1m-aen-evk-03`, 2026-09-15
14:49Z -- the `PROBE_LOOPBACK` mode of `examples/aen/aen-i2s-tas2563-probe`
on branch `test/u46-i2s-tas2563-on-reworked-mux` (commit `56631094d`, issue
#2143), **not this example**, which has no controlled stimulus of its own:
the EVK's TAS2563 speakers (independently verified audible) played known
tones while the PDM mics captured, gain `0x200` confirmed by readback.
Goertzel-bin analysis (2 s windows, first 200 ms discarded) found the 1 kHz
bin at 9.3/0.1 dB (ch0/ch1) in silence rising to 43.4/48.8 dB at volume 16
and 57.8/57.6 dB at volume 48 -- and a 500 Hz bin that only lit up (54.7/56.6
dB) during the 500 Hz stimulus window, staying within -9.8..+5.3 dB
everywhere else. Volume 16->48 raised the 1 kHz bin by +11.6 dB average
(expected +9.5 dB). Peak-to-peak (the `p2p` field of the probe's own
`loop_chan_result_t`; per channel, 960 ms window, after `zephyr_drv.c`'s
`dc_block_s16()` high-pass, hence not a multiple of 32) went from 128/128 in
silence to 266/292 at 1 kHz/volume 16 and 651/652 at 1 kHz/volume 48. This
mics-capture-real-frequency-correct-sound result grounds the honest
wording this example's own `PASS`/`INCONCLUSIVE` text now uses (see Status
above). **The D2 pair (HW 4/5) is register-level verified only, never
acoustically tested.** **Still NOT verified:** full-scale headroom at
`0x200` -- unmeasured; the loopback used a fixed speaker level, not a
calibration sweep.

The first block after every `DMIC_TRIGGER_START` (including a restart
after an overrun) may contain a decimator-settling transient and should be
discarded -- this example already excludes the anchor (first) block from
its signal stats for that reason. How LONG that transient lasts was never
directly measured until an early raw hex dump, taken from the same
unattended 30 s bench capture build later found not to be a genuine clap
test (image since deleted), showed roughly 7 zero samples followed by
~25 samples of ring-down at 48 kHz -- under 1 ms total, well inside one
block. Recommend discarding at least 1 ms of samples, or the first block,
whichever is longer -- a smaller-block consumer than this example's 100 ms
blocks could otherwise still see settling in its second block.

Getting here required finding a chain of real issues (the first cut had all of
them):
1. **Wrong PDM instance** — the mics are on the **HP `pdm@4902d000`**, not the
   LPPDM (per `from-alif.tsv`); with the wrong instance/pads nothing samples.
2. **Wrong/missing pads** — now the SoM-TSV mic route (D0=P6_0/C0=P6_1,
   D2=P5_4/C2=P11_4); data pads carry `input-enable` (pad REN).
3. **`MICROPHONE_SLEEP`** — as of issue #2133 the driver itself primes every
   enabled channel's FIR/IIR (from the Alif reference) and gain (from
   `channel-gain`, issue #2143) as part of
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

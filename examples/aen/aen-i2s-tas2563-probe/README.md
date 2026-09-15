# aen-i2s-tas2563-probe

Bench proof that the I2S0 path through the **reworked U46 mux** reaches both
TAS2563 smart amps on the E1M-AEN801 (Alif Ensemble E8, M55-HE) -- TWO
independent ways: a TAS2563 register flag (no ears needed) and an acoustic
loopback through the EVK's own on-board PDM mics (no bench operator needed
either).

> **BENCH-TEST branch `test/u46-i2s-tas2563-on-reworked-mux`, one physical
> board only: `e1m-aen-evk-03`.** The maintainer has replaced U46 (the I2S0
> mux) plus U38/U39 (the SD mux) with 74LV3257 bus switches -- bidirectional,
> true high-impedance when `/E` is deasserted -- in place of the stock
> 74LVC157, which had no Hi-Z state and drove the SoC's own I2S3 TX pads
> backwards (#2077). That is why I2S playback stays disabled on every other
> E1M-EVK 2626-R2 (`examples/aen/aen-i2s-amp-alif`,
> `examples/aen/aen-evk-demo`'s phase 11) -- this app's board overlay
> re-enables `i2s3` for real, ONLY because this one board no longer has the
> forced-low/direction-conflict premise. Do not read a passing run here as
> "I2S is safe to re-enable in general". This is a SEPARATE test from
> `test/2051-sdhc-enable-on-reworked-mux` (the SD card path through the same
> rework) -- neither branch's result stands in for the other.

## What it does, in order

1. Brings up the SoM's CC3501E bridge (`cc3501e_bridge_bringup()`), then
   drives the I2S0 mux **SELECT** (E1M IO13, 0 = TAS2563 amps) and only
   **THEN** **ENABLE** (E1M IO8, active low) over the bridge's GPIO proxy --
   SELECT first so the switch is never found closed onto the M.2 E-key side.
2. Releases AMP_ENABLE (SD_N, P5_2, a real hardware reset pulse) and brings
   both TAS2563s (0x4D / U27 / LEFT, 0x4E / U28 / RIGHT) up over I2C.
3. Sets the quietest analog gain (`TAS2563_AMP_LEVEL_MIN`, 8.5 dBV) on both
   amps, still in software shutdown, then tells both what the host I2S bus
   will do (`tas2563_configure_i2s()`).
4. Opens the EVK's on-board PDM mics (U19 LEFT / U20 RIGHT, no mux in that
   path -- see `src/main.c`'s file header for the netlist trace) and captures
   an acoustic **BASELINE** window (amps still SHUTDOWN).
5. Starts I2S3 TX at the quietest digital-volume step *before* either amp is
   ever told ACTIVE, then steps the digital volume through 4/16/48 out of
   255, capturing one acoustic window (Goertzel tone-bin + two off-tone
   reference bins + RMS, both mic channels) per step.
6. **Objective check 1, no ears needed:** clears + reads
   `TAS2563_FAULT_TDM_CLOCK` (the amp's own "TDM clock error" latch,
   `INT_LTCH0` bit 2) DURING the loudest step (tone genuinely playing,
   expected CLEAR) and again STOPPED (I2S3 drained, amps still ACTIVE,
   expected SET) -- a DURING-clear/STOPPED-set pair is what proves the flag
   tracks this run's own clock.
7. Captures acoustic **STOPPED** (I2S3 halted, amps still ACTIVE) and
   **SHUTDOWN** (both amps muted) windows, then mutes both amps before
   anything else tears down.
8. Prints the acoustic measurement table (all six windows) plus an
   **ACOUSTIC VERDICT** (`TONE HEARD` / `TONE NOT HEARD` / `INCONCLUSIVE`)
   and a **TDM_CLOCK VERDICT** (`bridge/mux-enable failed`,
   `AMP control GPIO failed`, `TAS2563 I2C not responding`,
   `I2S clocks not reaching the amp`, `clocks reach the amp`, or
   `inconclusive`) -- these are two INDEPENDENT lines of evidence; neither
   is allowed to paper over the other.

## Safety

Both TAS2563s can drive ~10 W peak into 4 ohm (SLASET3D Table 7-105). This
app never exceeds `TAS2563_AMP_LEVEL_MIN` on the analog side, CONFIRMED by a
register readback before either amp is ever told ACTIVE, and a
compile-time-capped digital volume (`SOUND_VOL_MAX` = 48/255, ~18.8% of
digital full scale, enforced by `BUILD_ASSERT`) on the I2S side. See
`src/main.c`'s file header for the full reasoning, including why 48/255 is
still speaker-safe layered on top of the analog floor.

## Does I2S3 or the PDM mic use DMA?

No, neither. `snps,designware-i2s` (`zephyr/drivers/i2s/i2s_dw.c`) and
`alif,alif-pdm` (`zephyr/drivers/audio/alif_pdm.c`) are both
FIFO/interrupt-driven -- the CPU copies each block into/out of the FIFO
register itself, with no external DMA bus master involved. The SD test on
this same board hit a real bug from that class (an ADMA descriptor table
placed in CPU-local DTCM, unreachable by the SDHC controller's own bus
master); it does not apply here, since nothing but the CPU ever touches this
app's audio buffers.

## Build

```
env ZEPHYR_SDK_INSTALL_DIR=<sdk> ZEPHYR_BASE=<zephyr> west build -p always \
    -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    -d build/aen-i2s-tas2563-probe examples/aen/aen-i2s-tas2563-probe -- \
    -DEXTRA_ZEPHYR_MODULES=<this-worktree-absolute-path>
```

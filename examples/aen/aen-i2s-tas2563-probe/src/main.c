/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i2s-tas2563-probe -- proves the I2S0 path through the REWORKED U46 mux
 * reaches both TAS2563 amps on the E1M-AEN801 (Alif Ensemble E8, M55-HE), two
 * independent ways: a TAS2563 register flag (no ears needed) AND an acoustic
 * loopback through the EVK's own on-board PDM mics (no bench operator needed
 * either -- the maintainer asked for this because they are not at the bench
 * to listen).
 *
 * ============================================================================
 * BENCH-TEST SCOPE (test/u46-i2s-tas2563-on-reworked-mux) -- READ FIRST
 * ============================================================================
 * On EVERY un-reworked E1M-EVK 2626-R2, I2S playback stays disabled -- see
 * examples/aen/aen-i2s-amp-alif and examples/aen/aen-evk-demo's file headers:
 * U46, the I2S0 74LVC157 mux, has NO high-impedance state, so its SoC-facing
 * Y outputs (I2S0_WS/SCLK/SDO) are forced LOW whenever `/E` is HIGH -- and
 * those Y outputs are wired BACKWARDS for playback (they drive the SoC's own
 * I2S3 TX pads, not the amps), so the SoC's own I2S3 controller fights U46
 * the instant its pinctrl is applied, regardless of ENABLE.
 *
 * This app runs on bench board `e1m-aen-evk-03` ONLY, where the maintainer has
 * PHYSICALLY REPLACED U46 (plus U38/U39, the SD mux) with 74LV3257 bus
 * switches -- bidirectional, with a TRUE high-impedance state when `/E` is
 * deasserted. That removes the forced-low/direction-conflict premise above,
 * so this app's board overlay re-enables `i2s3` for real and drives the mux
 * over the CC3501E bridge, to prove or disprove the rework on silicon. This
 * is a SEPARATE bench test from test/2051-sdhc-enable-on-reworked-mux (the SD
 * card path through the same rework, U38/U39) -- do not conflate the two or
 * read either as "the mux defect is fixed everywhere". See the board
 * overlay's own header for the same caveat in the devicetree.
 * ============================================================================
 *
 * U46 NETLIST (E1M-EVK-2626-R2_pinmap.csv, `grep -a ',U46,'`):
 *   I2S_SELECT -> U46 pin 1 (S)      I2S_EN -> U46 pin 15 (\E)
 *   AMP_I2S0_WS/SCLK/SDO/SDI  are the "x|0" inputs (S=0 side)  -- the amps.
 *   M2E_I2S_WS/CLK/DOUT/DIN   are the "x|1" inputs (S=1 side)  -- the M.2 slot.
 *   I2S0_WS/SCLK/SDO/SDI      are the Y outputs -- the SoC side.
 * `I2S_EN` and `I2S_SELECT` carry NO pull resistors -- they float until
 * driven, so this app drives both explicitly, every run, rather than relying
 * on a reset-time default:
 *   I2S_EN     = EVK_PIN_I2S_MUX_EN  = E1M_GPIO_IO8,  active low, CC3501E-
 *                proxied (E1M IO8 -> CC3501E GPIO_30,
 *                metadata/e1m_modules/E1M-AEN801.yaml:336-337).
 *   I2S_SELECT = EVK_PIN_I2S_MUX_SEL = E1M_GPIO_IO13, CC3501E-proxied
 *                (E1M IO13 -> CC3501E GPIO_13, :344-345). S=0 selects the
 *                "x|0" inputs = AMP_I2S0_* = the TAS2563 amps
 *                (metadata/boards/e1m-evk.yaml:125, "0 = TAS2563 amps").
 *
 * MUX DRIVE ORDER -- SELECT BEFORE ENABLE, deliberately: this app configures
 * and writes I2S_SELECT=0 (amps) to completion BEFORE it ever touches
 * I2S_EN. A 74LV3257 with `/E` high is Hi-Z regardless of S, so driving
 * SELECT first costs nothing electrically, but it means the switch can never
 * be found CLOSED onto the M.2 E-key side (S=1) for even one instruction --
 * the M.2 slot never sees a moment where ENABLE is already low and SELECT
 * has not yet been asserted to the amp side.
 *
 * ============================================================================
 * EVIDENCE 1 -- THE TDM_CLOCK REGISTER FLAG (no ears needed)
 * ============================================================================
 * TAS2563's four latched-interrupt registers (INT_LTCH0..4, read as one
 * packed word by tas2563_read_faults(), include/alp/chips/tas2563.h) carry
 * @ref TAS2563_FAULT_TDM_CLOCK (0x24[2], "TDM clock error", SLASET3D SS7.5.36
 * Table 7-136): the amp's own hardware report of whether it currently sees a
 * valid TDM/I2S clock on its RX pins.
 *
 * An earlier version of this app compared a pre-ACTIVE (software-shutdown)
 * baseline against a during-playback read and let that decide the verdict.
 * That is unsound: neither tas2563.h nor tas2563.c documents whether the
 * TDM-clock-error latch is even live while the part sits in software
 * shutdown, so a CLEAR/CLEAR run (the latch simply never active yet) would
 * have printed "clocks reach the amp" -- a false hardware conclusion. This
 * version instead runs a STOP-CONTROL entirely inside the ACTIVE window,
 * where the latch is known to be live (SLASET3D SS7.3.12 documents the fault
 * pin/latch behaviour for the ACTIVE/operating condition):
 *   DURING  -- both amps ACTIVE, the tone genuinely playing (taken right
 *              after the loudest volume step below): clear the latch,
 *              settle, read. Expected CLEAR if SCLK/WS reach the amp.
 *   STOPPED -- I2S3 TX stopped (alp_audio_out_stop(), which drains then
 *              I2S_TRIGGER_DRAINs the DesignWare controller -- see
 *              src/backends/audio/zephyr_drv.c's z_out_stop() and
 *              src/backends/i2s/zephyr_drv.c's alp_i2s_stop()) while BOTH
 *              amps stay ACTIVE (no tas2563_set_mode(SHUTDOWN) yet): clear
 *              the latch again, settle, read. Expected SET now that nothing
 *              clocks SCLK/WS.
 * A DURING-CLEAR + STOPPED-SET pair is the only outcome that demonstrates
 * the flag genuinely tracks this run's own clock, not a coincidence of
 * power-up state. Stopping I2S while ACTIVE is safe here because the part
 * mutes on a clock error (SLASET3D SS7.3.12) rather than free-running the
 * Class-D stage without one. The pre-ACTIVE software-shutdown read is kept
 * ONLY as supplementary printed context -- see faults_before below, which
 * does not drive the verdict.
 *
 * TAS2563_FAULT_ASI2_CLOCK (0x27[3], a SECOND serial-interface clock-error
 * bit) is NOT used here -- it names ASI2, a different serial port from the
 * TDM/I2S receiver this board wires, so it says nothing about this signal
 * path. The "DSP Mode & TDM_DET" register (0x11, SLASET3D SS7.5.19,
 * FS_RATIO/FS_RATE clock-detect readback) is NOT exposed by this driver --
 * no macro exists for it in include/alp/chips/tas2563.h -- so it is TBD, not
 * used, and not invented here.
 *
 * ============================================================================
 * EVIDENCE 2 -- ACOUSTIC LOOPBACK THROUGH THE ON-BOARD PDM MICS
 * ============================================================================
 * These two lines of evidence are INDEPENDENT -- this app reports both, and
 * neither is allowed to paper over the other.
 *
 * MIC MAPPING (verified, not assumed, against the actual netlist + SoM
 * routing table, not just the aen-evk-demo/aen-pdm-mic-alif file headers
 * that describe the same hardware):
 *   metadata/boards/e1m-evk.yaml:64: "pdm_mic: true # 4x PDM mics";
 *   :249-250: "E1M_I2S1 / EVK_I2S_PDM_MIC -- PDM mic capture (4x MP34DT05
 *   mics)." Carrier netlist (E1M-EVK-2626-R2_pinmap.csv, `grep -a -i pdm`):
 *   U19/U20 share E1M pads AA1 (PDM_C0, clock) / AA2 (PDM_D0, data) through
 *   series resistors R69-72 ONLY -- no mux component (no U38/U39/U46, no
 *   74LVC157/74LV3257) anywhere in those netlist rows. U19.LR ties to +VIO
 *   (HIGH = LEFT, per the LEFT/RIGHT edge-select convention
 *   examples/aen/aen-evk-demo's phase 11 documents) and U20.LR ties to 0V
 *   (LOW = RIGHT); both share the one PDM_C0/D0 clock+data pair, distinguished
 *   by which clock edge each answers on. U25/U26 (the second pair, PDM_C1/
 *   D1) are wired the same way but are NOT used here -- see below.
 *   SoM routing (metadata/e1m_modules/aen/from-alif.tsv): AA1 (PDM_C0) ->
 *   Alif P6_1, alt fn PDM_C0_C; AA2 (PDM_D0) -> Alif P6_0, alt fn PDM_D0_C.
 *   Controller: pdm@4902d000, the Ensemble E8 "HP PDM" (main peripheral
 *   domain, CLKCTL_PER_SLV) -- NOT lppdm@43002000 (the LP-domain PDM,
 *   zephyr/dts/alif/ensemble_e8_peripherals.dtsi:414, `status = "disabled"`
 *   by default, and that same dtsi's own comment at :398-400 confirms the
 *   EVK mics are on the main-domain node). Same alif,alif-pdm driver backs
 *   both instances (DT_DRV_COMPAT alif_alif_pdm, zephyr/drivers/audio/
 *   alif_pdm.c:48) -- only the base address/pinctrl differ.
 *   NO MUX: confirmed above -- the PDM path is series resistors + the two
 *   mic ICs, nothing switched. Safe to enable directly; no CC3501E proxy,
 *   no U46-style gating, no rework dependency of any kind.
 *   Only the FIRST pair (U19 LEFT / U20 RIGHT, channels 0/1) is captured:
 *   <alp/audio.h>'s Zephyr backend caps channels at 2
 *   (src/backends/audio/zephyr_drv.c z_in_open()), so the second pair
 *   (U25/U26, channels 4/5 on the SoC's own numbering) is unreachable
 *   through the portable capture API regardless of whether its pads are
 *   declared -- the same reasoning examples/aen/aen-evk-demo's phase 11
 *   already used for this exact mic.
 *
 * DOES THE PDM CAPTURE USE DMA? NO -- same answer as I2S3 TX (see below).
 * zephyr/drivers/audio/alif_pdm.c has ZERO "dma"/"DMA" references anywhere
 * in the file (grepped); its ISRs (alif_pdm_warning_isr() et al.) poll
 * PDM_FIFO_STATUS_REGISTER and copy FIFO bursts into k_mem_slab blocks via
 * plain sys_read32()/memory stores on the CPU, exactly like i2s_dw.c's
 * FIFO-drain ISR. So the DTCM-unreachable-by-a-bus-master class of bug the
 * SD test hit (test/2051-sdhc-enable-on-reworked-mux) does not apply here
 * either -- no `chosen { zephyr,sram = &sram0; }` override is needed, and
 * this app's RAM stays DTCM (the bench `chosen { zephyr,flash = &itcm; }`
 * retarget's usual default).
 *
 * ANALYSIS METHOD -- a single-bin Goertzel, not a general FFT. <alp/dsp.h>
 * exists but only exposes a composable chain terminated by a full
 * power-of-two FFT (ALP_DSP_MIN_FFT_POINTS=32 .. ALP_DSP_MAX_FFT_POINTS=1024,
 * include/alp/dsp.h) -- heavier machinery than three frequency bins need,
 * and the task this app was built against explicitly says so ("no FFT
 * library needed for one bin"). goertzel_t below is the textbook streaming
 * recursion (state = two floats per bin, no sample buffer, fed one sample
 * at a time as mic blocks arrive) -- O(1) memory per bin regardless of
 * window length. Three bins are tracked per channel: the TONE_BIN_HZ tone
 * itself, plus two off-tone reference bins (REF_BIN_LOW_HZ/REF_BIN_HIGH_HZ)
 * -- a real 1 kHz tone should stand out against ITS neighbours, not just
 * against a silent recording, which is what stops ambient wideband noise
 * (fan, HVAC) at any single "loud" frequency from faking a pass. Plus plain
 * RMS (accumulated in `double` for precision over ACOUSTIC_ANALYSIS_FRAMES
 * samples, converted to float only at the end) as a coarse sanity check.
 *
 * EXACT-BIN WINDOW SIZING -- a Goertzel bin is only leakage-free when the
 * target frequency lands EXACTLY on an integer DFT bin of the analysis
 * window: k = N * f / fs must be a whole number. With fs = MIC_SAMPLE_RATE_HZ
 * = 16000 Hz and N = ACOUSTIC_ANALYSIS_FRAMES = 6400 (25 blocks of 256
 * frames, ~400 ms): k(1000 Hz) = 6400*1000/16000 = 400, k(800 Hz) =
 * 6400*800/16000 = 320, k(1200 Hz) = 6400*1200/16000 = 480 -- all three
 * exact integers, verified by hand here (not just "computed at runtime and
 * hoped").
 *
 * WINDOW SEQUENCE, in order (each ~(ACOUSTIC_DISCARD_BLOCKS +
 * ACOUSTIC_ANALYSIS_BLOCKS) * 16 ms ~= 496 ms, first ACOUSTIC_DISCARD_BLOCKS
 * = 6 blocks / ~96 ms of each window discarded for settling):
 *   1. BASELINE  -- amps SHUTDOWN, I2S3 not yet opened. Ambient.
 *   2. VOL=4, VOL=16, VOL=48 -- amps ACTIVE (confirmed per the safety gate
 *      below), I2S3 playing the tone at each digital-volume step in turn.
 *      The TDM_CLOCK "DURING" read (Evidence 1) is taken right after the
 *      VOL=48 window, while the tone is still genuinely playing -- reusing
 *      this tone-on time rather than adding a separate segment.
 *   3. STOPPED -- I2S3 drained (the same stop-control Evidence 1 uses),
 *      amps still ACTIVE, no tone. The TDM_CLOCK "STOPPED" read is taken
 *      right before this window's mic capture.
 *   4. SHUTDOWN -- both amps back in TAS2563_MODE_SHUTDOWN, no tone.
 * Total tone-ON time: SOUND_PREACTIVE_BLOCKS (2 blocks, ~32 ms, the quiet
 * pre-ACTIVE priming -- see HARDWARE SAFETY) + 3 * (ACOUSTIC_DISCARD_BLOCKS +
 * ACOUSTIC_ANALYSIS_BLOCKS) blocks (~1488 ms) ~= 1.52 s, comfortably under a
 * 5 s budget. The BASELINE/STOPPED/SHUTDOWN windows add mic-only wall time
 * (no tone) on top -- total wall time across all six windows ~= 3 s.
 *
 * ACOUSTIC VERDICT -- see acoustic_verdict_t / the matrix built in main():
 * composite (2-channel-averaged, linear-power-domain-averaged) tone-bin dB
 * across the three volume windows must ALL clear (BASELINE, STOPPED,
 * SHUTDOWN's tone-bin dB) + ACOUSTIC_MARGIN_DB, must rise monotonically
 * with volume, and must exceed BOTH off-tone reference bins at every step --
 * TONE HEARD only if all three hold; TONE NOT HEARD only if NO volume step
 * clears the margin; INCONCLUSIVE otherwise (named explicitly, per which
 * criterion failed). The scaling-with-volume and off-tone-reference checks
 * are what stop a mic fault (stuck-high, clipping) or ambient noise from
 * faking a pass -- neither is dropped.
 *
 * ============================================================================
 * HARDWARE SAFETY -- unchanged from the register-flag-only version, plus
 * ONE new cap for the digital volume steps
 * ============================================================================
 * Both TAS2563s can drive ~10 W peak into 4 ohm (SLASET3D Table 7-105);
 * tas2563.h's own @warning calls the power-on AMP_LEVEL "near the top of the
 * part's range". This app never exceeds:
 *   - TAS2563_AMP_LEVEL_MIN (8.5 dBV, the quietest listed analog gain code) --
 *     set on BOTH amps over I2C while they are still in software shutdown,
 *     and CONFIRMED by a register readback (tas2563_read_amp_level(), added
 *     to the driver for exactly this) before either amp is ever told
 *     ACTIVE. A write whose readback does not come back MIN keeps BOTH amps
 *     in software shutdown for the rest of this run, full stop -- see
 *     level_confirmed[]/i2s_confirmed[] below. This is THE analog ceiling
 *     for the WHOLE run -- only the digital volume changes across steps.
 *   - SOUND_VOL_MAX (48/255 ~= 18.8% of full digital scale), enforced at
 *     COMPILE TIME by the BUILD_ASSERTs below on every entry of
 *     sound_vol_steps[]. Why 48/255 is speaker-safe layered on top of the
 *     8.5 dBV analog floor: 8.5 dBV = 3.76 Vpk (SLASET3D Table 7-105) is
 *     already the quietest analog code TAS2563 offers -- roughly 9x lower
 *     amplitude, ~17x lower power, than the 22 dBV/17.8 Vpk maximum. The
 *     digital volume (alp_audio_out_set_volume(), 0..255 linear,
 *     include/alp/audio.h) scales the PCM sample amplitude BEFORE that
 *     analog stage; at vol=48 the effective sample amplitude is
 *     SOUND_TONE_AMPLITUDE * 48/255 ~= 3764 out of a possible 32767 (~11.5%
 *     of digital full-scale), so the two ceilings COMPOUND rather than
 *     either alone bounding the output: quietest-available analog gain
 *     driven by a signal already well under a fifth of digital full-scale.
 *   - A FLAT digital volume within each window (only stepped BETWEEN
 *     windows, never ramped within one) -- opened and STARTED at the first,
 *     quietest step (sound_vol_steps[0] = 4) before ACTIVE, so the amp
 *     comes out of shutdown already receiving a near-silent stream.
 *   - Short tone-on time in total -- see WINDOW SEQUENCE above (~1.52 s),
 *     and teardown always mutes (tas2563_set_mode(SHUTDOWN)) BOTH amps
 *     before anything else tears down. This is true on EVERY exit path, not
 *     just the bottom of main(): every early return past the point
 *     AMP_ENABLE is first released (`--- 2.` below) drives AMP_ENABLE back
 *     to 0, mutes any amp that ever answered I2C, deinitialises it,
 *     releases the mux `/E`, and -- once the I2C bus is open -- closes it.
 *   Both levers (analog MIN + a bounded, stepped digital volume) mirror
 *   examples/aen/aen-evk-demo's phase 11, which this app was mined from --
 *   see that phase's file header for the fuller "why two independent
 *   levers" reasoning.
 *
 * DOES I2S3 USE DMA? NO. The vendored `snps,designware-i2s` driver
 * (zephyr/drivers/i2s/i2s_dw.c, CONFIG_I2S_DW) is FIFO/interrupt-driven --
 * see that file's own header comment ("no DMA subsystem needed") and its ISR
 * (i2s_dw_isr_handler()), which copies each k_mem_slab block into/out of the
 * TX/RX FIFO register a word at a time on the CPU, not via an external DMA
 * bus master. So the class of bug the SD test hit on this same board
 * (test/2051-sdhc-enable-on-reworked-mux: an ADMA descriptor table placed in
 * CPU-local DTCM at 0x20000a00, unreachable by the SDHC controller's own bus
 * master, because `zephyr,sram = &dtcm` on AEN801 boards) DOES NOT APPLY
 * here -- there is no second bus master that ever needs to resolve this
 * app's I2S TX buffer address; the CPU touches every byte itself. The
 * `alp_i2s_open()` TX slab, this app's own `static int16_t tone_buf[]`, and
 * the PDM mic's `static int16_t mic_buf[]` (capture_window() below) all live
 * in whatever RAM the linker puts BSS in (this board's default -- ITCM/DTCM
 * per the bench RAM-run `chosen` retarget), and that is fine precisely
 * because nothing but the CPU ever reads them.
 *
 * A REAL RISK WORTH NAMING: whether a quiet 8.5 dBV tone at a bounded
 * digital volume is loud enough for the EVK's own on-board mics to pick up
 * at all is NOT something this app can know in advance -- the two safety
 * ceilings above were chosen to be conservative BEFORE knowing whether they
 * leave enough acoustic margin for the mics to hear over ambient bench
 * noise. If the ACOUSTIC VERDICT below comes back TONE NOT HEARD or
 * INCONCLUSIVE while Evidence 1 (the TDM_CLOCK flag) says CLOCKS REACH AMP,
 * the most likely explanation is exactly this -- an electrically-proven but
 * acoustically-too-quiet signal chain, not a broken mux. The two lines of
 * evidence are independent for exactly this reason; do not let one silently
 * overrule the other.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/audio.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/tas2563.h"
#include "alp/i2s.h"
#include "alp/peripheral.h"

#include "cc3501e_bridge.h"

/* ---- AMP_ENABLE (SD_N, P5_2) / AMP_FAULT (IRQZ, P5_0) on raw gpio5 ------- */
/* Both are "overlay pins" past the standard 52-slot E1M map this standalone
 * app does not carry (metadata/boards/e1m-evk.yaml overlay_pins:) -- reached
 * via pinctrl_configure_pins() + the Zephyr gpio_pin_* API directly on
 * gpio5, the same pattern examples/aen/aen-evk-demo's phase 11 uses. gpio_dw
 * applies no pad mux of its own, so function-0 GPIO has to be selected
 * through Alif pinctrl first. */
#define AMP_ENABLE_PIN 2 /* SPI0_CS0 = P5_2 (gpio5), active-high SD_N. */
#define AMP_FAULT_PIN  0 /* SPI0_MISO = P5_0 (gpio5), open-drain IRQ_N, active-low. */
static const pinctrl_soc_pin_t amp_enable_mux[] = { PIN_P5_2__GPIO };
#define AMP_FAULT_PAD_REN (1U << 16) /* pad input-buffer enable -- an input pin needs it. */
static const pinctrl_soc_pin_t amp_fault_mux[] = { PIN_P5_0__GPIO | AMP_FAULT_PAD_REN };

/* SLASET3D SS7.3.11.1: SDZ low ramps audio down, stops Class-D, then enters
 * Hardware Shutdown -- SDZ_TIMEOUT's worst case is 23.8 ms (Table 7-7).
 * Holding SD_N low this long guarantees Hardware Shutdown is reached. */
#define AMP_ENABLE_RESET_HOLD_MS 24u
#define MUX_SETTLE_MS            10u
#define CLOCK_CHECK_SETTLE_MS    20u /* time for a fresh TDM-clock-detect edge after clear. */

#define SOUND_SAMPLE_RATE_HZ   16000u
#define SOUND_FRAMES_PER_BLOCK 256u
#define SOUND_TONE_HZ          1000u
#define SOUND_TONE_AMPLITUDE   20000 /* int16, headroom below INT16_MAX. */
#define SOUND_PREACTIVE_BLOCKS 2u    /* ~32ms of quiet (step-0 volume) stream before ACTIVE. */

/* Digital volume steps + the hard compile-time cap -- see the file header's
 * HARDWARE SAFETY section for why 48/255 is speaker-safe layered on top of
 * the TAS2563_AMP_LEVEL_MIN analog floor. Named #defines (not array
 * elements) so BUILD_ASSERT sees a real integer constant expression. */
#define SOUND_VOL_STEP_0 4u
#define SOUND_VOL_STEP_1 16u
#define SOUND_VOL_STEP_2 48u
#define SOUND_VOL_MAX    48u
BUILD_ASSERT(SOUND_VOL_STEP_0 <= SOUND_VOL_MAX, "digital volume step exceeds the safety cap");
BUILD_ASSERT(SOUND_VOL_STEP_1 <= SOUND_VOL_MAX, "digital volume step exceeds the safety cap");
BUILD_ASSERT(SOUND_VOL_STEP_2 <= SOUND_VOL_MAX, "digital volume step exceeds the safety cap");
static const uint8_t sound_vol_steps[] = { SOUND_VOL_STEP_0, SOUND_VOL_STEP_1, SOUND_VOL_STEP_2 };
#define SOUND_VOL_STEP_COUNT ARRAY_SIZE(sound_vol_steps)

/* ---- PDM mics (U19 LEFT / U20 RIGHT) -- see the file header's MIC MAPPING */
#define MIC_CHANNELS        2u                   /* ch0=U19 LEFT, ch1=U20 RIGHT. */
#define MIC_SAMPLE_RATE_HZ  SOUND_SAMPLE_RATE_HZ /* one shared clock domain/timing base. */
#define MIC_READ_TIMEOUT_MS 200u

/* ---- Acoustic analysis window -- see the file header's EXACT-BIN WINDOW
 * SIZING derivation for why these particular numbers. */
/* ~96ms (6*16ms) settle, discarded before analysis starts. Not a measured
 * PDM/DC-block settling time -- deliberate headroom, chosen over
 * examples/aen/aen-evk-demo's phase 11 precedent (SOUND_BASELINE_BLOCKS
 * there accumulates energy from the very FIRST captured block, no discard
 * at all) because that phase's coarse |sum| energy check tolerates a noisy
 * lead-in sample or two while a single-bin Goertzel over an EXACT window
 * (see EXACT-BIN WINDOW SIZING) has no such slack: any transient in the
 * analyzed window (mic startup, the DC-blocking filter's own settle, or
 * the tone/volume step that just changed) leaks across every bin. 6 blocks
 * covers the DC-blocker's own time constant several times over (alpha =
 * 0.995 in dc_block_s16(), src/backends/audio/zephyr_drv.c -- time
 * constant ~= 1/(1-0.995) = 200 samples = 12.5 ms at 16 kHz) with margin
 * to spare; nothing more rigorous than that informed the choice of 6. */
#define ACOUSTIC_DISCARD_BLOCKS  6u
#define ACOUSTIC_ANALYSIS_BLOCKS 25u /* ~400ms, 25*256=6400 samples -- the exact-bin window. */
#define ACOUSTIC_ANALYSIS_FRAMES (ACOUSTIC_ANALYSIS_BLOCKS * SOUND_FRAMES_PER_BLOCK)
#define TONE_BIN_HZ              SOUND_TONE_HZ /* 1000 Hz, exact bin k=400 at N=6400/fs=16000. */
#define REF_BIN_LOW_HZ           800u          /* exact bin k=320. */
#define REF_BIN_HIGH_HZ          1200u         /* exact bin k=480. */
#define ACOUSTIC_MARGIN_DB       10.0f         /* the task's own example threshold. */

#define AMP_COUNT 2u
static const uint8_t amp_addrs[AMP_COUNT] = {
	EVK_I2C_ADDR_TAS2563_LOW,  /* U27, LEFT  (J14) */
	EVK_I2C_ADDR_TAS2563_HIGH, /* U28, RIGHT (J21) */
};
/* Explicit LEFT/RIGHT, not TAS2563_RX_SLOT_FROM_ADDR -- see the @warning on
 * tas2563_rx_channel_t (include/alp/chips/tas2563.h) for why FROM_ADDR
 * silently mutes U28 on this board's 2-slot I2S frame. */
static const tas2563_rx_channel_t amp_rx_channel[AMP_COUNT] = {
	TAS2563_RX_LEFT,
	TAS2563_RX_RIGHT,
};

/* The named outcomes this probe can report for EVIDENCE 1 (the TDM_CLOCK
 * register flag) -- see the file header for what DURING/STOPPED mean.
 *
 * AMP_CONTROL_GPIO_FAILED is split out from TAS2563_NOT_RESPONDING: a
 * gpio5 device or pinctrl fault is a DIFFERENT layer than an I2C NACK, and
 * printing "TAS2563 I2C not responding" for a GPIO fault the app never got
 * far enough to even attempt I2C on would misname the cause.
 *
 * INCONCLUSIVE covers every DURING/STOPPED combination that is not the
 * clean "flag demonstrably tracks the clock" pair in either direction --
 * see the verdict matrix in main() for the four combinations. */
typedef enum {
	VERDICT_BRIDGE_MUX_FAILED,
	VERDICT_AMP_CONTROL_GPIO_FAILED,
	VERDICT_TAS2563_NOT_RESPONDING,
	VERDICT_CLOCKS_NOT_REACHING_AMP,
	VERDICT_CLOCKS_REACH_AMP,
	VERDICT_INCONCLUSIVE,
} verdict_t;

static const char *verdict_str(verdict_t v)
{
	switch (v) {
	case VERDICT_BRIDGE_MUX_FAILED:
		return "bridge/mux-enable failed";
	case VERDICT_AMP_CONTROL_GPIO_FAILED:
		return "AMP control GPIO failed (gpio5 / AMP_ENABLE / AMP_FAULT)";
	case VERDICT_TAS2563_NOT_RESPONDING:
		return "TAS2563 I2C not responding";
	case VERDICT_CLOCKS_NOT_REACHING_AMP:
		return "I2S clocks not reaching the amp";
	case VERDICT_CLOCKS_REACH_AMP:
		return "clocks reach the amp";
	case VERDICT_INCONCLUSIVE:
		return "inconclusive (TDM_CLOCK flag did not discriminate -- see the per-amp reads above)";
	default:
		return "unknown";
	}
}

/* mux_sel/mux_en teardown shared by every exit path.  alp_gpio_close() on
 * NULL is a documented no-op, so this is safe to call from a path that never
 * got as far as opening either handle. */
static void mux_disable(alp_gpio_t *mux_sel, alp_gpio_t *mux_en)
{
	if (mux_en != NULL) {
		(void)alp_gpio_write(mux_en, true); /* /E high = mux disabled/Hi-Z. */
	}
	alp_gpio_close(mux_sel);
	alp_gpio_close(mux_en);
}

/* Per-amp TDM_CLOCK verdict for the DURING/STOPPED pair -- see the file
 * header's EVIDENCE 1 section and the matrix in main(). */
typedef enum {
	AMP_CLOCK_REACHES,
	AMP_CLOCK_NOT_REACHING,
	AMP_CLOCK_INCONCLUSIVE,
} amp_clock_verdict_t;

static amp_clock_verdict_t amp_clock_verdict(uint32_t during, uint32_t stopped)
{
	bool during_clear = (during & TAS2563_FAULT_TDM_CLOCK) == 0u;
	bool stopped_set  = (stopped & TAS2563_FAULT_TDM_CLOCK) != 0u;
	if (during_clear && stopped_set) return AMP_CLOCK_REACHES;       /* the clean, strong case. */
	if (!during_clear && stopped_set) return AMP_CLOCK_NOT_REACHING; /* flag live, stayed set. */
	return AMP_CLOCK_INCONCLUSIVE; /* during_clear&&!stopped_set, or !during_clear&&!stopped_set. */
}

/* One tone block: a square wave, identical samples on both channels (both
 * speakers play the same tone, not a stereo mix -- see amp_rx_channel[]'s
 * comment on why BOTH channels need real, non-zero samples). Advances
 * *phase_acc and blocks in alp_audio_out_write() for real wall-clock time.
 * Returns false on any write failure. */
static bool write_one_tone_block(alp_audio_out_t *spk,
                                 int16_t         *buf,
                                 uint32_t        *phase_acc,
                                 uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		int16_t sample  = ((*phase_acc % samples_per_cycle) < samples_per_cycle / 2u)
		                      ? SOUND_TONE_AMPLITUDE
		                      : -SOUND_TONE_AMPLITUDE;
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u) == ALP_OK;
}

/* Plain tone-only pacing loop, used ONLY when the PDM mic failed to open --
 * still drives the tone for EVIDENCE 1 (the register flag), just with no
 * acoustic capture running alongside it. */
static bool play_tone_blocks(alp_audio_out_t *spk,
                             int16_t         *buf,
                             uint32_t        *phase_acc,
                             uint32_t         samples_per_cycle,
                             unsigned         count)
{
	for (unsigned b = 0; b < count; b++) {
		if (!write_one_tone_block(spk, buf, phase_acc, samples_per_cycle)) return false;
	}
	return true;
}

/* ================================================================== */
/* EVIDENCE 2 -- streaming single-bin Goertzel + the per-window capture */
/* ================================================================== */

#define GOERTZEL_TWO_PI 6.283185307179586f

/* State for ONE frequency bin -- two floats, no sample buffer. Reset once
 * per analysis window (goertzel_reset), fed one sample at a time
 * (goertzel_step) as mic blocks arrive, read once at the end
 * (goertzel_power). */
typedef struct {
	float coeff;
	float s_prev;
	float s_prev2;
} goertzel_t;

static void
goertzel_reset(goertzel_t *g, uint32_t freq_hz, uint32_t n_samples, uint32_t sample_rate_hz)
{
	/* k is the (caller-verified-exact, see EXACT-BIN WINDOW SIZING) bin
	 * index for this window length; computed in float only because cosf()
	 * takes float, not because the bin index is allowed to be fractional. */
	float k     = ((float)n_samples * (float)freq_hz) / (float)sample_rate_hz;
	float omega = GOERTZEL_TWO_PI * k / (float)n_samples;
	g->coeff    = 2.0f * cosf(omega);
	g->s_prev   = 0.0f;
	g->s_prev2  = 0.0f;
}

static inline void goertzel_step(goertzel_t *g, float x)
{
	float s    = x + g->coeff * g->s_prev - g->s_prev2;
	g->s_prev2 = g->s_prev;
	g->s_prev  = s;
}

static float goertzel_power(const goertzel_t *g)
{
	return g->s_prev * g->s_prev + g->s_prev2 * g->s_prev2 - g->coeff * g->s_prev * g->s_prev2;
}

/* 10*log10, not 20*log10 -- goertzel_power() is already a squared-amplitude
 * (power-like) quantity, same convention DSP text books use for a bin
 * magnitude squared. +1e-6f floors it away from log(0)/log(negative) on a
 * true-silence window (float rounding can put goertzel_power() a hair
 * below zero for an all-zero input). */
#define POWER_TO_DB(p) (10.0f * log10f((p) + 1e-6f))

/* Per-channel result of one capture window: LINEAR power (not yet dB, so
 * callers can average across channels correctly -- see composite_of()
 * below) for the tone bin + both reference bins, plus plain RMS amplitude
 * (not dB; printed as a raw sanity-check number). */
typedef struct {
	float tone_power;
	float ref_low_power;
	float ref_high_power;
	float rms;
} chan_result_t;

/*
 * Run one capture window: discard ACOUSTIC_DISCARD_BLOCKS blocks (settle),
 * then feed ACOUSTIC_ANALYSIS_BLOCKS more into three Goertzel bins + an RMS
 * accumulator PER CHANNEL. If spk is non-NULL, writes one tone block before
 * each mic read (both discard and analysis phases), so the read that
 * follows samples genuinely-playing audio -- the same write-then-read
 * interleave examples/aen/aen-evk-demo's phase 11 uses. If spk is NULL,
 * this is a silent window (BASELINE/STOPPED/SHUTDOWN) -- mic-only.
 *
 * @p mic must be a valid, started handle -- this function does not handle
 * mic == NULL; see play_tone_blocks() for the no-mic fallback used when the
 * PDM mic itself failed to open.
 *
 * Returns false if either the mic read or (when spk != NULL) the tone
 * write ever failed, or a read returned fewer than SOUND_FRAMES_PER_BLOCK
 * frames (the fixed-N Goertzel bin math assumes full blocks) -- the caller
 * must not trust @p results on a false return.
 */
static bool capture_window(alp_audio_in_t  *mic,
                           alp_audio_out_t *spk,
                           int16_t         *tone_buf,
                           uint32_t        *phase_acc,
                           uint32_t         samples_per_cycle,
                           chan_result_t    results[MIC_CHANNELS])
{
	static int16_t mic_buf[SOUND_FRAMES_PER_BLOCK * MIC_CHANNELS]; /* static: off caller's stack. */

	for (unsigned b = 0; b < ACOUSTIC_DISCARD_BLOCKS; b++) {
		if (spk != NULL && !write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle)) {
			return false;
		}
		size_t got = 0;
		if (alp_audio_in_read(mic, mic_buf, SOUND_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS) !=
		    ALP_OK) {
			return false;
		}
	}

	goertzel_t tone[MIC_CHANNELS];
	goertzel_t ref_lo[MIC_CHANNELS];
	goertzel_t ref_hi[MIC_CHANNELS];
	double     sumsq[MIC_CHANNELS] = { 0 };
	for (size_t c = 0; c < MIC_CHANNELS; c++) {
		goertzel_reset(&tone[c], TONE_BIN_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
		goertzel_reset(&ref_lo[c], REF_BIN_LOW_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
		goertzel_reset(&ref_hi[c], REF_BIN_HIGH_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
	}

	for (unsigned b = 0; b < ACOUSTIC_ANALYSIS_BLOCKS; b++) {
		if (spk != NULL && !write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle)) {
			return false;
		}
		size_t got = 0;
		if (alp_audio_in_read(mic, mic_buf, SOUND_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS) !=
		    ALP_OK) {
			return false;
		}
		if (got != SOUND_FRAMES_PER_BLOCK) return false; /* fixed-N bin math needs full blocks. */
		for (size_t f = 0; f < got; f++) {
			for (size_t c = 0; c < MIC_CHANNELS; c++) {
				float x = (float)mic_buf[f * MIC_CHANNELS + c];
				goertzel_step(&tone[c], x);
				goertzel_step(&ref_lo[c], x);
				goertzel_step(&ref_hi[c], x);
				sumsq[c] += (double)x * (double)x;
			}
		}
	}

	for (size_t c = 0; c < MIC_CHANNELS; c++) {
		results[c].tone_power     = goertzel_power(&tone[c]);
		results[c].ref_low_power  = goertzel_power(&ref_lo[c]);
		results[c].ref_high_power = goertzel_power(&ref_hi[c]);
		results[c].rms            = sqrtf((float)(sumsq[c] / (double)ACOUSTIC_ANALYSIS_FRAMES));
	}
	return true;
}

static void print_acoustic_row(const char *label, const chan_result_t r[MIC_CHANNELS], bool ok)
{
	if (!ok) {
		printf("[probe] ACOUSTIC %-8s -- capture failed (write/read error), no data\n", label);
		return;
	}
	static const char *const chan_names[MIC_CHANNELS] = { "LEFT/U19", "RIGHT/U20" };
	for (size_t c = 0; c < MIC_CHANNELS; c++) {
		printf("[probe] ACOUSTIC %-8s ch%zu(%-9s): tone(%uHz)=%6.1fdB  ref(%uHz)=%6.1fdB  "
		       "ref(%uHz)=%6.1fdB  RMS=%7.1f\n",
		       label,
		       c,
		       chan_names[c],
		       (unsigned)TONE_BIN_HZ,
		       (double)POWER_TO_DB(r[c].tone_power),
		       (unsigned)REF_BIN_LOW_HZ,
		       (double)POWER_TO_DB(r[c].ref_low_power),
		       (unsigned)REF_BIN_HIGH_HZ,
		       (double)POWER_TO_DB(r[c].ref_high_power),
		       (double)r[c].rms);
	}
}

/* Composite (2-channel, linear-power-domain-averaged) dB values for the
 * ACOUSTIC VERDICT -- averaging power (not pre-converted dB) is the
 * mathematically correct order of operations, and it is a deliberate
 * choice to use the average rather than requiring both mics independently:
 * the two mics sit at different points on the board, so one may legitimately
 * couple to the speakers better than the other. */
typedef struct {
	float tone_db;
	float ref_low_db;
	float ref_high_db;
} composite_db_t;

static composite_db_t composite_of(const chan_result_t r[MIC_CHANNELS])
{
	composite_db_t c;
	c.tone_db     = POWER_TO_DB(0.5f * (r[0].tone_power + r[1].tone_power));
	c.ref_low_db  = POWER_TO_DB(0.5f * (r[0].ref_low_power + r[1].ref_low_power));
	c.ref_high_db = POWER_TO_DB(0.5f * (r[0].ref_high_power + r[1].ref_high_power));
	return c;
}

typedef enum {
	ACOUSTIC_TONE_HEARD,
	ACOUSTIC_TONE_NOT_HEARD,
	ACOUSTIC_INCONCLUSIVE,
} acoustic_verdict_t;

static const char *acoustic_verdict_str(acoustic_verdict_t v)
{
	switch (v) {
	case ACOUSTIC_TONE_HEARD:
		return "TONE HEARD";
	case ACOUSTIC_TONE_NOT_HEARD:
		return "TONE NOT HEARD";
	case ACOUSTIC_INCONCLUSIVE:
		return "INCONCLUSIVE";
	default:
		return "unknown";
	}
}

int main(void)
{
	printf("\n=== aen-i2s-tas2563-probe: I2S0 through the reworked U46 mux ===\n");
	(void)alp_init();

	/* --- 1. CC3501E bridge, then mux SELECT (amps), THEN mux ENABLE ---- */
	static cc3501e_t fw; /* static: ~32 KB, would blow PSPLIM on main()'s stack. */
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[probe] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) {
		printf("[probe] VERDICT: %s (bridge did not come up)\n",
		       verdict_str(VERDICT_BRIDGE_MUX_FAILED));
		return 0;
	}

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[probe] alp_gpio_open(mux SELECT/ENABLE) -> NULL -- check "
		       "CONFIG_ALP_SDK_GPIO_CC3501E_PROXY and src/cc3501e_gpio_routes.c "
		       "carry the IO8/IO13 routes\n");
		mux_disable(mux_sel, mux_en); /* nothing open yet on either handle if this fired. */
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_BRIDGE_MUX_FAILED));
		return 0;
	}
	/* SELECT to the amp side (S=0) FIRST and to completion -- see the file
	 * header's "MUX DRIVE ORDER" note for why. */
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[probe] I2S_SELECT (E1M IO13 -> CC3501E GPIO_13, 0=amps) -> %d\n", (int)mux_rc);
	/* THEN ENABLE (active low). */
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[probe] I2S_EN (E1M IO8 -> CC3501E GPIO_30, active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en); /* EP1: nothing past the mux has been touched yet. */
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_BRIDGE_MUX_FAILED));
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	/* --- 2. AMP_ENABLE (SD_N) hardware reset + AMP_FAULT as input -------- */
	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[probe] gpio5 not ready -- AMP_ENABLE/AMP_FAULT (P5_2/P5_0) unreachable\n");
		mux_disable(mux_sel, mux_en); /* EP2: AMP_ENABLE was never touched -- gpio5 unusable. */
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_AMP_CONTROL_GPIO_FAILED));
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1); /* releases SD_N high. */
	printf("[probe] AMP_ENABLE (SD_N, P5_2) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		/* EP3: AMP_ENABLE may already be HIGH by the time a LATER step
		 * (the AMP_FAULT pinctrl/configure calls) is what actually failed --
		 * drive it back to 0 unconditionally rather than assuming the
		 * failure happened before SD_N was ever released. A gpio_pin_set()
		 * on a pin whose earlier pinctrl/configure step never ran is a
		 * documented no-op-ish failure on this backend, not a hazard. */
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[probe] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d) -- driving "
		       "AMP_ENABLE back to 0; amps may or may not have briefly left hardware "
		       "shutdown\n",
		       grc);
		mux_disable(mux_sel, mux_en);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_AMP_CONTROL_GPIO_FAILED));
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US); /* SDZ just went high -- settle before any I2C. */

	/* --- 3. Carrier I2C bus + tas2563_init() on both amps ---------------- */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	/* Which amps actually answered tas2563_init() -- needed so the EP4
	 * cleanup below only deinits contexts that are real (tas2563_deinit()
	 * on an un-initialised tas2563_t is not something this driver's
	 * contract covers). */
	bool amp_up[AMP_COUNT] = { false, false };
	int  ok_amps           = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[probe] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) {
			amp_up[i] = true;
			ok_amps++;
		}
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[probe] %d/%zu amp(s) answered -- both U27 and U28 are fitted on every "
		       "E1M-EVK, so a partial count is a real fault\n",
		       ok_amps,
		       (size_t)AMP_COUNT);
		/* EP4: every amp that DID answer init() is already in software
		 * shutdown (tas2563_init()'s own contract), but set_mode(SHUTDOWN)
		 * again is cheap and makes "both amps end in SHUTDOWN on every
		 * exit path" true by construction rather than by inference. */
		for (size_t i = 0; i < AMP_COUNT; i++) {
			if (amp_up[i]) {
				(void)tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
				tas2563_deinit(&amps[i]);
			}
		}
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_TAS2563_NOT_RESPONDING));
		return 0;
	}

	/* --- 4. Safety lever 1: quietest analog level, CONFIRMED by readback - */
	/* level_confirmed[i] gates the ONLY thing that matters for hardware
	 * safety: whether amps[i] is ever allowed to reach ACTIVE below. A
	 * write that returns ALP_OK is NOT sufficient by itself -- see the
	 * file header's HARDWARE SAFETY note on why tas2563_set_amp_level()'s
	 * own return cannot be fully trusted -- so this reads PB_CFG1.AMP_LEVEL
	 * back via tas2563_read_amp_level() (added to chips/tas2563 for this)
	 * and only trusts an EXACT match against TAS2563_AMP_LEVEL_MIN. */
	bool level_confirmed[AMP_COUNT] = { false, false };
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc      = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		uint8_t      readback = 0xFFu; /* poisoned: never a valid AMP_LEVEL code. */
		alp_status_t rrc      = ALP_ERR_IO;
		if (lrc == ALP_OK) rrc = tas2563_read_amp_level(&amps[i], &readback);
		level_confirmed[i] =
		    (lrc == ALP_OK) && (rrc == ALP_OK) && (readback == TAS2563_AMP_LEVEL_MIN);
		printf("[probe] tas2563_set_amp_level(0x%02x, MIN=8.5dBV) -> %d, "
		       "tas2563_read_amp_level() -> %d (readback=0x%02x) -- %s\n",
		       amp_addrs[i],
		       (int)lrc,
		       (int)rrc,
		       readback,
		       level_confirmed[i] ? "CONFIRMED" : "NOT CONFIRMED -- will never go ACTIVE");
	}

	/* --- 5. Tell both amps the I2S config the host bus will use ---------- */
	/* channels=2 (genuinely stereo): the Alif DW I2S3 mono path writes a
	 * hardcoded 0 to the RIGHT slot, so U28 (RIGHT) would hear only silence
	 * on a mono stream regardless of which slot it is told to read -- see
	 * amp_rx_channel[]'s comment. word_bits=16 is translated by
	 * word_len_codes() (chips/tas2563/tas2563.c) into the 32-bit RX_SLEN a
	 * 2-slot TDM frame actually needs; this is a pure I2C write, no signal
	 * moves yet. */
	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	bool i2s_confirmed[AMP_COUNT] = { false, false };
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		i2s_confirmed[i] = (crc == ALP_OK);
		printf("[probe] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	/* An amp only ever reaches ACTIVE below if BOTH the level readback and
	 * the I2S configure succeeded -- "An amp whose level is not confirmed
	 * never goes ACTIVE, full stop." If either amp is not confirmed, this
	 * run never opens I2S3 or sets ANY amp ACTIVE at all. */
	int confirmed_count = 0;
	for (size_t i = 0; i < AMP_COUNT; i++) {
		if (level_confirmed[i] && i2s_confirmed[i]) confirmed_count++;
	}
	if (confirmed_count != (int)AMP_COUNT) {
		printf("[probe] %d/%zu amp(s) confirmed (level readback + I2S configure) -- "
		       "refusing to start I2S3 or set any amp ACTIVE\n",
		       confirmed_count,
		       (size_t)AMP_COUNT);
		/* EP5: same shape as EP4 -- both amps are still in software
		 * shutdown (ACTIVE was never reached), but drive it explicitly. */
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
			tas2563_deinit(&amps[i]);
		}
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		alp_i2c_close(bus);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_TAS2563_NOT_RESPONDING));
		return 0;
	}

	/* --- 6. Supplementary-only TDM_CLOCK baseline: still in software
	 * shutdown -- see EVIDENCE 1 for why this does not drive the verdict. */
	uint32_t faults_before[AMP_COUNT] = { 0 };
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_clear_faults(&amps[i]);
	}
	k_msleep(CLOCK_CHECK_SETTLE_MS);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_read_faults(&amps[i], &faults_before[i]);
		printf("[probe] tas2563_read_faults(0x%02x) BASELINE (software shutdown, "
		       "supplementary only) -> 0x%08x (TDM_CLOCK %s)\n",
		       amp_addrs[i],
		       faults_before[i],
		       (faults_before[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET" : "clear");
	}

	/* --- 7. PDM mic: open + start EARLY, so it can capture the acoustic
	 * BASELINE window before I2S3 or ACTIVE ever happen. ------------------ */
	alp_audio_in_t *mic    = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = MIC_SAMPLE_RATE_HZ,
	    .channels         = MIC_CHANNELS,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t    mic_rc = (mic != NULL) ? alp_audio_in_start(mic) : alp_last_error();
	bool            mic_ok = (mic != NULL) && (mic_rc == ALP_OK);
	printf("[probe] alp_audio_in_open+start(PDM, U19 LEFT/U20 RIGHT) -> %d%s\n",
	       (int)mic_rc,
	       mic_ok ? "" : " -- acoustic capture skipped, EVIDENCE 1 (TDM_CLOCK) still runs");

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u]; /* static: off main()'s stack. */
	uint32_t       phase_acc         = 0;
	const uint32_t samples_per_cycle = SOUND_SAMPLE_RATE_HZ / SOUND_TONE_HZ;

	chan_result_t baseline_r[MIC_CHANNELS]                  = { 0 };
	chan_result_t vol_r[SOUND_VOL_STEP_COUNT][MIC_CHANNELS] = { 0 };
	chan_result_t stopped_r[MIC_CHANNELS]                   = { 0 };
	chan_result_t shutdown_r[MIC_CHANNELS]                  = { 0 };
	bool          baseline_ok                               = false;
	bool          vol_ok[SOUND_VOL_STEP_COUNT]              = { false, false, false };
	bool          stopped_ok                                = false;
	bool          shutdown_ok                               = false;

	/* --- 8. Acoustic BASELINE: amps SHUTDOWN, I2S3 not yet opened -------- */
	if (mic_ok) {
		baseline_ok =
		    capture_window(mic, NULL, tone_buf, &phase_acc, samples_per_cycle, baseline_r);
	}

	/* --- 9. I2S3 TX: open at the first (quietest) volume step, THEN start */
	alp_audio_out_t *spk = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc =
	    (spk != NULL) ? alp_audio_out_set_volume(spk, sound_vol_steps[0]) : alp_last_error();
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_start(spk);
	printf("[probe] alp_audio_out_open+set_volume(%u)+start(I2S3) -> %d\n",
	       sound_vol_steps[0],
	       (int)spk_rc);

	/* --- 10. A few quiet blocks -- the stream is running BEFORE either amp
	 * goes ACTIVE (lever 2, digital volume, already engaged). ------------- */
	bool preactive_ok =
	    (spk_rc == ALP_OK) &&
	    play_tone_blocks(spk, tone_buf, &phase_acc, samples_per_cycle, SOUND_PREACTIVE_BLOCKS);

	/* --- 11. ONLY NOW: both (confirmed) amps ACTIVE -- the stream is
	 * already quiet and running, matching examples/aen/aen-evk-demo's
	 * phase 11 ordering. ---------------------------------------------------*/
	bool active_set = false;
	if (preactive_ok) {
		active_set = true;
		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
			if (arc != ALP_OK) active_set = false;
			printf("[probe] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
		}
	}

	/* --- 12. Three volume-step windows (VOL=4/16/48), tone + acoustic
	 * capture together. `tone_ok` tracks whether EVERY step's tone stayed
	 * healthy -- that is what EVIDENCE 1's DURING read (right after this
	 * loop) needs; `vol_ok[step]` independently tracks whether THAT step's
	 * acoustic data is trustworthy. Both are tracked because a mic hiccup
	 * on one step must not silently invalidate the TDM_CLOCK check, and a
	 * tone-write hiccup must not silently keep a stale acoustic reading. */
	bool tone_ok = active_set;
	for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
		if (!active_set) break;
		alp_status_t vrc = alp_audio_out_set_volume(spk, sound_vol_steps[step]);
		printf("[probe] alp_audio_out_set_volume(%u) [step %zu/%zu] -> %d\n",
		       sound_vol_steps[step],
		       step + 1,
		       (size_t)SOUND_VOL_STEP_COUNT,
		       (int)vrc);
		bool step_tone_ok;
		if (vrc != ALP_OK) {
			step_tone_ok = false;
		} else if (mic_ok) {
			vol_ok[step] =
			    capture_window(mic, spk, tone_buf, &phase_acc, samples_per_cycle, vol_r[step]);
			step_tone_ok = vol_ok[step];
		} else {
			step_tone_ok = play_tone_blocks(spk,
			                                tone_buf,
			                                &phase_acc,
			                                samples_per_cycle,
			                                ACOUSTIC_DISCARD_BLOCKS + ACOUSTIC_ANALYSIS_BLOCKS);
		}
		if (!step_tone_ok) tone_ok = false;
	}

	/* --- 13. EVIDENCE 1, DURING: amps ACTIVE, tone genuinely playing
	 * (right after the loudest volume step, while I2S3 is still running). */
	uint32_t faults_during[AMP_COUNT] = { 0 };
	bool     during_valid             = tone_ok;
	if (during_valid) {
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_clear_faults(&amps[i]);
		}
		k_msleep(CLOCK_CHECK_SETTLE_MS);
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_read_faults(&amps[i], &faults_during[i]);
			printf("[probe] tas2563_read_faults(0x%02x) DURING (ACTIVE, playing) -> "
			       "0x%08x (TDM_CLOCK %s)\n",
			       amp_addrs[i],
			       faults_during[i],
			       (faults_during[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET" : "clear");
		}
	}

	/* --- 14. STOP-CONTROL: stop I2S3 while BOTH amps stay ACTIVE --------- */
	/* alp_audio_out_stop() -> alp_i2s_stop() -> i2s_trigger(...,
	 * I2S_TRIGGER_DRAIN) (src/backends/audio/zephyr_drv.c z_out_stop(),
	 * src/backends/i2s/zephyr_drv.c) -- drains any in-flight block then
	 * halts the DesignWare I2S3 controller, which is what actually stops
	 * SCLK/WS. Safe with both amps ACTIVE: SLASET3D SS7.3.12 documents the
	 * part muting on a TDM clock error rather than free-running the
	 * Class-D stage without one. */
	bool         stream_stop_attempted = false;
	alp_status_t stop_rc               = ALP_ERR_NOT_READY;
	if (during_valid) {
		stream_stop_attempted = true;
		stop_rc               = alp_audio_out_stop(spk);
		printf("[probe] alp_audio_out_stop(I2S3) [stop-control, amps still ACTIVE] -> %d\n",
		       (int)stop_rc);
	}

	/* --- 15. EVIDENCE 1, STOPPED: amps still ACTIVE, I2S3 halted --------- */
	uint32_t faults_stopped[AMP_COUNT] = { 0 };
	bool     stopped_valid             = during_valid && (stop_rc == ALP_OK);
	if (stopped_valid) {
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_clear_faults(&amps[i]);
		}
		k_msleep(CLOCK_CHECK_SETTLE_MS);
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_read_faults(&amps[i], &faults_stopped[i]);
			printf("[probe] tas2563_read_faults(0x%02x) STOPPED (ACTIVE, I2S3 halted) -> "
			       "0x%08x (TDM_CLOCK %s)\n",
			       amp_addrs[i],
			       faults_stopped[i],
			       (faults_stopped[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET" : "clear");
		}
	}

	/* --- 16. Acoustic STOPPED window: mic-only, no tone (I2S3 halted) ---- */
	if (mic_ok && stopped_valid) {
		stopped_ok = capture_window(mic, NULL, tone_buf, &phase_acc, samples_per_cycle, stopped_r);
	}

	/* --- 17. Teardown, mute FIRST -- confirmed SHUTDOWN before anything
	 * else stops (both for EVIDENCE 1's safety contract and so the
	 * acoustic SHUTDOWN window below genuinely observes amps off). ------- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
	}

	/* --- 18. Acoustic SHUTDOWN window: mic-only, amps now off ------------ */
	if (mic_ok) {
		shutdown_ok =
		    capture_window(mic, NULL, tone_buf, &phase_acc, samples_per_cycle, shutdown_r);
	}

	/* --- 19. Rest of teardown -- this is the only remaining exit path. --- */
	if (spk != NULL) {
		if (!stream_stop_attempted) alp_audio_out_stop(spk); /* not yet stopped above. */
		alp_audio_out_close(spk);
	}
	if (mic != NULL) {
		alp_audio_in_stop(mic);
		alp_audio_in_close(mic);
	}
	for (size_t i = 0; i < AMP_COUNT; i++) {
		tas2563_deinit(&amps[i]);
	}
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	/* --- 20. ACOUSTIC MEASUREMENT TABLE ----------------------------------- */
	printf("[probe] === ACOUSTIC MEASUREMENT TABLE ===\n");
	print_acoustic_row("BASELINE", baseline_r, baseline_ok);
	for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
		char label[16];
		snprintf(label, sizeof(label), "VOL=%u", sound_vol_steps[step]);
		print_acoustic_row(label, vol_r[step], vol_ok[step]);
	}
	print_acoustic_row("STOPPED", stopped_r, stopped_ok);
	print_acoustic_row("SHUTDOWN", shutdown_r, shutdown_ok);

	/* --- 21. ACOUSTIC VERDICT ---------------------------------------------- */
	bool windows_valid = baseline_ok && stopped_ok && shutdown_ok;
	for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
		if (!vol_ok[step]) windows_valid = false;
	}

	acoustic_verdict_t av;
	const char        *av_reason = "";
	if (!mic_ok) {
		av        = ACOUSTIC_INCONCLUSIVE;
		av_reason = "PDM mic never opened/started";
	} else if (!windows_valid) {
		av        = ACOUSTIC_INCONCLUSIVE;
		av_reason = "one or more capture windows failed (write/read error) -- see the table above";
	} else {
		composite_db_t base_c          = composite_of(baseline_r);
		composite_db_t stop_c          = composite_of(stopped_r);
		composite_db_t shut_c          = composite_of(shutdown_r);
		float          silent_floor_db = base_c.tone_db;
		if (stop_c.tone_db > silent_floor_db) silent_floor_db = stop_c.tone_db;
		if (shut_c.tone_db > silent_floor_db) silent_floor_db = shut_c.tone_db;

		composite_db_t vol_c[SOUND_VOL_STEP_COUNT];
		int            clear_count  = 0;
		bool           rises        = true;
		bool           exceeds_ref  = true;
		float          prev_tone_db = -1.0e9f;
		for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
			vol_c[step] = composite_of(vol_r[step]);
			if (vol_c[step].tone_db >= silent_floor_db + ACOUSTIC_MARGIN_DB) clear_count++;
			if (!(vol_c[step].tone_db > prev_tone_db)) rises = false;
			prev_tone_db = vol_c[step].tone_db;
			if (!(vol_c[step].tone_db > vol_c[step].ref_low_db &&
			      vol_c[step].tone_db > vol_c[step].ref_high_db)) {
				exceeds_ref = false;
			}
		}
		printf("[probe] ACOUSTIC silent floor (max of BASELINE/STOPPED/SHUTDOWN composite "
		       "tone-bin) = %.1f dB, margin = %.1f dB, %d/%zu volume step(s) cleared\n",
		       (double)silent_floor_db,
		       (double)ACOUSTIC_MARGIN_DB,
		       clear_count,
		       (size_t)SOUND_VOL_STEP_COUNT);

		if (clear_count == 0) {
			av        = ACOUSTIC_TONE_NOT_HEARD;
			av_reason = "no volume step cleared the silent-floor margin";
		} else if (clear_count == (int)SOUND_VOL_STEP_COUNT && rises && exceeds_ref) {
			av = ACOUSTIC_TONE_HEARD;
		} else {
			av = ACOUSTIC_INCONCLUSIVE;
			if (clear_count != (int)SOUND_VOL_STEP_COUNT) {
				av_reason = "not every volume step cleared the margin";
			} else if (!rises) {
				av_reason = "tone-bin power did not rise monotonically with volume";
			} else {
				av_reason = "tone bin did not clearly exceed both off-tone reference bins at "
				            "every step";
			}
		}
	}
	printf("[probe] ACOUSTIC VERDICT: %s%s%s\n",
	       acoustic_verdict_str(av),
	       av_reason[0] != '\0' ? " -- " : "",
	       av_reason);

	/* --- 22. EVIDENCE 1 (TDM_CLOCK) verdict, per amp then combined ------- */
	if (!during_valid) {
		printf("[probe] I2S3/ACTIVE never reached a clean playing state through every volume "
		       "step (open/set_volume/start/write/set_mode rc above) -- cannot run the "
		       "DURING/STOPPED check\n");
		printf("[probe] TDM_CLOCK VERDICT: %s\n", verdict_str(VERDICT_CLOCKS_NOT_REACHING_AMP));
		printf("[probe] done\n");
		return 0;
	}
	if (!stopped_valid) {
		printf("[probe] could not stop the I2S3 stream for the stop-control read (rc=%d)\n",
		       (int)stop_rc);
		printf("[probe] TDM_CLOCK VERDICT: %s\n", verdict_str(VERDICT_INCONCLUSIVE));
		printf("[probe] done\n");
		return 0;
	}

	amp_clock_verdict_t per_amp[AMP_COUNT];
	bool                any_inconclusive = false;
	bool                all_reach        = true;
	bool                all_not_reach    = true;
	for (size_t i = 0; i < AMP_COUNT; i++) {
		per_amp[i] = amp_clock_verdict(faults_during[i], faults_stopped[i]);
		printf("[probe] amp 0x%02x: DURING=0x%08x STOPPED=0x%08x -> %s\n",
		       amp_addrs[i],
		       faults_during[i],
		       faults_stopped[i],
		       per_amp[i] == AMP_CLOCK_REACHES        ? "REACHES"
		       : per_amp[i] == AMP_CLOCK_NOT_REACHING ? "NOT REACHING"
		                                              : "INCONCLUSIVE");
		if (per_amp[i] != AMP_CLOCK_REACHES) all_reach = false;
		if (per_amp[i] != AMP_CLOCK_NOT_REACHING) all_not_reach = false;
		if (per_amp[i] == AMP_CLOCK_INCONCLUSIVE) any_inconclusive = true;
	}

	verdict_t v;
	if (!any_inconclusive && all_reach) {
		v = VERDICT_CLOCKS_REACH_AMP;
	} else if (!any_inconclusive && all_not_reach) {
		v = VERDICT_CLOCKS_NOT_REACHING_AMP;
	} else {
		v = VERDICT_INCONCLUSIVE; /* either amp inconclusive, or the two amps disagree. */
	}
	printf("[probe] TDM_CLOCK VERDICT: %s\n", verdict_str(v));
	printf("[probe] done\n");
	return 0;
}

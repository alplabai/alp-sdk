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
 * (fan, HVAC) at any single "loud" frequency from faking a pass. Plus three
 * coarse, non-frequency-selective sanity numbers per channel -- RMS
 * (accumulated in `double` for precision over ACOUSTIC_ANALYSIS_FRAMES
 * samples, converted to float only at the end), peak |sample|, and mean
 * (DC residual, post the backend's own DC-block filter) -- printed
 * alongside the bins specifically so an idle or under-clocked mic (flat at
 * +-1-2 LSB) is distinguishable from a mic that is genuinely capturing
 * something, just not at 1 kHz: three near-zero Goertzel bins alone cannot
 * tell those two apart, but a peak that never rises above a couple of LSB
 * can. See chan_result_t's own comment for the full reasoning.
 *
 * MIC CAPTURE RATE IS 48 kHz -- neither of the two earlier guesses (16 kHz,
 * then 8 kHz) survived contact with the mic's own datasheet. History, so
 * the next person doesn't repeat either mistake:
 *   - 16 kHz (the original guess): wrong -- see the #2133 bench trace
 *     below, which showed 8 kHz on silicon.
 *   - 8 kHz (PDM_MODE_STANDARD_VOICE_512_CLK_FRQ /
 *     ARM_PDM_MODE_AUDIOFREQ_8K_DECM_64, cross-checked against the Alif DFP
 *     Driver_PDM.h -- the mode the #2133 bench trace actually measured, a
 *     512 kHz PDM bit clock): ALSO wrong for THIS mic. U19/U20 are
 *     MP34DT05TR-A parts; ST's own Zephyr driver for this family
 *     (zephyr/drivers/audio/mpxxdtyy.h:19-20) states
 *     `MPXXDTYY_MIN_PDM_FREQ 1200000` / `MPXXDTYY_MAX_PDM_FREQ 3250000` --
 *     a 1.2-3.25 MHz PDM bit clock. Both the 512 kHz (8 kHz mode) and the
 *     1024 kHz (16 kHz mode) clocks are BELOW that 1.2 MHz floor -- neither
 *     was ever in spec for this mic, on ANY board, regardless of what the
 *     Alif PDM controller was willing to generate or what FIR table the
 *     driver had. The in-spec modes are 32 kHz and 48 kHz (mode 7, a
 *     3072 kHz clock, decimation 64) -- 48 kHz is used here. The #2133 fix
 *     is being changed again to add 48 kHz and to reject the two
 *     out-of-spec 8/16 kHz modes on this board via a devicetree mic clock
 *     range (clk-frequency-min/clk-frequency-max on the board overlay's
 *     pdm@4902d000 node below -- these are Zephyr's own pdm-dmic.yaml
 *     property names, issue #2133 round 4a).
 *   - #2133 BENCH TRACE (kept for the record, not because 8 kHz is used):
 *     testing the #2133 fix against examples/aen/aen-pdm-mic-alif on
 *     e1m-aen-evk-03 showed correct blocks (PDM_CONFIG_REGISTER =
 *     0x00010033, FIFO moving, rc=0) arriving ~200 ms apart for
 *     1600-frame blocks -- 8 kHz, not the 16 kHz this app originally
 *     requested. That is what first disproved the 16 kHz guess; the mic
 *     datasheet then disproved 8 kHz too.
 * MIC_SAMPLE_RATE_HZ is therefore 48000, independent of
 * SOUND_SAMPLE_RATE_HZ (16000, unchanged -- I2S3 TX to the TAS2563 amps is
 * a different peripheral and is unaffected by any of this: the TDM_CLOCK
 * check and every safety property are untouched by the mic's own rate).
 *
 * EXACT-BIN WINDOW SIZING -- a Goertzel bin is only leakage-free when the
 * target frequency lands EXACTLY on an integer DFT bin of the analysis
 * window: k = N * f / fs must be a whole number. With fs = MIC_SAMPLE_RATE_HZ
 * = 48000 Hz and N = ACOUSTIC_ANALYSIS_FRAMES = 19200 (25 blocks of
 * MIC_FRAMES_PER_BLOCK=768 frames, ~400 ms): k(1000 Hz) =
 * 19200*1000/48000 = 400, k(800 Hz) = 19200*800/48000 = 320, k(1200 Hz) =
 * 19200*1200/48000 = 480 -- all three exact integers, verified by hand
 * here (not just "computed at runtime and hoped"), and numerically
 * IDENTICAL to the k values at both earlier (wrong-rate) window sizes:
 * k = N/fs * f = (window duration in seconds) * f, and the window
 * duration (0.4 s) has never changed across any of the three rates --
 * only fs and N move together, in the same proportion.
 *
 * WINDOW SEQUENCE, in order. EVIDENCE 1's DURING/STOPPED check is now ITS
 * OWN segment (run_tdm_clock_check()), run entirely before any acoustic
 * window and entirely before the mic is ever interleaved with a tone write
 * -- see "TONE-ON BUDGET, RESTATED" below for why that changed:
 *   1. BASELINE  -- amps SHUTDOWN, I2S3 not yet opened. Ambient.
 *   2. TDM_CLOCK check -- amps ACTIVE, tone-only (no mic): prime
 *      (TDM_CHECK_PRIME_BLOCKS), clear latches, settle
 *      (TDM_CHECK_SETTLE_BLOCKS, still writing tone), read = DURING; drain
 *      (alp_audio_out_stop()), clear, k_msleep(CLOCK_CHECK_SETTLE_MS)
 *      (nothing to starve -- the stream is genuinely stopped), read =
 *      STOPPED.
 *   3. VOL=4, VOL=16, VOL=48 -- SKIPPED ENTIRELY once the mic is known dead
 *      (mic_dead -- true from step 1 on this driver as of #2133). When the
 *      mic is alive: the stream is explicitly restarted (deferred-start
 *      re-arms on the next write), then each step plays the tone at that
 *      digital-volume level while capturing acoustically.
 *   4. STOPPED (acoustic) -- I2S3 already drained by step 2's stop-control;
 *      mic-only, no tone.
 *   5. SHUTDOWN (acoustic) -- both amps back in TAS2563_MODE_SHUTDOWN, no
 *      tone.
 *
 * TONE-ON BUDGET, RESTATED (bench-review fix): the PREVIOUS version reused
 * the loudest acoustic volume window's tone-on time for the DURING read,
 * which meant EVIDENCE 1 could only ever be as good as the mic's own
 * timing that run -- and on real silicon it was worse than that: each
 * acoustic window's alp_audio_in_read() blocked up to MIC_READ_TIMEOUT_MS
 * with NOTHING feeding the 2-block I2S TX slab in between, draining it and
 * failing the VERY NEXT alp_audio_out_write() -- so EVIDENCE 1 read
 * "clocks not reaching" for a reason that had nothing to do with clocks.
 * The TDM_CLOCK check's own tone-on cost is now fixed and small:
 * SOUND_PREACTIVE_BLOCKS (2 blocks, ~32 ms, the quiet pre-ACTIVE priming --
 * see HARDWARE SAFETY) + TDM_CHECK_PRIME_BLOCKS (6, ~96 ms) +
 * TDM_CHECK_SETTLE_BLOCKS (2, ~32 ms) = 10 blocks, ~160 ms -- independent
 * of the mic entirely. The three acoustic volume windows, IF they run
 * (mic alive), add 3 * (ACOUSTIC_DISCARD_BLOCKS + ACOUSTIC_ANALYSIS_BLOCKS)
 * blocks (~1488 ms) on top, for ~1.65 s total -- still comfortably under a
 * 5 s budget. In the current (mic known dead from BASELINE) state, total
 * tone-on time is just the ~160 ms TDM-check cost: the acoustic windows are
 * skipped rather than played to nobody.
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
 * FIRST-SILICON BENCH RESULT + A REAL SDK-LEVEL BUG (NOT FIXED IN THIS APP)
 * ============================================================================
 * The first attended run on e1m-aen-evk-03 (image e72fec2ed) had EVIDENCE 1
 * pass cleanly (I2S3 opened/started, both amps went ACTIVE, no faults, clean
 * teardown) but EVERY acoustic window -- including BASELINE, no tone, amps
 * still SHUTDOWN -- failed with alp_audio_in_read() timing out at almost
 * exactly the (then-)200 ms MIC_READ_TIMEOUT_MS. Two things about that
 * pointed at the mic path itself, not the tone: BASELINE has no tone in
 * flight at all, and the failure latency matched the timeout, not an
 * instant NACK. capture_window() below now self-diagnoses this (FIX 3) --
 * see dump_pdm_diagnostics() -- and this run's earlier bug (FIX 1/2) meant
 * that mic failure was ALSO wrongly making EVIDENCE 1 print "clocks not
 * reaching" despite EVIDENCE 1 never having been affected by it.
 *
 * Investigating against examples/aen/aen-pdm-mic-alif (bench-verified,
 * docs/test-plan.md:310 "Live varying PCM captured") found TWO real gaps in
 * `<alp/audio.h>`'s Zephyr audio-in backend (src/backends/audio/
 * zephyr_drv.c) against the SAME alif,alif-pdm driver -- neither
 * fixable from this app, since alp_audio_in_open() exposes no channel-FIR
 * or clock-mode control:
 *
 *   1. NO CHANNEL/CLOCK-MODE PROGRAMMING AT ALL. aen-pdm-mic-alif's own file
 *      header states plainly: "The driver leaves the PDM clock-mode field at
 *      reset (MICROPHONE_SLEEP) until the app calls pdm_mode(), and never
 *      programs the FIR/gain -- so configuring each enabled channel +
 *      selecting a non-sleep mode here is what makes the FIFO actually
 *      fill" (examples/aen/aen-pdm-mic-alif/src/main.c, pdm_config_channel()
 *      + the pdm_mode(dmic, PDM_MODE_STANDARD_VOICE_512_CLK_FRQ) call in
 *      main()). z_in_open() (src/backends/audio/zephyr_drv.c:221-296) calls
 *      ONLY dmic_configure() + (in z_in_start()) dmic_trigger(START) --
 *      never pdm_channel_config()/pdm_set_ch_*()/pdm_mode(). Per the driver
 *      comment above, that leaves the PDM block in MICROPHONE_SLEEP, FIFO
 *      permanently empty, every read timing out -- independent of tone,
 *      volume, or anything else this app does. This alone explains the
 *      observed BASELINE failure.
 *
 *   2. THE CHANNEL MAP IS LIKELY WRONG TOO. zephyr_drv.c:287-290 builds
 *      `req_chan_map_lo` with the GENERIC Zephyr `dmic_build_channel_map()`
 *      nibble encoding (`dmic_build_channel_map(0,0,PDM_CHAN_LEFT) |
 *      dmic_build_channel_map(1,0,PDM_CHAN_RIGHT)` = 0x10 for 2 channels --
 *      zephyr/include/zephyr/audio/dmic.h:238-243). But
 *      dmic_alif_pdm_configure() (zephyr/drivers/audio/alif_pdm.c:125,148)
 *      does `pdata->channel_map = config->channel.req_chan_map_lo & 0xFF`
 *      -- it takes the byte VERBATIM as a raw per-bit HARDWARE
 *      channel-enable mask (aen-pdm-mic-alif's own comment says so too: "The
 *      alif_pdm driver takes req_chan_map_lo's low byte VERBATIM as the PDM
 *      hardware channel-enable mask... NOT the dmic_build_channel_map()
 *      nibble encoding"), where bit N = PDM_MASK_CHANNEL_N (zephyr/include/
 *      zephyr/drivers/pdm/pdm_alif.h:39-40). 0x10 = bit 4 = PDM_MASK_CHANNEL_4
 *      -- so even if (1) were fixed, this app's 2-channel open would tell
 *      the driver to enable HW channel 4, NOT channels 0/1 (U19/U20, this
 *      board's actual D0-pair mics) -- and channel 4's pads (P11_4/P5_4,
 *      PDM_C2_B/PDM_D2_B) are not even pinmuxed by this app's overlay (see
 *      dump_pdm_diagnostics()'s P11_4/P5_4 dump, included specifically to
 *      show those pads sitting unconfigured). aen-pdm-mic-alif sidesteps
 *      this entirely by building `req_chan_map_lo` itself from
 *      `PDM_MASK_CHANNEL_*` directly, bypassing `dmic_build_channel_map()`.
 *
 * Both are SDK-level bugs in the shared backend, worth their own issue(s)
 * against src/backends/audio/zephyr_drv.c -- reported here, not papered
 * over by this app; filed as #2133, fix in progress. Until it lands,
 * acoustic capture through `<alp/audio.h>` on this driver is expected to
 * keep failing regardless of any tuning at this app's level.
 *
 * SECOND FINDING, SAME BENCH RUN: EVIDENCE 1 was NOT actually unaffected,
 * as an earlier version of this comment claimed. The dead mic's blocking
 * alp_audio_in_read() (up to MIC_READ_TIMEOUT_MS, then MIC_READ_TIMEOUT_MS
 * again for the next window, and so on) starved the 2-block I2S TX slab
 * between tone writes and broke the very next write, which this app's OWN
 * verdict logic then misread as "DURING never happened" -- see the file
 * header's "TONE-ON BUDGET, RESTATED" section and run_tdm_clock_check()
 * for the fix: EVIDENCE 1 now runs as its own tone-only segment that never
 * touches the mic at all, so it is genuinely independent this time.
 *
 * THIRD FINDING, LATER RUN (rebuilt against #2137 + #2133 round 4a/4b): a
 * TX underrun/RX overrun used to leave Zephyr's DW I2S driver stuck in
 * I2S_STATE_ERROR forever (zephyr/drivers/i2s/i2s_dw.c) -- once the acoustic
 * windows stopped feeding it (mic already dead that run), the very next
 * alp_audio_out_stop()/alp_audio_out_start() both returned -5, so
 * run_tdm_clock_check() could never reach STOPPED even though DURING had
 * read cleanly. Fixed in the shared I2S backend, not this app (#2137):
 * PREPARE now recovers I2S_STATE_ERROR on both stop() and start(). Separately,
 * "48 kHz measured ~32 kHz" (the "MIC CAPTURE RATE IS 48 kHz" section above)
 * turned out to be the CONSUMER, not the clock: chan_stats-style
 * double-precision math on every sample, with no CONFIG_FPU, ran slower than
 * the 100 ms block period and starved the mic's slab. #2133 round 4b makes
 * dmic_alif_pdm_read() return -EIO (sticky until DMIC_TRIGGER_START) instead
 * of silently splicing the next block onto a drop, and capture_window()
 * below now (a) does zero per-sample work in its read loop -- Goertzel/RMS/
 * peak/dc all run once, after the window's reads are done -- and (b) treats
 * -EIO as a per-window DROPPED event (STOP/START, try again next window),
 * keeping it separate from the run-wide mic_dead latch that a genuine
 * failure sets.
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
 *   - Short tone-on time in total -- see the "TONE-ON BUDGET, RESTATED"
 *     note above (~160 ms with the mic dead, as it currently is; ~1.65 s
 *     if the acoustic windows also run), and teardown always mutes
 *     (tas2563_set_mode(SHUTDOWN)) BOTH amps
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
#include <zephyr/sys/sys_io.h>
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

/* ---- PDM mics (U19 LEFT / U20 RIGHT) -- see the file header's MIC MAPPING
 * and MIC CAPTURE RATE IS 48 kHz sections. */
#define MIC_CHANNELS 2u /* ch0=U19 LEFT, ch1=U20 RIGHT. */
/* 48000 Hz, NOT SOUND_SAMPLE_RATE_HZ (16000, the SEPARATE I2S3 TX rate).
 * The ONLY in-spec rate for the MP34DT05TR-A's 1.2-3.25 MHz PDM clock
 * range (ST's mpxxdtyy.h) that this driver/board combination offers --
 * 8 kHz and 16 kHz are both out of spec for this mic regardless of driver
 * support; see the file header for the two-wrong-guesses history. */
#define MIC_SAMPLE_RATE_HZ 48000u
/* 768 frames at 48 kHz = 16 ms/block -- deliberately the SAME wall-clock
 * block period as SOUND_FRAMES_PER_BLOCK (256 @ 16 kHz = 16 ms) for the
 * I2S TX side, NOT the same frame count. capture_window() writes one TX
 * tone block and attempts one mic-read block per loop iteration; keeping
 * both at 16 ms/block is what keeps that interleave balanced -- a mic
 * block LONGER than the TX block's duration would mean each mic read
 * blocks longer than the 2-block TX slab can cover from a single write,
 * reintroducing the exact underrun class the TDM check was moved out from
 * under (see run_tdm_clock_check() and the file header). The BUILD_ASSERT
 * below makes that equal-period promise a compile-time fact instead of an
 * eyeballed comment, so a future rate change on either side cannot desync
 * them silently. */
#define MIC_FRAMES_PER_BLOCK 768u
BUILD_ASSERT(MIC_FRAMES_PER_BLOCK *SOUND_SAMPLE_RATE_HZ ==
                 SOUND_FRAMES_PER_BLOCK * MIC_SAMPLE_RATE_HZ,
             "mic and I2S TX block periods must match (cross-multiplied to dodge fractional ms): "
             "768*16000 == 256*48000 == 12288000");
/* 768 frames * MIC_CHANNELS(2) * sizeof(int16_t) = 3072 B per block --
 * checked against CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES (src/backends/audio/
 * zephyr_drv.c, default 4096, not overridden in this app's prj.conf): fits
 * with headroom. If MIC_CHANNELS ever grows (e.g. capturing the second mic
 * pair, U25/U26 -- which the 2-channel backend cap still prevents, see the
 * file header), re-check this against CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES
 * before raising it. The per-handle slab that actually backs this
 * (g_in_be_pool[].slab_buf[], src/backends/audio/zephyr_drv.c) is a
 * STATIC array sized CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES *
 * CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS (default 4) = 16 KiB regardless of
 * this app's actual per-block byte count, so this rate change costs this
 * app NO additional heap or slab RAM -- CONFIG_HEAP_MEM_POOL_SIZE and
 * CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS are unchanged in prj.conf. */
BUILD_ASSERT((MIC_FRAMES_PER_BLOCK * MIC_CHANNELS * (uint32_t)sizeof(int16_t)) <= 4096u,
             "mic block bytes must fit CONFIG_ALP_SDK_AUDIO_BLOCK_BYTES's default (4096)");
/* Aligned to examples/aen/aen-pdm-mic-alif's bench-verified READ_TIMEOUT_MS
 * (2000) -- was 200 here, which removed "our timeout was just too short" as
 * a candidate explanation for the first-silicon-run mic read failure
 * (capture_window() below stops retrying a window's mic reads after the
 * FIRST failure, so the worst-case extra wall time this adds is bounded to
 * roughly one timeout per window that ever fails, not per block). */
#define MIC_READ_TIMEOUT_MS 2000u

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
 * covers the DC-blocker's own time constant with wide margin (alpha =
 * 0.995 in dc_block_s16(), src/backends/audio/zephyr_drv.c -- time
 * constant ~= 1/(1-0.995) = 200 samples = 4.17 ms at the mic's 48 kHz
 * capture rate, so 96 ms is ~23x that); nothing more rigorous than that
 * informed the choice of 6, and 6 is unchanged from the two earlier
 * (wrong-rate) versions of this app -- only the margin it buys changed as
 * the mic rate moved: ~7.7x at the original 16 kHz guess, ~4x at the
 * 8 kHz guess, ~23x at the actual 48 kHz rate. */
#define ACOUSTIC_DISCARD_BLOCKS  6u
#define ACOUSTIC_ANALYSIS_BLOCKS 25u /* ~400ms, 25*768=19200 samples -- the exact-bin window. */
#define ACOUSTIC_ANALYSIS_FRAMES (ACOUSTIC_ANALYSIS_BLOCKS * MIC_FRAMES_PER_BLOCK)
#define TONE_BIN_HZ              SOUND_TONE_HZ /* 1000 Hz, exact bin k=400 at N=19200/fs=48000. */
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

/* ================================================================== */
/* PDM/clock/pinmux diagnostic register dump (bench issue: mic read timeout) */
/* ================================================================== */
/* Printed ONCE, read-only, on the FIRST alp_audio_in_read() failure this run
 * -- docs/aen-bench-bringup.md:597 names exactly this symptom: PDM
 * `dmic_read` -> `-EAGAIN` (FIFO=0). See capture_window() below for the call
 * site. All addresses are cited, not invented:
 *
 * PDM block registers -- zephyr/drivers/audio/alif_pdm_reg.h:21-24,28. Base
 * 0x4902d000 is this app's own pdm@4902d000 node (board overlay).
 *
 * Audio clock enables the Tier-1.5 clockctrl patch claims to set on
 * clock_control_on() -- zephyr/drivers/clock_control/clock_control_alif.c:
 * 132-190 (ALIF_CGU_CLK_ENA_REG_OFF=0x14/ALIF_CGU_CLK76P8M_BIT=24,
 * ALIF_EXPMST0_CTRL_REG_OFF=0x00/IPCLK_FORCE=bit31/PCLK_FORCE=bit30),
 * docs/aen-bench-bringup.md:568-571. Bases (cgu=0x1a602000,
 * clkctl_per_slv=0x4902f000) read off THIS build's own resolved devicetree
 * (the build directory's zephyr/zephyr.dts: clock-controller@1a602000,
 * reg-names "cgu"/"clkctl_per_slv" -- sourced from zephyr/dts/arm/alif/
 * ensemble/common/ensemble_common.dtsi:41-50 in ZEPHYR_BASE, not this
 * repo's own zephyr/ directory).
 *
 * Pinmux registers: base 0x1a603000 (the `pinctrl` node,
 * pin-controller@1a603000), offset = port*32 + pin*4 -- formula AND base
 * both read from zephyr/drivers/pinctrl/pinctrl_alif.c:38,42-43,51-57
 * (ALIF_PINCTRL_BASE, ALIF_PORT_REG_SIZE=32, ALIF_PINMUX_REG_SIZE=4),
 * cross-checked against examples/aen/aen-sdhc-probe's own
 * SD_PINMUX_P14_1_CLK=0x1A6031C4 (port14*32 + pin1*4 + 0x1a603000 =
 * 0x1a6031c4 -- matches). Padcfg bits[23:16], read-enable = bit 16, same
 * layout that SD diagnostic used. P6_1/P6_0 are this app's actual pads
 * (PDM_C0_C/PDM_D0_C); P11_4/P5_4 (PDM_C2_B/PDM_D2_B) are dumped too even
 * though this app's overlay never muxes them -- see the CHANNEL-MAP SDK BUG
 * note below for why that is directly relevant evidence, not idle curiosity.
 */
#define PDM_DIAG_PDM_BASE           0x4902d000u
#define PDM_DIAG_CONFIG_REG         (PDM_DIAG_PDM_BASE + 0x0u)
#define PDM_DIAG_CTL_REG            (PDM_DIAG_PDM_BASE + 0x4u)
#define PDM_DIAG_THRESHOLD_REG      (PDM_DIAG_PDM_BASE + 0x8u)
#define PDM_DIAG_FIFO_STATUS_REG    (PDM_DIAG_PDM_BASE + 0xCu)
#define PDM_DIAG_INTERRUPT_REG      (PDM_DIAG_PDM_BASE + 0x1Cu)
#define PDM_DIAG_FIFO_STAT_CNT_MASK 0xFu

#define PDM_DIAG_CGU_BASE            0x1a602000u
#define PDM_DIAG_CGU_CLK_ENA_REG     (PDM_DIAG_CGU_BASE + 0x14u)
#define PDM_DIAG_CGU_CLK76P8M_BIT    24u
#define PDM_DIAG_CLKCTL_PER_SLV_BASE 0x4902f000u
#define PDM_DIAG_EXPMST0_CTRL_REG    (PDM_DIAG_CLKCTL_PER_SLV_BASE + 0x0u)
#define PDM_DIAG_EXPMST0_IPCLK_FORCE (1u << 31)
#define PDM_DIAG_EXPMST0_PCLK_FORCE  (1u << 30)

#define PDM_DIAG_PINMUX_BASE 0x1a603000u
#define PDM_DIAG_PINMUX(port, pin) \
	(PDM_DIAG_PINMUX_BASE + (uint32_t)(port) * 32u + (uint32_t)(pin) * 4u)
#define PDM_DIAG_P6_1_CLK         PDM_DIAG_PINMUX(6, 1)  /* PDM_C0_C -- this app's clock pad. */
#define PDM_DIAG_P6_0_DATA        PDM_DIAG_PINMUX(6, 0)  /* PDM_D0_C -- this app's data pad. */
#define PDM_DIAG_P11_4_CLK2       PDM_DIAG_PINMUX(11, 4) /* PDM_C2_B -- NOT muxed by this app. */
#define PDM_DIAG_P5_4_DATA2       PDM_DIAG_PINMUX(5, 4)  /* PDM_D2_B -- NOT muxed by this app. */
#define PDM_DIAG_PADCFG_POS       16u
#define PDM_DIAG_PADCFG_MASK      (0xFFu << PDM_DIAG_PADCFG_POS)
#define PDM_DIAG_READ_ENABLE_MASK (1u << PDM_DIAG_PADCFG_POS)

static void pdm_diag_print_pinmux(const char *label, uint32_t addr)
{
	uint32_t v = sys_read32(addr);
	printf("[probe][diag] %s pinmux @0x%08x = 0x%08x  padcfg=0x%02x read-enable=%u\n",
	       label,
	       addr,
	       v,
	       (unsigned)((v & PDM_DIAG_PADCFG_MASK) >> PDM_DIAG_PADCFG_POS),
	       (unsigned)((v & PDM_DIAG_READ_ENABLE_MASK) != 0u));
}

static bool s_pdm_diag_dumped;

static void dump_pdm_diagnostics(void)
{
	if (s_pdm_diag_dumped) return;
	s_pdm_diag_dumped = true;

	printf("[probe][diag] === PDM/clock/pinmux register dump (first mic read failure) ===\n");
	printf("[probe][diag] docs/aen-bench-bringup.md:597 symptom: PDM dmic_read -> -EAGAIN "
	       "(FIFO=0)\n");

	uint32_t cfg  = sys_read32(PDM_DIAG_CONFIG_REG);
	uint32_t ctl  = sys_read32(PDM_DIAG_CTL_REG);
	uint32_t thr  = sys_read32(PDM_DIAG_THRESHOLD_REG);
	uint32_t fifo = sys_read32(PDM_DIAG_FIFO_STATUS_REG);
	uint32_t irq  = sys_read32(PDM_DIAG_INTERRUPT_REG);
	printf("[probe][diag] PDM_CONFIG_REGISTER      @0x%08x = 0x%08x\n", PDM_DIAG_CONFIG_REG, cfg);
	printf("[probe][diag] PDM_CTL_REGISTER         @0x%08x = 0x%08x\n", PDM_DIAG_CTL_REG, ctl);
	printf(
	    "[probe][diag] PDM_THRESHOLD_REGISTER   @0x%08x = 0x%08x\n", PDM_DIAG_THRESHOLD_REG, thr);
	printf("[probe][diag] PDM_FIFO_STATUS_REGISTER @0x%08x = 0x%08x  (FIFO count bits[3:0]=%u)\n",
	       PDM_DIAG_FIFO_STATUS_REG,
	       fifo,
	       (unsigned)(fifo & PDM_DIAG_FIFO_STAT_CNT_MASK));
	printf(
	    "[probe][diag] PDM_INTERRUPT_REGISTER   @0x%08x = 0x%08x\n", PDM_DIAG_INTERRUPT_REG, irq);

	uint32_t cgu     = sys_read32(PDM_DIAG_CGU_CLK_ENA_REG);
	uint32_t expmst0 = sys_read32(PDM_DIAG_EXPMST0_CTRL_REG);
	printf("[probe][diag] CGU CLK_ENA    @0x%08x = 0x%08x  76.8MHz/HFOSCx2(bit24)=%u\n",
	       PDM_DIAG_CGU_CLK_ENA_REG,
	       cgu,
	       (unsigned)((cgu >> PDM_DIAG_CGU_CLK76P8M_BIT) & 1u));
	printf("[probe][diag] EXPMST0_CTRL   @0x%08x = 0x%08x  IPCLK_FORCE(bit31)=%u "
	       "PCLK_FORCE(bit30)=%u\n",
	       PDM_DIAG_EXPMST0_CTRL_REG,
	       expmst0,
	       (unsigned)((expmst0 & PDM_DIAG_EXPMST0_IPCLK_FORCE) != 0u),
	       (unsigned)((expmst0 & PDM_DIAG_EXPMST0_PCLK_FORCE) != 0u));

	pdm_diag_print_pinmux("P6_1 (PDM_C0_C, clock, muxed)", PDM_DIAG_P6_1_CLK);
	pdm_diag_print_pinmux("P6_0 (PDM_D0_C, data, muxed)", PDM_DIAG_P6_0_DATA);
	pdm_diag_print_pinmux("P11_4 (PDM_C2_B, clock2, NOT muxed)", PDM_DIAG_P11_4_CLK2);
	pdm_diag_print_pinmux("P5_4 (PDM_D2_B, data2, NOT muxed)", PDM_DIAG_P5_4_DATA2);
}

/* One tone block: a square wave, identical samples on both channels (both
 * speakers play the same tone, not a stereo mix -- see amp_rx_channel[]'s
 * comment on why BOTH channels need real, non-zero samples). Advances
 * *phase_acc and blocks in alp_audio_out_write() for real wall-clock time.
 * Returns the write's own alp_status_t -- NOT collapsed to a bool -- so
 * every call site can log the real rc at the point of failure (bench
 * review finding: an earlier version threw the rc away here, and the only
 * evidence of a real underrun was a bare "write FAILED" with no code). */
static alp_status_t write_one_tone_block(alp_audio_out_t *spk,
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
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* Plain tone-only pacing loop -- used for the pre-ACTIVE priming blocks
 * (step 10 in main()), which have no mic involved at all by construction.
 * capture_window() below handles the "mic present but failing" case itself;
 * this helper is for the simpler "no mic in the picture yet" case. */
static bool play_tone_blocks(alp_audio_out_t *spk,
                             int16_t         *buf,
                             uint32_t        *phase_acc,
                             uint32_t         samples_per_cycle,
                             unsigned         count)
{
	for (unsigned b = 0; b < count; b++) {
		alp_status_t wrc = write_one_tone_block(spk, buf, phase_acc, samples_per_cycle);
		if (wrc != ALP_OK) {
			printf("[probe] play_tone_blocks: alp_audio_out_write FAILED (block %u/%u) rc=%d\n",
			       b,
			       count,
			       (int)wrc);
			return false;
		}
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
 * below) for the tone bin + both reference bins, plus three coarse,
 * non-frequency-selective sanity numbers printed alongside the bins:
 *   rms  -- overall signal amplitude.
 *   peak -- max |sample| observed this window (post the backend's own
 *           DC-block filter). An idle or under-clocked mic reads flat at
 *           +-1-2 LSB regardless of what the Goertzel bins say (three bins
 *           of near-zero noise can still look "quiet" without this), so
 *           peak is what actually distinguishes "no real signal reached
 *           the ADC" from "signal reached it but wasn't at 1 kHz".
 *   dc   -- mean sample value this window. The backend's DC-block filter
 *           (dc_block_s16(), src/backends/audio/zephyr_drv.c) already runs
 *           on every block before capture_window() ever sees it, so this
 *           is a RESIDUAL, expected near zero -- printed as a filter
 *           sanity check, not a raw mic DC-offset measurement. */
typedef struct {
	float tone_power;
	float ref_low_power;
	float ref_high_power;
	float rms;
	float peak;
	float dc;
} chan_result_t;

/* capture_window()'s two outcomes, tracked INDEPENDENTLY: a mic failure must
 * never silently invalidate the tone/EVIDENCE-1 side, and a tone-write
 * failure must never silently keep stale acoustic data. Fixes a real bug: an
 * earlier version returned one bool for both, so a mic read timeout (the
 * silicon failure this app actually hit) made the caller believe the TONE
 * had also failed and print "I2S clocks not reaching the amp" -- a claim
 * nothing had measured. See the file header's EVIDENCE 1 / FIX 1-2 notes.
 *
 * mic_dropped/mic_fail_rc (issue #2133 round 4b): dmic_alif_pdm_read() now
 * returns -EIO, sticky until the next DMIC_TRIGGER_START, when the ISR
 * silently dropped a burst (slab-alloc miss or a full delivery queue) --
 * ZEPHYR_BASE zephyr/drivers/audio/alif_pdm.c divergence (5). That is a
 * RECOVERABLE per-window event, not proof the mic is dead: the caller
 * (run_acoustic_window() below) tells the two apart via mic_dropped and
 * only STOP/STARTs the mic on a dropped burst, reserving the mic_dead
 * run-wide latch for every OTHER read failure. mic_fail_rc is the
 * triggering alp_status_t, valid iff !mic_ok, so the caller can print it
 * instead of a bare unexplained latch. */
typedef struct {
	bool         tone_ok;     /* true if spk == NULL, or every tone write in this window ok. */
	bool         mic_ok;      /* true if mic == NULL, or every mic read succeeded, full block. */
	bool         mic_dropped; /* true iff !mic_ok because of a recoverable dropped burst (-EIO). */
	alp_status_t mic_fail_rc; /* the triggering alp_audio_in_read() rc; valid iff !mic_ok. */
} window_status_t;

/*
 * Run one capture window: discard ACOUSTIC_DISCARD_BLOCKS blocks (settle),
 * then feed ACOUSTIC_ANALYSIS_BLOCKS more into three Goertzel bins + an RMS
 * accumulator PER CHANNEL. If spk is non-NULL, writes one tone block before
 * each mic read (both discard and analysis phases), so the read that
 * follows samples genuinely-playing audio -- the same write-then-read
 * interleave examples/aen/aen-evk-demo's phase 11 uses. If spk is NULL, no
 * tone is written (BASELINE/STOPPED/SHUTDOWN). If mic is NULL, no mic read
 * is attempted (the PDM mic never opened/started) -- the tone, if any,
 * still gets written and paced.
 *
 * The tone-write and mic-read streams are handled INDEPENDENTLY within a
 * window: once EITHER one fails (write error, read error/timeout, or a
 * short read -- the fixed-N Goertzel bin math needs a full block), this
 * function stops attempting THAT stream for the rest of the window (no
 * point retrying a NACK 25 more times) but keeps attempting the OTHER one,
 * so a broken mic never truncates the tone this window's EVIDENCE 1 read
 * may depend on, and a tone glitch never aborts an otherwise-working mic
 * capture. See window_status_t above for how the caller reads the result.
 * @p results is only written when the mic stream ran to completion
 * (returned .mic_ok == true); the caller must not read it otherwise.
 *
 * On the FIRST mic read failure THIS RUN (across every call to this
 * function), prints which call failed, its rc, and frames actually
 * received, then dumps the PDM/clock/pinmux diagnostic registers once
 * (dump_pdm_diagnostics() above).
 */
/* rrc/got -> window_status_t's mic fields, shared by both read call sites
 * below: -EIO is a recoverable dropped burst (mic_dropped), anything else
 * (a real error, or a short read) is a genuine failure that gets the
 * register dump -- see window_status_t's comment. */
static void note_mic_read_failure(window_status_t *st,
                                  const char      *phase,
                                  unsigned         b,
                                  alp_status_t     rrc,
                                  size_t           got)
{
	st->mic_ok      = false;
	st->mic_fail_rc = rrc;
	st->mic_dropped = (rrc == ALP_ERR_IO);
	printf("[probe] capture_window: alp_audio_in_read %s (%s block %u) rc=%d got=%zu/%u frames\n",
	       st->mic_dropped ? "reports a DROPPED burst (-EIO, sticky until STOP/START)" : "FAILED",
	       phase,
	       b,
	       (int)rrc,
	       got,
	       (unsigned)MIC_FRAMES_PER_BLOCK);
	if (!st->mic_dropped) dump_pdm_diagnostics(); /* a genuine failure is the one worth a dump. */
}

/* One window's raw mic samples, filled by the ANALYSIS-phase read loop
 * below with NO per-sample work at all (issue #2133 round 4b root cause:
 * ~150 ms of double-precision Goertzel/Welford math per 100 ms block, with
 * no CONFIG_FPU, starved the 4-block slab between reads). Goertzel/RMS/
 * peak/dc all run ONCE, after every block in the window has been read --
 * see the file header's "MAKE THE MIC CONSUMER CHEAP" note. static: off
 * capture_window()'s stack (76 800 bytes). */
static int16_t s_analysis_buf[ACOUSTIC_ANALYSIS_FRAMES * MIC_CHANNELS];

static window_status_t capture_window(alp_audio_in_t  *mic,
                                      alp_audio_out_t *spk,
                                      int16_t         *tone_buf,
                                      uint32_t        *phase_acc,
                                      uint32_t         samples_per_cycle,
                                      chan_result_t    results[MIC_CHANNELS])
{
	static int16_t  discard_buf[MIC_FRAMES_PER_BLOCK * MIC_CHANNELS]; /* discard phase only. */
	window_status_t st = { .tone_ok = true, .mic_ok = (mic != NULL) };

	if (mic == NULL && spk == NULL) return st; /* nothing to write or read -- a true no-op call. */

	for (unsigned b = 0; b < ACOUSTIC_DISCARD_BLOCKS; b++) {
		if (spk != NULL && st.tone_ok) {
			alp_status_t wrc = write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle);
			if (wrc != ALP_OK) {
				st.tone_ok = false;
				printf("[probe] capture_window: alp_audio_out_write FAILED (discard block %u) "
				       "rc=%d\n",
				       b,
				       (int)wrc);
			}
		}
		if (mic != NULL && st.mic_ok) {
			size_t       got = 0;
			alp_status_t rrc = alp_audio_in_read(
			    mic, discard_buf, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
			if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
				note_mic_read_failure(&st, "discard", b, rrc, got);
			}
		}
	}

	/* ANALYSIS phase: read directly into s_analysis_buf, no per-sample work
	 * in this loop at all -- see s_analysis_buf's comment above. */
	for (unsigned b = 0; b < ACOUSTIC_ANALYSIS_BLOCKS; b++) {
		if (spk != NULL && st.tone_ok) {
			alp_status_t wrc = write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle);
			if (wrc != ALP_OK) {
				st.tone_ok = false;
				printf("[probe] capture_window: alp_audio_out_write FAILED (analysis block %u) "
				       "rc=%d\n",
				       b,
				       (int)wrc);
			}
		}
		if (mic != NULL && st.mic_ok) {
			size_t       got = 0;
			int16_t     *dst = &s_analysis_buf[b * MIC_FRAMES_PER_BLOCK * MIC_CHANNELS];
			alp_status_t rrc =
			    alp_audio_in_read(mic, dst, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
			if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
				note_mic_read_failure(&st, "analysis", b, rrc, got);
			}
		}
	}

	/* Post-window analysis: one pass over the whole buffer, entirely after
	 * every alp_audio_in_read() above has returned -- it can no longer be
	 * the reason a read starves the slab. sumsq/dcsum/peak are integer
	 * (int64/int32) here purely because this pass has no reason to touch
	 * an FPU either; Goertzel itself still needs float (cosf/its running
	 * state), same as before. */
	if (mic != NULL && st.mic_ok) {
		goertzel_t tone[MIC_CHANNELS];
		goertzel_t ref_lo[MIC_CHANNELS];
		goertzel_t ref_hi[MIC_CHANNELS];
		int64_t    sumsq[MIC_CHANNELS] = { 0 }; /* -> rms */
		int64_t    dcsum[MIC_CHANNELS] = { 0 }; /* -> dc (residual, see chan_result_t's comment) */
		int32_t    peak[MIC_CHANNELS]  = { 0 }; /* -> peak, max |sample| this window */
		for (size_t c = 0; c < MIC_CHANNELS; c++) {
			goertzel_reset(&tone[c], TONE_BIN_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
			goertzel_reset(
			    &ref_lo[c], REF_BIN_LOW_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
			goertzel_reset(
			    &ref_hi[c], REF_BIN_HIGH_HZ, ACOUSTIC_ANALYSIS_FRAMES, MIC_SAMPLE_RATE_HZ);
		}
		for (size_t f = 0; f < ACOUSTIC_ANALYSIS_FRAMES; f++) {
			for (size_t c = 0; c < MIC_CHANNELS; c++) {
				int16_t x  = s_analysis_buf[f * MIC_CHANNELS + c];
				int32_t ax = (x < 0) ? -(int32_t)x : (int32_t)x;
				goertzel_step(&tone[c], (float)x);
				goertzel_step(&ref_lo[c], (float)x);
				goertzel_step(&ref_hi[c], (float)x);
				sumsq[c] += (int64_t)x * (int64_t)x;
				dcsum[c] += x;
				if (ax > peak[c]) peak[c] = ax;
			}
		}
		for (size_t c = 0; c < MIC_CHANNELS; c++) {
			results[c].tone_power     = goertzel_power(&tone[c]);
			results[c].ref_low_power  = goertzel_power(&ref_lo[c]);
			results[c].ref_high_power = goertzel_power(&ref_hi[c]);
			results[c].rms            = sqrtf((float)sumsq[c] / (float)ACOUSTIC_ANALYSIS_FRAMES);
			results[c].peak           = (float)peak[c];
			results[c].dc             = (float)dcsum[c] / (float)ACOUSTIC_ANALYSIS_FRAMES;
		}
	}
	return st;
}

/* Run one acoustic window with a RUN-WIDE dead-mic latch: once ANY window's
 * mic read has failed, *mic_dead latches true and every LATER call (from
 * ANY caller) stops attempting mic reads at all -- passes capture_window()
 * a NULL mic instead. Fixes the #2132-adjacent bug this task exists for: a
 * dead mic that keeps getting retried for up to MIC_READ_TIMEOUT_MS on
 * every remaining window drains the I2S TX slab out from under the tone
 * writer between blocks, which made the (unrelated) TDM_CLOCK check fail
 * too. The TDM_CLOCK check itself no longer calls this at all (see
 * run_tdm_clock_check() below) -- it never touches the mic in the first
 * place, which is the real fix; this latch is the belt-and-suspenders
 * fix for the ACOUSTIC windows specifically, so a dead mic costs at most
 * one MIC_READ_TIMEOUT_MS timeout for the WHOLE run, not one per window.
 *
 * @p eligible is the window's own precondition (mic_ok, and for STOPPED
 * also stopped_valid) -- independent of mic_dead. When eligible is true but
 * mic_dead already latched, prints a one-line "skipped" note instead of
 * silently returning false, so a bench log distinguishes "this window
 * genuinely failed" from "skipped, already known dead".
 *
 * mic_dead only latches on a GENUINE failure now (issue #2133 round 4b): a
 * dropped burst (capture_window()'s st.mic_dropped, -EIO) is recoverable
 * per-window, not proof the mic is dead, so this function STOP/STARTs the
 * mic right here and lets the NEXT window try again -- see
 * window_status_t's comment for why the driver can tell the two apart. Only
 * an actual latch prints "mic_dead latched" with the triggering rc; a
 * recovered drop prints its own line and mic_dead stays false. */
static bool run_acoustic_window(const char      *label,
                                alp_audio_in_t  *mic,
                                bool            *mic_dead,
                                bool             eligible,
                                alp_audio_out_t *spk,
                                int16_t         *tone_buf,
                                uint32_t        *phase_acc,
                                uint32_t         samples_per_cycle,
                                chan_result_t    results[MIC_CHANNELS])
{
	if (eligible && *mic_dead) {
		printf("[probe] ACOUSTIC %s skipped -- mic already failed earlier this run\n", label);
	}
	bool attempt_mic = eligible && !*mic_dead;
	if (!attempt_mic && spk == NULL) return false; /* nothing to write or read either. */

	window_status_t st = capture_window(
	    attempt_mic ? mic : NULL, spk, tone_buf, phase_acc, samples_per_cycle, results);

	if (attempt_mic && !st.mic_ok) {
		if (st.mic_dropped) {
			alp_status_t stop_rc  = alp_audio_in_stop(mic);
			alp_status_t start_rc = alp_audio_in_start(mic);
			printf("[probe] ACOUSTIC %s DROPPED (rc=%d) -- recovering for the next window: "
			       "alp_audio_in_stop -> %d, alp_audio_in_start -> %d\n",
			       label,
			       (int)st.mic_fail_rc,
			       (int)stop_rc,
			       (int)start_rc);
			if (stop_rc != ALP_OK || start_rc != ALP_OK) {
				*mic_dead = true;
				printf("[probe] ACOUSTIC %s mic_dead latched -- drop recovery itself failed "
				       "(stop=%d start=%d)\n",
				       label,
				       (int)stop_rc,
				       (int)start_rc);
			}
		} else {
			*mic_dead = true;
			printf("[probe] ACOUSTIC %s mic_dead latched -- alp_audio_in_read rc=%d\n",
			       label,
			       (int)st.mic_fail_rc);
		}
	}
	return attempt_mic && st.mic_ok;
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
		       "ref(%uHz)=%6.1fdB  RMS=%7.1f  peak=%6.0f  DC=%7.1f\n",
		       label,
		       c,
		       chan_names[c],
		       (unsigned)TONE_BIN_HZ,
		       (double)POWER_TO_DB(r[c].tone_power),
		       (unsigned)REF_BIN_LOW_HZ,
		       (double)POWER_TO_DB(r[c].ref_low_power),
		       (unsigned)REF_BIN_HIGH_HZ,
		       (double)POWER_TO_DB(r[c].ref_high_power),
		       (double)r[c].rms,
		       (double)r[c].peak,
		       (double)r[c].dc);
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

/* Print BOTH verdict lines for an EARLY exit path (bridge/mux/gpio/I2C
 * failures, before the mic or I2S3 is ever opened) -- at every one of these
 * points NEITHER evidence stream was attempted, so both must say so
 * explicitly rather than the run silently omitting the ACOUSTIC line, which
 * a bench log reader could otherwise misread as "acoustic wasn't part of
 * this build" rather than "never got the chance to run". See the file
 * header's FIX 1 audit for why every verdict print site is guarded like
 * this: a verdict string must only come from the condition that actually
 * measured it. */
static void print_early_exit_verdicts(verdict_t v, const char *reason)
{
	printf("[probe] TDM_CLOCK VERDICT: %s\n", verdict_str(v));
	printf("[probe] ACOUSTIC VERDICT: %s -- %s\n",
	       acoustic_verdict_str(ACOUSTIC_INCONCLUSIVE),
	       reason);
}

/* ================================================================== */
/* EVIDENCE 1's DURING/STOPPED check -- its OWN tone-only segment, entirely */
/* before any mic activity. See the file header's "TONE-ON BUDGET, RESTATED" */
/* note for why this had to move, and why it is now a fixed, small cost.   */
/* ================================================================== */

/* Tone-only priming BEFORE the latch is cleared, so clear/settle/read runs
 * against a controller that has genuinely been playing for a while, not
 * the very first sample after ACTIVE. */
#define TDM_CHECK_PRIME_BLOCKS 6u /* ~96ms. */
/* Settle AFTER clear_faults(), paced by CONTINUING TO WRITE TONE BLOCKS --
 * not a blind k_msleep(). This is the actual bench-review fix: the
 * previous design's "settle" was an alp_audio_in_read() blocking for up to
 * MIC_READ_TIMEOUT_MS with NOTHING feeding the 2-block I2S TX slab, which
 * drained it and broke the very next alp_audio_out_write() -- read
 * capture_window() as it stood, or `git show`-diff this commit's parent,
 * for the exact call sequence that produced it. Must feed the slab for at
 * least CLOCK_CHECK_SETTLE_MS; the BUILD_ASSERT below is that promise kept
 * honest at compile time rather than by eyeballing block-count * ms/block. */
#define TDM_CHECK_SETTLE_BLOCKS 2u /* 2 * 16ms = 32ms >= CLOCK_CHECK_SETTLE_MS (20ms). */
BUILD_ASSERT(TDM_CHECK_SETTLE_BLOCKS *(SOUND_FRAMES_PER_BLOCK * 1000u / SOUND_SAMPLE_RATE_HZ) >=
                 CLOCK_CHECK_SETTLE_MS,
             "TDM_CHECK_SETTLE_BLOCKS must feed the TX slab for at least CLOCK_CHECK_SETTLE_MS");

/*
 * Run EVIDENCE 1's DURING/STOPPED check as ONE tone-only segment. This
 * function never takes a mic handle and never can -- that absence is the
 * actual fix, not a detail: nothing in here can ever block on a read, so
 * nothing in here can ever starve the TX slab the way an interleaved mic
 * read did.
 *
 * DURING: prime (TDM_CHECK_PRIME_BLOCKS), clear both amps' TDM_CLOCK
 * latches, settle (TDM_CHECK_SETTLE_BLOCKS, STILL writing tone throughout
 * -- see that constant's comment), read.
 * STOPPED: alp_audio_out_stop() (drain; amps stay ACTIVE), clear,
 * k_msleep(CLOCK_CHECK_SETTLE_MS) (safe to actually sleep here -- the
 * stream is stopped, there is nothing left to starve), read.
 *
 * *during_valid / *stopped_valid report whether that read actually
 * happened; @p faults_during / @p faults_stopped are only meaningful when
 * the matching flag is true. *during_fail_rc carries the exact
 * alp_audio_out_write() status that stopped DURING from happening (ALP_OK
 * if DURING did happen), so the caller's verdict line can print "DURING
 * was never read (rc=N)" instead of a bare unexplained INCONCLUSIVE.
 * *stop_rc carries alp_audio_out_stop()'s own status for the same reason
 * on the STOPPED side. Independent outcomes reported via out-params, same
 * reasoning as window_status_t above -- no single bool could represent
 * "DURING happened but STOPPED didn't, because of THIS rc" without losing
 * information the caller needs for its verdict-reason text.
 */
static void run_tdm_clock_check(tas2563_t       *amps,
                                const uint8_t   *amp_addrs,
                                alp_audio_out_t *spk,
                                int16_t         *tone_buf,
                                uint32_t        *phase_acc,
                                uint32_t         samples_per_cycle,
                                uint32_t         faults_during[AMP_COUNT],
                                bool            *during_valid,
                                alp_status_t    *during_fail_rc,
                                uint32_t         faults_stopped[AMP_COUNT],
                                bool            *stopped_valid,
                                alp_status_t    *stop_rc)
{
	*during_valid   = false;
	*during_fail_rc = ALP_OK; /* set below iff a tone write is what stopped DURING happening. */
	*stopped_valid  = false;

	bool tone_ok = true;
	for (unsigned b = 0; b < TDM_CHECK_PRIME_BLOCKS && tone_ok; b++) {
		alp_status_t wrc = write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle);
		if (wrc != ALP_OK) {
			tone_ok         = false;
			*during_fail_rc = wrc;
			printf("[probe] TDM check: alp_audio_out_write FAILED (prime block %u) rc=%d\n",
			       b,
			       (int)wrc);
		}
	}
	if (!tone_ok)
		return; /* DURING never read -- caller must print INCONCLUSIVE, not a direction. */

	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_clear_faults(&amps[i]);
	}

	for (unsigned b = 0; b < TDM_CHECK_SETTLE_BLOCKS && tone_ok; b++) {
		alp_status_t wrc = write_one_tone_block(spk, tone_buf, phase_acc, samples_per_cycle);
		if (wrc != ALP_OK) {
			tone_ok         = false;
			*during_fail_rc = wrc;
			printf("[probe] TDM check: alp_audio_out_write FAILED (settle block %u) rc=%d\n",
			       b,
			       (int)wrc);
		}
	}
	if (!tone_ok) return;

	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_read_faults(&amps[i], &faults_during[i]);
		printf("[probe] tas2563_read_faults(0x%02x) DURING (ACTIVE, playing) -> "
		       "0x%08x (TDM_CLOCK %s)\n",
		       amp_addrs[i],
		       faults_during[i],
		       (faults_during[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET" : "clear");
	}
	*during_valid = true;

	*stop_rc = alp_audio_out_stop(spk);
	printf("[probe] alp_audio_out_stop(I2S3) [stop-control, amps still ACTIVE] -> %d\n",
	       (int)*stop_rc);
	if (*stop_rc != ALP_OK) return; /* STOPPED never read -- caller must print INCONCLUSIVE. */

	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_clear_faults(&amps[i]);
	}
	k_msleep(CLOCK_CHECK_SETTLE_MS); /* no tone to feed -- the stream is genuinely stopped. */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_read_faults(&amps[i], &faults_stopped[i]);
		printf("[probe] tas2563_read_faults(0x%02x) STOPPED (ACTIVE, I2S3 halted) -> "
		       "0x%08x (TDM_CLOCK %s)\n",
		       amp_addrs[i],
		       faults_stopped[i],
		       (faults_stopped[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET" : "clear");
	}
	*stopped_valid = true;
}

/* ================================================================== */
/* PROBE_LISTEN -- attended listening variant (no PASS/FAIL verdict)  */
/* ================================================================== */
/* Compile-time mode, NOT a runtime branch: `west build ... -- -DPROBE_LISTEN=1`
 * (CMakeLists.txt) defines PROBE_LISTEN, which replaces main()'s ENTIRE body
 * with listen_main() below -- the normal probe (PROBE_LISTEN undefined) is
 * byte-for-byte the same object code as before this section was added.
 *
 * WHY THIS EXISTS: the r2137 run (image md5 77c1a833b43cb78aaac6ee59799dd124)
 * had every rc return 0 -- including alp_audio_out_stop()/start() after
 * #2137 -- and TDM_CLOCK read clear in BASELINE, DURING, and STOPPED on both
 * amps, yet the maintainer standing at the bench heard nothing at J14/J21.
 * TDM_CLOCK clear only proves the TDM bit clock is reaching the amp; it says
 * nothing about whether the amp is actually driving its Class-D output into
 * the speaker load, whether the 0.4 s acoustic windows this probe uses were
 * just too short for a human ear/room, or whether some OTHER page-0 field
 * (a mute bit, a slot mismatch that still clocks but plays silence, a boost/
 * charge-pump fault that isn't a latched fault bit) is the real reason nothing
 * is audible. This variant swaps the probe's brief automated windows for one
 * long, loud, continuous tone plus a wide page-0 register dump, so a human at
 * the bench can correlate "do I hear it" against "what does the chip say".
 *
 * REGISTER SET: every address below is copied VERBATIM from chips/tas2563/
 * tas2563.c's own (private, not exported by <alp/chips/tas2563.h>) TAS2563_REG_*
 * -- same values, same SLASET3D section/page citations, LISTEN_REG_ prefix
 * only to make clear this is this app's OWN copy, not a reach into driver
 * internals. NOT dumped: INT_MASK0-3, MISC_CFG2, and a dedicated playback
 * digital-volume register -- chips/tas2563/tas2563.c never reads or writes
 * any of the three, and nothing in this repository (driver, header, doc)
 * cites a SLASET3D section/address for them. Printing an invented hex
 * address for a real register on real hardware is exactly the kind of
 * register-address guess the house data-fidelity rule forbids -- flagged
 * here and in this task's report, not silently skipped. INT_CLK (0x30,
 * "INT & CLK CFG") is dumped as the closest GROUNDED stand-in for "chip
 * status/clock-detect" the driver actually defines; it is not a dedicated
 * status register. */
#if defined(PROBE_LISTEN)

#define LISTEN_REG_PAGE      0x00u /* Device page          (SLASET3D §7.5.2,  p.65). */
#define LISTEN_REG_PWR_CTL   0x02u /* Power control        (SLASET3D §7.5.4,  p.65). */
#define LISTEN_REG_PB_CFG1   0x03u /* Playback config 1    (SLASET3D §7.5.5,  p.66) -- AMP_LEVEL. */
#define LISTEN_REG_MISC_CFG1 0x04u /* Misc configuration 1 (SLASET3D §7.5.6,  p.67). */
#define LISTEN_REG_TDM_CFG0  0x06u /* TDM configuration 0  (SLASET3D §7.5.8,  p.68). */
#define LISTEN_REG_TDM_CFG1  0x07u /* TDM configuration 1  (SLASET3D §7.5.9,  p.69). */
#define LISTEN_REG_TDM_CFG2  0x08u /* TDM configuration 2  (SLASET3D §7.5.10, p.69). */
#define LISTEN_REG_TDM_CFG5  0x0Bu /* TDM TX V-sense slot  (SLASET3D §7.5.13, p.71). */
#define LISTEN_REG_TDM_CFG6  0x0Cu /* TDM TX I-sense slot  (SLASET3D §7.5.14, p.71). */
#define LISTEN_REG_INT_LTCH0 0x24u /* Latched interrupts 0 (SLASET3D §7.5.36, p.82). */
#define LISTEN_REG_INT_LTCH1 0x25u /* Latched interrupts 1 (SLASET3D §7.5.37, p.83). */
#define LISTEN_REG_INT_LTCH3 \
	0x26u                          /* Latched interrupts 2 (SLASET3D §7.5.38, p.84) -- driver's own
                                     * naming skips "LTCH2"; see chips/tas2563/tas2563.c. */
#define LISTEN_REG_INT_LTCH4 0x27u /* Latched interrupts 3 (SLASET3D §7.5.39, p.84). */
#define LISTEN_REG_INT_CLK   0x30u /* INT & CLK CFG        (SLASET3D §7.5.43, p.86). */
#define LISTEN_REG_MISC      0x32u /* IRQZ pin polarity    (SLASET3D §7.5.45, p.87). */
#define LISTEN_REG_REVID     0x7Du /* Revision + PG ID, RO (SLASET3D §7.5.60, p.93). */
#define LISTEN_REG_BOOK      0x7Fu /* Device book          (SLASET3D §7.5.62, p.94). */

/* Continuous-tone block counts, matching the app's existing SOUND_SAMPLE_RATE_HZ
 * (16 kHz) / SOUND_FRAMES_PER_BLOCK (256) -- one block is exactly 16 ms
 * (256/16000 s). 20 s divides exactly; 5 s does not (313*16ms = 5.008s, the
 * nearest whole block) -- fine for a human-timed listening phase. */
#define LISTEN_BLOCK_MS      (SOUND_FRAMES_PER_BLOCK * 1000u / SOUND_SAMPLE_RATE_HZ) /* 16 ms */
#define LISTEN_TONE1_BLOCKS  1250u /* 1250 * 16 ms = 20.000 s exact. */
#define LISTEN_TONE2_BLOCKS  313u  /* 313  * 16 ms =  5.008 s (nearest whole block to 5 s). */
#define LISTEN_REGDUMP_EVERY 125u  /* 125  * 16 ms =  2.000 s exact -- the "every 2 s" cadence. */

/* Raw single-register read, mirroring chips/tas2563/tas2563.c's own private
 * reg_read() (same two calls) -- legitimate because tas2563_t.bus/.addr are
 * PUBLIC fields (include/alp/chips/tas2563.h), not a vendor/internal
 * reach-around. 0xFF is poisoned: not a valid reset value for any register
 * dumped here, so a read that silently fails is visible in the printout. */
static uint8_t listen_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

/* Full register dump this task's step 2 asked for -- see the #if PROBE_LISTEN
 * block header comment for exactly what is and is not included and why.
 * Leaves PAGE/BOOK at whatever it read them as (both already page 0/book 0
 * on every call site below); nothing here ever leaves page 0, so there is
 * nothing to restore -- unlike the digital-volume register this task asked
 * for, which is NOT dumped (no grounded address, see above). */
static void listen_reg_dump(tas2563_t *ctx, uint8_t addr, const char *label)
{
	printf("[listen][regs] 0x%02x %-14s PAGE=0x%02x BOOK=0x%02x PWR_CTL=0x%02x "
	       "PB_CFG1=0x%02x MISC_CFG1=0x%02x\n",
	       addr,
	       label,
	       listen_reg_read(ctx, LISTEN_REG_PAGE),
	       listen_reg_read(ctx, LISTEN_REG_BOOK),
	       listen_reg_read(ctx, LISTEN_REG_PWR_CTL),
	       listen_reg_read(ctx, LISTEN_REG_PB_CFG1),
	       listen_reg_read(ctx, LISTEN_REG_MISC_CFG1));
	printf("[listen][regs] 0x%02x %-14s TDM_CFG0=0x%02x TDM_CFG1=0x%02x TDM_CFG2=0x%02x "
	       "TDM_CFG5=0x%02x TDM_CFG6=0x%02x\n",
	       addr,
	       label,
	       listen_reg_read(ctx, LISTEN_REG_TDM_CFG0),
	       listen_reg_read(ctx, LISTEN_REG_TDM_CFG1),
	       listen_reg_read(ctx, LISTEN_REG_TDM_CFG2),
	       listen_reg_read(ctx, LISTEN_REG_TDM_CFG5),
	       listen_reg_read(ctx, LISTEN_REG_TDM_CFG6));
	printf("[listen][regs] 0x%02x %-14s INT_LTCH0=0x%02x INT_LTCH1=0x%02x INT_LTCH3=0x%02x "
	       "INT_LTCH4=0x%02x INT_CLK=0x%02x MISC=0x%02x REVID=0x%02x\n",
	       addr,
	       label,
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH0),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH1),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH3),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH4),
	       listen_reg_read(ctx, LISTEN_REG_INT_CLK),
	       listen_reg_read(ctx, LISTEN_REG_MISC),
	       listen_reg_read(ctx, LISTEN_REG_REVID));
}

/* Step 3/4's periodic subset (PWR_CTL, INT_LTCH0..4, INT_CLK-as-status) --
 * fewer registers than listen_reg_dump() so it fits the 2 s/1 s cadence
 * without spamming the console during a 20 s tone. */
static void listen_reg_dump_brief(tas2563_t *ctx, uint8_t addr, const char *label)
{
	printf("[listen][regs] 0x%02x %-14s PWR_CTL=0x%02x INT_LTCH0=0x%02x INT_LTCH1=0x%02x "
	       "INT_LTCH3=0x%02x INT_LTCH4=0x%02x INT_CLK(status)=0x%02x\n",
	       addr,
	       label,
	       listen_reg_read(ctx, LISTEN_REG_PWR_CTL),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH0),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH1),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH3),
	       listen_reg_read(ctx, LISTEN_REG_INT_LTCH4),
	       listen_reg_read(ctx, LISTEN_REG_INT_CLK));
}

/* One CONTINUOUS 1 kHz sine block -- unlike write_one_tone_block() (a square
 * wave, reused for the automated probe's short Goertzel windows, where the
 * extra harmonics don't matter), this task explicitly asked for a sine.
 * Amplitude is SOUND_TONE_AMPLITUDE (20000/32767, the SAME headroom-limited
 * peak the file header's HARDWARE SAFETY section already analyses), not raw
 * INT16_MAX -- "full-scale" here means this app's own existing safety
 * ceiling, not literally the bit pattern 0x7FFF; SOUND_VOL_MAX (48/255) is
 * the other half of that same, already-reviewed safety margin and is what
 * this mode is told to use. Blocks back-to-back, so the only silence in a
 * tone phase is a write() that itself failed -- counted by the caller. */
static alp_status_t listen_write_sine_block(alp_audio_out_t *spk,
                                            int16_t         *buf,
                                            uint32_t        *phase_acc,
                                            uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		float theta =
		    GOERTZEL_TWO_PI * (float)(*phase_acc % samples_per_cycle) / (float)samples_per_cycle;
		int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * sinf(theta));
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* count blocks/write failures over `blocks` continuous sine blocks, dumping
 * listen_reg_dump_brief() every LISTEN_REGDUMP_EVERY blocks (~2 s). Shared by
 * both tone phases (step 3 and step 5). */
static uint32_t listen_play_sine(alp_audio_out_t *spk,
                                 int16_t         *buf,
                                 uint32_t        *phase_acc,
                                 uint32_t         samples_per_cycle,
                                 uint32_t         blocks,
                                 tas2563_t        amps[AMP_COUNT])
{
	uint32_t write_failures = 0;
	for (uint32_t b = 0; b < blocks; b++) {
		alp_status_t wrc = listen_write_sine_block(spk, buf, phase_acc, samples_per_cycle);
		if (wrc != ALP_OK) {
			write_failures++;
			printf(
			    "[listen] alp_audio_out_write FAILED (block %u/%u) rc=%d\n", b, blocks, (int)wrc);
		}
		if (b != 0 && (b % LISTEN_REGDUMP_EVERY) == 0) {
			for (size_t i = 0; i < AMP_COUNT; i++) {
				listen_reg_dump_brief(&amps[i], amp_addrs[i], "TONE");
			}
		}
	}
	return write_failures;
}

static int listen_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_LISTEN): attended listening test ===\n");
	(void)alp_init();

	/* --- 1. Same bring-up as the probe: bridge, mux, AMP_ENABLE ---------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[listen] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[listen] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[listen] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[listen] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[listen] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[listen] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[listen] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[listen] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[listen] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[listen] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[listen] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	/* --- 2. Register dump after configure --------------------------------- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		listen_reg_dump(&amps[i], amp_addrs[i], "POST-CONFIG");
	}

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u];
	uint32_t       phase_acc         = 0;
	const uint32_t samples_per_cycle = SOUND_SAMPLE_RATE_HZ / SOUND_TONE_HZ;

	/* --- 3. Countdown, I2S start, both amps ACTIVE, 20 s continuous tone -- */
	printf("[listen] tone starts in 3 s\n");
	k_msleep(1000);
	printf("[listen] tone starts in 2 s\n");
	k_msleep(1000);
	printf("[listen] tone starts in 1 s\n");
	k_msleep(1000);

	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[listen] alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_set_volume(spk, SOUND_VOL_MAX);
	printf("[listen] alp_audio_out_set_volume(%u) -> %d\n", SOUND_VOL_MAX, (int)spk_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
		printf("[listen] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
	}

	uint32_t total_write_failures = 0;
	if (spk_rc == ALP_OK) {
		printf("[listen] TONE ON: 1 kHz full-scale, 20 s -- listen at J14/J21 speakers\n");
		total_write_failures += listen_play_sine(
		    spk, tone_buf, &phase_acc, samples_per_cycle, LISTEN_TONE1_BLOCKS, amps);
	} else {
		printf("[listen] I2S3 did not start -- skipping tone phase\n");
	}

	/* --- 4. Silence phase: I2S stopped, amps still ACTIVE, 5 s ------------ */
	printf("[listen] TONE OFF (I2S stopped, amps ACTIVE) 5 s\n");
	alp_status_t stop_rc = alp_audio_out_stop(spk);
	printf("[listen] alp_audio_out_stop(I2S3) -> %d\n", (int)stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		listen_reg_dump_brief(&amps[i], amp_addrs[i], "SILENCE");
	}
	for (unsigned s = 0; s < 5u; s++) {
		k_msleep(1000);
		for (size_t i = 0; i < AMP_COUNT; i++) {
			listen_reg_dump_brief(&amps[i], amp_addrs[i], "SILENCE");
		}
	}

	/* --- 5. Second tone phase: restart, 5 s, stop -------------------------- */
	printf("[listen] TONE ON again 5 s\n");
	alp_status_t restart_rc = alp_audio_out_start(spk);
	printf("[listen] alp_audio_out_start(I2S3) [restart] -> %d\n", (int)restart_rc);
	if (restart_rc == ALP_OK) {
		total_write_failures += listen_play_sine(
		    spk, tone_buf, &phase_acc, samples_per_cycle, LISTEN_TONE2_BLOCKS, amps);
	} else {
		printf("[listen] restart failed -- skipping second tone phase\n");
	}
	alp_status_t stop2_rc = alp_audio_out_stop(spk);
	printf("[listen] alp_audio_out_stop(I2S3) [final] -> %d\n", (int)stop2_rc);

	printf("[listen] total alp_audio_out_write() failures this run: %u\n", total_write_failures);

	/* --- 6. Shutdown + teardown -------------------------------------------- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[listen] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
		tas2563_deinit(&amps[i]);
	}
	if (spk != NULL) alp_audio_out_close(spk);
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	printf("[listen] done\n");
	return 0;
}

#endif /* PROBE_LISTEN */

/* ================================================================== */
/* PROBE_MELODY -- attended short-tune variant (Ode to Joy, public domain) */
/* ================================================================== */
/* Compile-time mode, same shape as PROBE_LISTEN above: `west build ... --
 * -DPROBE_MELODY=1` replaces main()'s ENTIRE body with melody_main(); with
 * neither PROBE_LISTEN nor PROBE_MELODY defined, main() is unchanged, and
 * with PROBE_LISTEN defined, this whole block is preprocessed away before
 * the compiler ever sees it -- so neither existing build's object code
 * moves by adding this one. melody_main() deliberately duplicates
 * listen_main()'s bring-up sequence (bridge/mux/AMP_ENABLE/tas2563_init/
 * level MIN/configure_i2s) rather than sharing a new helper with it, for
 * the same reason: touching listen_main() to extract one risks it no
 * longer being byte-identical to the image the maintainer already heard
 * tone from on e1m-aen-evk-03.
 *
 * With U46's VCC now on +3V3, the maintainer heard PROBE_LISTEN's tone
 * cleanly -- this mode asks for a short recognisable tune instead of one
 * sustained note, at the SAME SOUND_VOL_MAX (48) volume that was already
 * proven comfortably audible; it does not go any louder. */
#if defined(PROBE_MELODY)

#include <string.h> /* memset(), used by melody_play_note()'s silence spans. */

/* Ode to Joy's main theme (Beethoven's 9th Symphony, 1824 -- public domain)
 * uses only 5 pitches spanning a major 5th, C4-G4. A4=440 Hz equal
 * temperament: freq = 440 * 2^((n-9)/12), n = semitones from A4 (C4=-9,
 * D4=-7, E4=-5, F4=-4, G4=-2); rounded to the nearest Hz here -- e.g.
 * 261.63 Hz -> 262 -- rather than computed at runtime, since a note's pitch
 * never changes mid-note. */
typedef enum { NOTE_C4, NOTE_D4, NOTE_E4, NOTE_F4, NOTE_G4, NOTE_REST } melody_note_t;

static const uint32_t melody_freq_hz[5] = {
	262u, /* NOTE_C4, 261.63 Hz */
	294u, /* NOTE_D4, 293.66 Hz */
	330u, /* NOTE_E4, 329.63 Hz */
	349u, /* NOTE_F4, 349.23 Hz */
	392u, /* NOTE_G4, 392.00 Hz */
};

/* PWR_CTL address, copied verbatim from chips/tas2563/tas2563.c's own
 * (private) TAS2563_REG_PWR_CTL -- see PROBE_LISTEN's LISTEN_REG_* block
 * above for the same value/citation; melody_reg_read() mirrors that
 * block's listen_reg_read() (same two alp_i2c_write_read() calls, again
 * legitimate because tas2563_t.bus/.addr are public fields). Read here so
 * melody_main() can print, right before each play, whether the amp is
 * genuinely ACTIVE (mode bits 0x00) or was auto-shutdown by the chip
 * itself -- see MELODY_REPLAY_GAP_BLOCKS's comment for the bench evidence
 * that made this necessary. */
#define MELODY_REG_PWR_CTL 0x02u /* Power control (SLASET3D §7.5.4, p.65). */

static uint8_t melody_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

static void melody_print_pwr_ctl(tas2563_t amps[AMP_COUNT])
{
	printf("[melody] PWR_CTL before play: 0x%02x=0x%02x 0x%02x=0x%02x\n",
	       amp_addrs[0],
	       melody_reg_read(&amps[0], MELODY_REG_PWR_CTL),
	       amp_addrs[1],
	       melody_reg_read(&amps[1], MELODY_REG_PWR_CTL));
}

/* Every duration is a WHOLE number of SOUND_FRAMES_PER_BLOCK (256 frames,
 * 16 ms at SOUND_SAMPLE_RATE_HZ=16000) blocks -- no partial block, so
 * melody_play_note() below never needs to pad or split a write(), the same
 * simplification listen_play_sine() relies on for its own block counts.
 * quarter=25 (400 ms), eighth=12 (~192 ms), dotted-quarter=37 (~592 ms),
 * half=50 (800 ms) -- a tempo of 150 BPM chosen only so 400 ms/quarter
 * divides 16 ms/block evenly (25 blocks exactly); nothing about the tune
 * requires this exact tempo. */
typedef struct {
	melody_note_t note;
	uint16_t      blocks;
} melody_step_t;

/* Two 4-bar phrases (the classic "beginner" simplification of the theme,
 * numbered notation 3 3 4 5|5 4 3 2|1 1 2 3|3. 2 2 / 3 3 4 5|5 4 3 2|1 1 2
 * 3|2. 1 1, 1=C), a short rest between them, ~31 steps / ~13 s total --
 * within this task's "16-32 notes"/"10-15 s" ask. */
static const melody_step_t ode_to_joy[] = {
	/* Phrase A */
	{ NOTE_E4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_F4, 25 },
	{ NOTE_G4, 25 },
	{ NOTE_G4, 25 },
	{ NOTE_F4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_D4, 25 },
	{ NOTE_C4, 25 },
	{ NOTE_C4, 25 },
	{ NOTE_D4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_E4, 37 },
	{ NOTE_D4, 12 },
	{ NOTE_D4, 50 },
	{ NOTE_REST, 12 },
	/* Phrase B */
	{ NOTE_E4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_F4, 25 },
	{ NOTE_G4, 25 },
	{ NOTE_G4, 25 },
	{ NOTE_F4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_D4, 25 },
	{ NOTE_C4, 25 },
	{ NOTE_C4, 25 },
	{ NOTE_D4, 25 },
	{ NOTE_E4, 25 },
	{ NOTE_D4, 37 },
	{ NOTE_C4, 12 },
	{ NOTE_C4, 50 },
};
#define ODE_TO_JOY_PHRASE_A_STEPS 16u /* through the rest; phrase B is the remainder. */

#define MELODY_GAP_BLOCKS      2u   /* ~32 ms of silence between notes (not for a REST step). */
#define MELODY_ENVELOPE_FRAMES 128u /* 8 ms at 16 kHz -- inside this task's 5-10 ms ask. */
/* 125 * 16 ms = 2.000 s exact -- the "play it again in 2 s" gap. Written as
 * silent blocks, NEVER as a k_msleep() with I2S left idle: bench evidence
 * from the PROBE_LISTEN run on e1m-aen-evk-03 showed that once the bit
 * clock stalls (alp_audio_out_stop()), the TAS2563 self-shuts-down within
 * ~1 s on its own -- PWR_CTL read 0x0c (ACTIVE) then 0x0e (SHUTDOWN) with
 * no tas2563_set_mode() call in between, and INT_LTCH0 bit 2 (TDM clock
 * error), INT_LTCH3 bit 6 (BOOST_CLOCK), INT_LTCH4 bit 7
 * (DEVICE_POWER_DOWN) all latched. A silent WRITE keeps the TDM bit clock
 * (and therefore the amp) alive with nothing audible. */
#define MELODY_REPLAY_GAP_BLOCKS 125u

/* `blocks` silent (all-zero) SOUND_FRAMES_PER_BLOCK writes -- keeps I2S's
 * bit clock running (see MELODY_REPLAY_GAP_BLOCKS's comment on why this
 * must never be a k_msleep() with the stream idle instead). Shared by
 * melody_play_note()'s NOTE_REST/gap-tail cases and melody_main()'s
 * between-plays gap. */
static uint32_t melody_write_silence_blocks(alp_audio_out_t *spk, int16_t *buf, uint32_t blocks)
{
	uint32_t write_failures = 0;
	memset(buf, 0, SOUND_FRAMES_PER_BLOCK * 2u * sizeof(int16_t));
	for (uint32_t b = 0; b < blocks; b++) {
		if (alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u) != ALP_OK) {
			write_failures++;
		}
	}
	return write_failures;
}

/* One note (or, for NOTE_REST, one span of silence): a linear attack/
 * release envelope over MELODY_ENVELOPE_FRAMES at each end (no clicks),
 * flat in between, written SOUND_FRAMES_PER_BLOCK frames at a time so
 * playback can never underrun -- the same write-block-by-block pacing
 * write_one_tone_block()/listen_write_sine_block() above already use.
 * samples_per_cycle is computed ONCE per note (integer division), not per
 * sample. Returns the write-failure count for this note/rest. */
static uint32_t
melody_play_note(alp_audio_out_t *spk, int16_t *buf, melody_note_t note, uint16_t total_blocks)
{
	if (note == NOTE_REST) return melody_write_silence_blocks(spk, buf, total_blocks);

	uint32_t write_failures    = 0;
	uint16_t tone_blocks       = (total_blocks > MELODY_GAP_BLOCKS)
	                                 ? (uint16_t)(total_blocks - MELODY_GAP_BLOCKS)
	                                 : total_blocks;
	uint32_t samples_per_cycle = SOUND_SAMPLE_RATE_HZ / melody_freq_hz[note];
	uint32_t total_samples     = (uint32_t)tone_blocks * SOUND_FRAMES_PER_BLOCK;
	uint32_t phase_acc         = 0;

	for (uint16_t b = 0; b < tone_blocks; b++) {
		for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
			uint32_t idx = (uint32_t)b * SOUND_FRAMES_PER_BLOCK + f;
			float    env = 1.0f;
			if (idx < MELODY_ENVELOPE_FRAMES) {
				env = (float)idx / (float)MELODY_ENVELOPE_FRAMES;
			} else if (idx >= total_samples - MELODY_ENVELOPE_FRAMES) {
				env = (float)(total_samples - idx) / (float)MELODY_ENVELOPE_FRAMES;
			}
			float theta =
			    GOERTZEL_TWO_PI * (float)(phase_acc % samples_per_cycle) / (float)samples_per_cycle;
			int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * env * sinf(theta));
			buf[2u * f]     = sample;
			buf[2u * f + 1] = sample;
			phase_acc++;
		}
		if (alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u) != ALP_OK) {
			write_failures++;
		}
	}

	if (tone_blocks < total_blocks) {
		write_failures += melody_write_silence_blocks(spk, buf, total_blocks - tone_blocks);
	}
	return write_failures;
}

/* Plays ode_to_joy[] once, phrase by phrase, printing one line per phrase
 * (this task's console-format step 3). Returns the run's write-failure
 * count so the two play-throughs in melody_main() can be summed. */
static uint32_t melody_play_tune(alp_audio_out_t *spk, int16_t *buf)
{
	uint32_t write_failures = 0;

	printf("[melody] phrase A: E E F G | G F E D | C C D E | E. D D\n");
	for (size_t i = 0; i < ODE_TO_JOY_PHRASE_A_STEPS; i++) {
		write_failures += melody_play_note(spk, buf, ode_to_joy[i].note, ode_to_joy[i].blocks);
	}

	printf("[melody] phrase B: E E F G | G F E D | C C D E | D. C C\n");
	for (size_t i = ODE_TO_JOY_PHRASE_A_STEPS; i < ARRAY_SIZE(ode_to_joy); i++) {
		write_failures += melody_play_note(spk, buf, ode_to_joy[i].note, ode_to_joy[i].blocks);
	}

	return write_failures;
}

static int melody_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_MELODY): attended tune test ===\n");
	(void)alp_init();

	/* --- Same bring-up as PROBE_LISTEN: bridge, mux, AMP_ENABLE --------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[melody] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[melody] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[melody] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[melody] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[melody] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[melody] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[melody] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[melody] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[melody] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[melody] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[melody] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u];

	printf("[melody] starts in 3 s\n");
	k_msleep(1000);
	printf("[melody] starts in 2 s\n");
	k_msleep(1000);
	printf("[melody] starts in 1 s\n");
	k_msleep(1000);

	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[melody] alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	/* SAME SOUND_VOL_MAX (48) PROBE_LISTEN used and the maintainer already
	 * heard comfortably -- never louder. */
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_set_volume(spk, SOUND_VOL_MAX);
	printf("[melody] alp_audio_out_set_volume(%u) -> %d\n", SOUND_VOL_MAX, (int)spk_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
		printf("[melody] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
	}

	uint32_t total_write_failures = 0;
	if (spk_rc == ALP_OK) {
		melody_print_pwr_ctl(amps);
		printf("[melody] PLAYING: Ode to Joy (Beethoven, public domain)\n");
		total_write_failures += melody_play_tune(spk, tone_buf);

		printf("[melody] play it again in 2 s\n");
		/* Silent WRITES, not k_msleep() -- see MELODY_REPLAY_GAP_BLOCKS's
		 * comment: an idle I2S bus lets the TAS2563 auto-shutdown within
		 * ~1 s, which would leave the second play silent. */
		total_write_failures +=
		    melody_write_silence_blocks(spk, tone_buf, MELODY_REPLAY_GAP_BLOCKS);

		melody_print_pwr_ctl(amps);
		total_write_failures += melody_play_tune(spk, tone_buf);
	} else {
		printf("[melody] I2S3 did not start -- skipping playback\n");
	}
	printf("[melody] total alp_audio_out_write() failures this run: %u\n", total_write_failures);

	/* The ONLY alp_audio_out_stop() this run makes -- nothing plays after
	 * it, so there is no restart to recover for (no clear-latches +
	 * ACTIVE-again dance needed here): both notes/rests and the between-
	 * plays gap above are silent WRITES, never a stop, specifically so the
	 * TAS2563 never sees the bit clock disappear -- and therefore never
	 * self-shuts-down -- anywhere but this final, deliberate teardown. */
	alp_status_t stop_rc = alp_audio_out_stop(spk);
	printf("[melody] alp_audio_out_stop(I2S3) -> %d\n", (int)stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[melody] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
		tas2563_deinit(&amps[i]);
	}
	if (spk != NULL) alp_audio_out_close(spk);
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	printf("[melody] done\n");
	return 0;
}

#endif /* PROBE_MELODY */

/* ================================================================== */
/* PROBE_LOOPBACK -- speakers as a controlled stimulus for the PDM mics */
/* ================================================================== */
/* Compile-time mode, same shape as PROBE_LISTEN/PROBE_MELODY above: `west
 * build ... -- -DPROBE_LOOPBACK=1` replaces main()'s ENTIRE body with
 * loop_main(); with none of PROBE_LISTEN/PROBE_MELODY/PROBE_LOOPBACK
 * defined, main() is unchanged, and with exactly one of the other two
 * defined, this whole block is preprocessed away -- so none of the three
 * existing builds' object code moves by adding this one. loop_main()
 * duplicates listen_main()/melody_main()'s bring-up sequence rather than
 * sharing a helper with either, for the same byte-identity reason those
 * two don't share one with each other.
 *
 * PROBE_LISTEN and PROBE_MELODY both proved the speakers are real and
 * audible on e1m-aen-evk-03. This mode turns that proven output into a
 * CONTROLLED acoustic stimulus for the PDM mics (U19 LEFT / U20 RIGHT),
 * which this app has never shown actually capturing real audio (the
 * original acoustic-loopback windows never got a clean silicon run -- see
 * the file header's "FIRST-SILICON BENCH RESULT" / "THIRD FINDING"
 * sections). Six ~2 s windows (three silent, three tone) look for the
 * mic's Goertzel bins tracking WHICH tone is playing, not just that
 * something loud happened.
 *
 * ISSUE #2146 (this run's root cause, found via PROBE_MELODY's bench
 * data): the TAS2563 self-shuts-down within ~1 s of the I2S bit clock
 * stopping. This mode NEVER calls alp_audio_out_stop() until the very
 * end -- every silent window is a stream of zero-sample writes, exactly
 * like PROBE_MELODY's rests/gap fix. */
#if defined(PROBE_LOOPBACK)

#include <string.h> /* memset(), used by the silence writers below. */

/* PWR_CTL address, copied verbatim from chips/tas2563/tas2563.c's own
 * (private) TAS2563_REG_PWR_CTL -- see PROBE_LISTEN's LISTEN_REG_PWR_CTL /
 * PROBE_MELODY's MELODY_REG_PWR_CTL above for the same value/citation.
 * loop_reg_read() mirrors those blocks' own read helpers (same two
 * alp_i2c_write_read() calls, legitimate because tas2563_t.bus/.addr are
 * public fields). */
#define LOOP_REG_PWR_CTL 0x02u /* Power control (SLASET3D §7.5.4, p.65). */

static uint8_t loop_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

static void loop_print_pwr_ctl(const char *label, tas2563_t amps[AMP_COUNT])
{
	printf("[loop] PWR_CTL %s: 0x%02x=0x%02x 0x%02x=0x%02x\n",
	       label,
	       amp_addrs[0],
	       loop_reg_read(&amps[0], LOOP_REG_PWR_CTL),
	       amp_addrs[1],
	       loop_reg_read(&amps[1], LOOP_REG_PWR_CTL));
}

/* Window shape, all in whole SOUND_FRAMES_PER_BLOCK/MIC_FRAMES_PER_BLOCK
 * blocks (both exactly 16 ms at their own sample rate -- 256/16000 =
 * 768/48000 -- so the I2S write loop and the PDM read loop stay in lock
 * step one block at a time, the same simplification PROBE_LISTEN/
 * PROBE_MELODY rely on). Chosen so every Goertzel bin below (1000 Hz,
 * 500 Hz, 700 Hz, 1400 Hz) is EXACT at CAPTURE_FRAMES=46080 samples (all
 * four k = N*f/fs come out integer -- see the git history for the LCM
 * derivation): CAPTURE_ITERS=60 is the smallest multiple of 5 (so
 * 60*768=46080 is a multiple of 480, the LCM constraint) near "about 1 s"
 * (960 ms). PRE_SETTLE (832 ms) gives the volume/frequency change and the
 * decimator time to settle BEFORE the explicit 200 ms DISCARD this task
 * asked for; DISCARD+CAPTURE together read real mic data, DISCARD's just
 * never accumulated into any stat. Total = 125 blocks = 2.000 s exact,
 * this task's "about 2 s" per window. */
#define LOOP_PRE_SETTLE_ITERS 52u /* 832 ms. */
#define LOOP_DISCARD_ITERS    13u /* 208 ms -- "discard the first 200 ms". */
#define LOOP_CAPTURE_ITERS    60u /* 960 ms -- "about 1 s". */
#define LOOP_WINDOW_ITERS     (LOOP_PRE_SETTLE_ITERS + LOOP_DISCARD_ITERS + LOOP_CAPTURE_ITERS)
#define LOOP_CAPTURE_FRAMES   (LOOP_CAPTURE_ITERS * MIC_FRAMES_PER_BLOCK) /* 46080. */

typedef enum { LOOP_SILENCE, LOOP_TONE } loop_kind_t;

typedef struct {
	const char *name;
	loop_kind_t kind;
	uint32_t    freq_hz; /* only meaningful if kind == LOOP_TONE. */
	uint8_t     volume;  /* only meaningful if kind == LOOP_TONE. */
} loop_window_t;

static const loop_window_t loop_windows[] = {
	{ "SILENCE-A", LOOP_SILENCE, 0, 0 },      { "TONE-1k-VOL16", LOOP_TONE, 1000, 16 },
	{ "TONE-1k-VOL48", LOOP_TONE, 1000, 48 }, { "SILENCE-B", LOOP_SILENCE, 0, 0 },
	{ "TONE-500-VOL48", LOOP_TONE, 500, 48 }, { "SILENCE-C", LOOP_SILENCE, 0, 0 },
};
#define LOOP_WINDOW_COUNT ARRAY_SIZE(loop_windows)

static alp_status_t loop_write_sine_block(alp_audio_out_t *spk,
                                          int16_t         *buf,
                                          uint32_t        *phase_acc,
                                          uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		float theta =
		    GOERTZEL_TWO_PI * (float)(*phase_acc % samples_per_cycle) / (float)samples_per_cycle;
		int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * sinf(theta));
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

static alp_status_t loop_write_silence_block(alp_audio_out_t *spk, int16_t *buf)
{
	memset(buf, 0, SOUND_FRAMES_PER_BLOCK * 2u * sizeof(int16_t));
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* Fixed-point (Q13) single-bin Goertzel -- the per-sample recurrence
 * (loop_goertzel_step()) is pure integer arithmetic, per this task's
 * "keep all per-sample work integer and cheap" ask: the round-4b lesson
 * (examples/aen/aen-i2s-tas2563-probe's own earlier fix, this file's
 * capture_window()) was that float per-sample math with no CONFIG_FPU can
 * starve the mic's slab. coeff_q13 is computed ONCE per window (float
 * cosf() is fine there, it's not in the per-sample path); s_prev/s_prev2
 * stay int64 to leave enormous headroom against a full-scale, exactly-
 * resonant 46080-sample accumulation. loop_goertzel_power() runs ONCE per
 * channel per window (after every sample for this window is already
 * read), not per sample -- float/double there costs nothing. */
typedef struct {
	int32_t coeff_q13;
	int64_t s_prev;
	int64_t s_prev2;
} loop_goertzel_t;

static void
loop_goertzel_reset(loop_goertzel_t *g, uint32_t freq_hz, uint32_t n_samples, uint32_t fs_hz)
{
	float k = (float)n_samples * (float)freq_hz / (float)fs_hz; /* exact, see the table above. */
	float omega  = GOERTZEL_TWO_PI * k / (float)n_samples;
	float coeff  = 2.0f * cosf(omega);
	g->coeff_q13 = (int32_t)(coeff * 8192.0f + (coeff >= 0.0f ? 0.5f : -0.5f));
	g->s_prev    = 0;
	g->s_prev2   = 0;
}

static inline void loop_goertzel_step(loop_goertzel_t *g, int32_t x)
{
	int64_t s  = (int64_t)x + (((int64_t)g->coeff_q13 * g->s_prev) >> 13) - g->s_prev2;
	g->s_prev2 = g->s_prev;
	g->s_prev  = s;
}

static double loop_goertzel_power(const loop_goertzel_t *g)
{
	double s1 = (double)g->s_prev;
	double s2 = (double)g->s_prev2;
	double c  = (double)g->coeff_q13 / 8192.0;
	return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Per-channel accumulator, live ONLY during a window's CAPTURE_FRAMES --
 * min/max/sum/sumsq are int64/int16, updated once per sample, no float. */
typedef struct {
	int16_t         minv;
	int16_t         maxv;
	int64_t         sum;
	int64_t         sumsq;
	loop_goertzel_t f1k;
	loop_goertzel_t f500;
	loop_goertzel_t ref700;
	loop_goertzel_t ref1400;
} loop_chan_stats_t;

/* Per-channel RESULT, computed ONCE per window from loop_chan_stats_t
 * after every sample is in -- see loop_chan_stats_t's comment. dB values
 * are each bin's power EXPRESSED RELATIVE TO this window's own reference
 * bins (the (700 Hz + 1400 Hz)/2 average, per this task's ask), not an
 * absolute level -- so ref700_db/ref1400_db hover near 0 dB by
 * construction and f1k_db/f500_db read large and positive only when that
 * frequency's tone is actually playing. */
typedef struct {
	int32_t p2p;
	int32_t rms;
	int32_t dc;
	double  f1k_db;
	double  f500_db;
	double  ref700_db;
	double  ref1400_db;
} loop_chan_result_t;

static void loop_print_window(const char *name, const loop_chan_result_t r[MIC_CHANNELS])
{
	for (size_t c = 0; c < MIC_CHANNELS; c++) {
		printf("[loop] %-14s ch%zu p2p=%-6d rms=%-6d dc=%-6d f1k=%6.1fdB f500=%6.1fdB "
		       "ref700=%6.1fdB ref1400=%6.1fdB\n",
		       name,
		       c,
		       r[c].p2p,
		       r[c].rms,
		       r[c].dc,
		       r[c].f1k_db,
		       r[c].f500_db,
		       r[c].ref700_db,
		       r[c].ref1400_db);
	}
}

/* Runs one window (up to a second attempt if the mic drops a burst mid-
 * window -- see the -EIO handling below): writes I2S continuously (tone or
 * silence, NEVER a stop -- issue #2146), reads one PDM block per I2S block
 * in lock-step, discards PRE_SETTLE+DISCARD blocks' mic data with zero
 * per-sample work, then accumulates CAPTURE blocks' worth into
 * loop_chan_stats_t. Returns false only if the mic never produced clean
 * data even after one STOP/START recovery retry -- the caller must then
 * treat this window (and therefore the whole run) as INCONCLUSIVE, never
 * silently substitute stale/partial results. */
static bool loop_run_window(alp_audio_out_t     *spk,
                            alp_audio_in_t      *mic,
                            const loop_window_t *w,
                            int16_t             *out_buf,
                            int16_t             *mic_scratch,
                            loop_chan_result_t   results[MIC_CHANNELS],
                            uint32_t            *overrun_count)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		loop_chan_stats_t stats[MIC_CHANNELS];
		for (size_t c = 0; c < MIC_CHANNELS; c++) {
			stats[c].minv  = INT16_MAX;
			stats[c].maxv  = INT16_MIN;
			stats[c].sum   = 0;
			stats[c].sumsq = 0;
			loop_goertzel_reset(&stats[c].f1k, 1000u, LOOP_CAPTURE_FRAMES, MIC_SAMPLE_RATE_HZ);
			loop_goertzel_reset(&stats[c].f500, 500u, LOOP_CAPTURE_FRAMES, MIC_SAMPLE_RATE_HZ);
			loop_goertzel_reset(&stats[c].ref700, 700u, LOOP_CAPTURE_FRAMES, MIC_SAMPLE_RATE_HZ);
			loop_goertzel_reset(&stats[c].ref1400, 1400u, LOOP_CAPTURE_FRAMES, MIC_SAMPLE_RATE_HZ);
		}

		uint32_t phase_acc = 0;
		uint32_t samples_per_cycle =
		    (w->kind == LOOP_TONE) ? (SOUND_SAMPLE_RATE_HZ / w->freq_hz) : 0;
		bool         mic_error  = false;
		alp_status_t mic_err_rc = ALP_OK;

		for (uint32_t it = 0; it < LOOP_WINDOW_ITERS; it++) {
			if (w->kind == LOOP_TONE) {
				(void)loop_write_sine_block(spk, out_buf, &phase_acc, samples_per_cycle);
			} else {
				(void)loop_write_silence_block(spk, out_buf);
			}

			size_t       got = 0;
			alp_status_t rrc = alp_audio_in_read(
			    mic, mic_scratch, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
			if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
				mic_error  = true;
				mic_err_rc = rrc;
				break;
			}

			if (it >= LOOP_PRE_SETTLE_ITERS + LOOP_DISCARD_ITERS) {
				for (uint32_t f = 0; f < MIC_FRAMES_PER_BLOCK; f++) {
					for (size_t c = 0; c < MIC_CHANNELS; c++) {
						int16_t x = mic_scratch[f * MIC_CHANNELS + c];
						if (x < stats[c].minv) stats[c].minv = x;
						if (x > stats[c].maxv) stats[c].maxv = x;
						stats[c].sum += x;
						stats[c].sumsq += (int64_t)x * (int64_t)x;
						loop_goertzel_step(&stats[c].f1k, x);
						loop_goertzel_step(&stats[c].f500, x);
						loop_goertzel_step(&stats[c].ref700, x);
						loop_goertzel_step(&stats[c].ref1400, x);
					}
				}
			}
			/* PRE_SETTLE/DISCARD blocks: read, then nothing else -- zero
			 * per-sample work, the cheapest possible consumer. */
		}

		if (mic_error) {
			bool dropped = (mic_err_rc == ALP_ERR_IO);
			printf("[loop] %s: alp_audio_in_read %s rc=%d -- recovering (STOP/START), attempt "
			       "%d/2\n",
			       w->name,
			       dropped ? "reports a DROPPED burst (-EIO, overrun)" : "FAILED",
			       (int)mic_err_rc,
			       attempt + 1);
			if (dropped) (*overrun_count)++;
			alp_status_t stop_rc  = alp_audio_in_stop(mic);
			alp_status_t start_rc = alp_audio_in_start(mic);
			printf("[loop] %s: mic recovery alp_audio_in_stop -> %d, alp_audio_in_start -> %d\n",
			       w->name,
			       (int)stop_rc,
			       (int)start_rc);
			if (attempt == 0 && stop_rc == ALP_OK && start_rc == ALP_OK) continue;
			return false;
		}

		for (size_t c = 0; c < MIC_CHANNELS; c++) {
			double p1k            = loop_goertzel_power(&stats[c].f1k);
			double p500           = loop_goertzel_power(&stats[c].f500);
			double p700           = loop_goertzel_power(&stats[c].ref700);
			double p1400          = loop_goertzel_power(&stats[c].ref1400);
			double p_ref          = (p700 + p1400) / 2.0 + 1e-6;
			results[c].f1k_db     = 10.0 * log10((p1k + 1e-6) / p_ref);
			results[c].f500_db    = 10.0 * log10((p500 + 1e-6) / p_ref);
			results[c].ref700_db  = 10.0 * log10((p700 + 1e-6) / p_ref);
			results[c].ref1400_db = 10.0 * log10((p1400 + 1e-6) / p_ref);
			results[c].p2p        = (int32_t)stats[c].maxv - (int32_t)stats[c].minv;
			results[c].dc         = (int32_t)(stats[c].sum / (int64_t)LOOP_CAPTURE_FRAMES);
			results[c].rms = (int32_t)sqrtf((float)stats[c].sumsq / (float)LOOP_CAPTURE_FRAMES);
		}
		return true;
	}
	return false;
}

static int loop_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_LOOPBACK): speakers as mic stimulus ===\n");
	(void)alp_init();

	/* --- Same bring-up as PROBE_LISTEN/PROBE_MELODY: bridge, mux,
	 * AMP_ENABLE, tas2563_init x2, level MIN, configure_i2s x2 ---------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[loop] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[loop] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[loop] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[loop] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[loop] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[loop] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[loop] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[loop] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[loop] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[loop] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[loop] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	/* --- 1. PDM mic open+start, THEN I2S audio-out open+start+ACTIVE ---- */
	alp_audio_in_t *mic    = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = MIC_SAMPLE_RATE_HZ,
	    .channels         = MIC_CHANNELS,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = MIC_FRAMES_PER_BLOCK,
	});
	alp_status_t    mic_rc = (mic != NULL) ? alp_audio_in_start(mic) : alp_last_error();
	bool            mic_ok = (mic != NULL) && (mic_rc == ALP_OK);
	printf("[loop] alp_audio_in_open+start(PDM, U19 LEFT/U20 RIGHT) -> %d\n", (int)mic_rc);

	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[loop] alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
		printf("[loop] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
	}

	static int16_t     out_buf[SOUND_FRAMES_PER_BLOCK * 2u];
	static int16_t     mic_scratch[MIC_FRAMES_PER_BLOCK * MIC_CHANNELS];
	loop_chan_result_t win_r[LOOP_WINDOW_COUNT][MIC_CHANNELS];
	bool               win_ok[LOOP_WINDOW_COUNT] = { false, false, false, false, false, false };
	uint32_t           overrun_count             = 0;

	bool can_run = mic_ok && spk_rc == ALP_OK;
	if (!can_run) {
		printf("[loop] %s -- skipping all windows\n",
		       !mic_ok ? "PDM mic never opened/started" : "I2S3 never started");
	}

	for (size_t w = 0; w < LOOP_WINDOW_COUNT && can_run; w++) {
		if (loop_windows[w].kind == LOOP_TONE) {
			alp_status_t vrc = alp_audio_out_set_volume(spk, loop_windows[w].volume);
			printf("[loop] alp_audio_out_set_volume(%u) [%s] -> %d\n",
			       loop_windows[w].volume,
			       loop_windows[w].name,
			       (int)vrc);
			if (w == 1) loop_print_pwr_ctl("before first tone", amps); /* TONE-1k-VOL16. */
		}

		win_ok[w] = loop_run_window(
		    spk, mic, &loop_windows[w], out_buf, mic_scratch, win_r[w], &overrun_count);
		if (win_ok[w]) {
			loop_print_window(loop_windows[w].name, win_r[w]);
		} else {
			printf("[loop] %s: no clean data -- run is INCONCLUSIVE\n", loop_windows[w].name);
		}

		if (w == 4) loop_print_pwr_ctl("after last tone", amps); /* TONE-500-VOL48. */
	}

	/* --- Stop I2S (the ONLY stop this run makes -- nothing plays after
	 * it), amps to SHUTDOWN -------------------------------------------- */
	alp_status_t stop_rc = (spk != NULL) ? alp_audio_out_stop(spk) : ALP_ERR_NOT_READY;
	printf("[loop] alp_audio_out_stop(I2S3) -> %d\n", (int)stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[loop] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
		tas2563_deinit(&amps[i]);
	}
	if (spk != NULL) alp_audio_out_close(spk);
	if (mic != NULL) alp_audio_in_close(mic);
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	/* --- VERDICT ------------------------------------------------------ */
	bool all_ok = can_run;
	for (size_t w = 0; w < LOOP_WINDOW_COUNT; w++) {
		if (!win_ok[w]) all_ok = false;
	}
	if (!all_ok) {
		printf("[loop] VERDICT mics=INCONCLUSIVE -- %s\n",
		       !can_run ? "mic/I2S never started" : "one or more windows failed to capture");
	} else {
		double f1k_silence  = (win_r[0][0].f1k_db + win_r[0][1].f1k_db) / 2.0;
		double f1k_tone48   = (win_r[2][0].f1k_db + win_r[2][1].f1k_db) / 2.0;
		double f500_silence = (win_r[0][0].f500_db + win_r[0][1].f500_db) / 2.0;
		double f500_tone48  = (win_r[4][0].f500_db + win_r[4][1].f500_db) / 2.0;
		double f1k_at_500   = (win_r[4][0].f1k_db + win_r[4][1].f1k_db) / 2.0;

		static const size_t silence_windows[] = { 0, 3, 5 };
		static const size_t tone_windows[]    = { 2, 4 };

		int32_t silence_p2p = win_r[0][0].p2p;
		for (size_t i = 0; i < ARRAY_SIZE(silence_windows); i++) {
			size_t w2 = silence_windows[i];
			for (size_t c = 0; c < MIC_CHANNELS; c++) {
				if (win_r[w2][c].p2p > silence_p2p) silence_p2p = win_r[w2][c].p2p;
			}
		}
		int32_t tone_p2p_min = win_r[2][0].p2p;
		for (size_t i = 0; i < ARRAY_SIZE(tone_windows); i++) {
			size_t w2 = tone_windows[i];
			for (size_t c = 0; c < MIC_CHANNELS; c++) {
				if (win_r[w2][c].p2p < tone_p2p_min) tone_p2p_min = win_r[w2][c].p2p;
			}
		}

		bool c1 = (f1k_tone48 - f1k_silence) >= 15.0;
		bool c2 = (f500_tone48 - f500_silence) >= 15.0 && (f1k_at_500 - f1k_silence) < 15.0;
		bool c3 = tone_p2p_min >= 2 * silence_p2p;

		printf("[loop] criterion 1 (1kHz rise): TONE-1k-VOL48 f1k=%.1fdB - SILENCE-A f1k=%.1fdB "
		       "= %.1fdB (need >= 15.0) -- %s\n",
		       f1k_tone48,
		       f1k_silence,
		       f1k_tone48 - f1k_silence,
		       c1 ? "PASS" : "FAIL");
		printf("[loop] criterion 2 (500Hz rise, 1kHz stays put): TONE-500-VOL48 f500=%.1fdB - "
		       "SILENCE-A f500=%.1fdB = %.1fdB (need >= 15.0); f1k at TONE-500-VOL48=%.1fdB - "
		       "SILENCE-A f1k=%.1fdB = %.1fdB (need < 15.0) -- %s\n",
		       f500_tone48,
		       f500_silence,
		       f500_tone48 - f500_silence,
		       f1k_at_500,
		       f1k_silence,
		       f1k_at_500 - f1k_silence,
		       c2 ? "PASS" : "FAIL");
		printf("[loop] criterion 3 (p2p above silence): min tone p2p=%d >= 2 * max silence "
		       "p2p=%d (%d) -- %s\n",
		       tone_p2p_min,
		       silence_p2p,
		       2 * silence_p2p,
		       c3 ? "PASS" : "FAIL");

		printf("[loop] VERDICT mics=%s\n", (c1 && c2 && c3) ? "LIVE" : "DEAD");
	}
	printf("[loop] overrun count this run: %u\n", overrun_count);
	printf("[loop] done\n");
	return 0;
}

#endif /* PROBE_LOOPBACK */

/* ================================================================== */
/* PROBE_RESUME -- bench-verify #2146's tas2563_resume() fix          */
/* ================================================================== */
/* Compile-time mode, same shape as the three above: `west build ... --
 * -DPROBE_RESUME=1` replaces main()'s ENTIRE body with resume_main(); with
 * none of PROBE_LISTEN/PROBE_MELODY/PROBE_LOOPBACK/PROBE_RESUME defined,
 * main() is unchanged, and with exactly one of the other three defined,
 * this whole block is preprocessed away -- so none of the four existing
 * builds' object code moves by adding this one. resume_main() duplicates
 * the other three modes' bring-up sequence rather than sharing a helper
 * with any of them, for the same byte-identity reason.
 *
 * Issue #2146: the TAS2563 self-shuts-down (latches a TDM-clock/BOOST_
 * CLOCK/DEVICE_POWER_DOWN fault and drops PWR_CTL's mode field to
 * SHUTDOWN) within about 1 s of its I2S bit clock stopping. chips/tas2563/
 * tas2563.c's new tas2563_resume() clears the latches FIRST (a
 * shutdown-causing fault ties to CLR_INTP_LTCH, not to MODE -- clearing
 * after going ACTIVE would leave a stale latch that re-arms the very
 * shutdown this exists to undo), then sets ACTIVE. This mode benches that
 * fix directly: an ordered start using tas2563_resume() (A), a halt-time
 * measurement (B), a restart WITHOUT resume to reproduce the bug on
 * purpose (C), a resume AFTER that restart to prove the fix recovers it
 * (D), and a short-gap sweep to find the self-heal threshold (E). Volume
 * is 128 here, not the other modes' SOUND_VOL_MAX (48) -- explicitly
 * authorised for this mode only, already heard clean and louder on
 * e1m-aen-evk-03; the other three modes' own volume ceilings are
 * untouched. */
#if defined(PROBE_RESUME)

#define RESUME_VOLUME   128u
#define RESUME_BLOCK_MS (SOUND_FRAMES_PER_BLOCK * 1000u / SOUND_SAMPLE_RATE_HZ) /* 16 ms. */

/* Register addresses, copied verbatim from chips/tas2563/tas2563.c's own
 * (private) TAS2563_REG_* -- see PROBE_LISTEN/PROBE_MELODY/PROBE_LOOPBACK's
 * own *_REG_PWR_CTL above for the same PWR_CTL value/citation.
 * resume_reg_read() mirrors those blocks' own read helpers (same two
 * alp_i2c_write_read() calls, legitimate because tas2563_t.bus/.addr are
 * public fields). */
#define RESUME_REG_PWR_CTL   0x02u /* Power control        (SLASET3D §7.5.4,  p.65). */
#define RESUME_REG_INT_LTCH0 0x24u /* Latched interrupts 0 (SLASET3D §7.5.36, p.82) -- TDM_CLOCK. */
#define RESUME_REG_INT_LTCH3 \
	0x26u /* Latched interrupts 2 (SLASET3D §7.5.38, p.84) -- BOOST_CLOCK. */
#define RESUME_REG_INT_LTCH4 \
	0x27u /* Latched interrupts 3 (SLASET3D §7.5.39, p.84) -- POWER_DOWN. */

static uint8_t resume_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

/* Every "read" step this task asks for: uptime + PWR_CTL/INT_LTCH0/
 * INT_LTCH3/INT_LTCH4 for both amps, one line. */
static void resume_read_full(const char *label, tas2563_t amps[AMP_COUNT])
{
	printf("[resume] %-20s uptime=%u ms  PWR_CTL=0x%02x/0x%02x  INT_LTCH0=0x%02x/0x%02x  "
	       "INT_LTCH3=0x%02x/0x%02x  INT_LTCH4=0x%02x/0x%02x\n",
	       label,
	       k_uptime_get_32(),
	       resume_reg_read(&amps[0], RESUME_REG_PWR_CTL),
	       resume_reg_read(&amps[1], RESUME_REG_PWR_CTL),
	       resume_reg_read(&amps[0], RESUME_REG_INT_LTCH0),
	       resume_reg_read(&amps[1], RESUME_REG_INT_LTCH0),
	       resume_reg_read(&amps[0], RESUME_REG_INT_LTCH3),
	       resume_reg_read(&amps[1], RESUME_REG_INT_LTCH3),
	       resume_reg_read(&amps[0], RESUME_REG_INT_LTCH4),
	       resume_reg_read(&amps[1], RESUME_REG_INT_LTCH4));
}

static void resume_read_pwr_ctl_pair(tas2563_t amps[AMP_COUNT], uint8_t out[AMP_COUNT])
{
	for (size_t c = 0; c < AMP_COUNT; c++)
		out[c] = resume_reg_read(&amps[c], RESUME_REG_PWR_CTL);
}

static alp_status_t resume_write_sine_block(alp_audio_out_t *spk,
                                            int16_t         *buf,
                                            uint32_t        *phase_acc,
                                            uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		float theta =
		    GOERTZEL_TWO_PI * (float)(*phase_acc % samples_per_cycle) / (float)samples_per_cycle;
		int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * sinf(theta));
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* Plays a continuous tone for total_ms (rounded to the nearest
 * RESUME_BLOCK_MS block, keeping I2S continuous -- no stop, no gap), doing
 * a full resume_read_full() at each of checkpoints_ms[] (each rounded to
 * its own nearest block) as it passes. Prints the TONE ON/OFF pair this
 * task asks for so a listener can match what they hear to the log.
 * Returns the write-failure count for this tone span. */
static uint32_t resume_play_tone(alp_audio_out_t *spk,
                                 tas2563_t        amps[AMP_COUNT],
                                 int16_t         *buf,
                                 uint32_t        *phase_acc,
                                 uint32_t         samples_per_cycle,
                                 const char      *label,
                                 uint32_t         total_ms,
                                 const uint32_t  *checkpoints_ms,
                                 size_t           n_checkpoints)
{
	printf("[resume] TONE ON %s (uptime=%u ms)\n", label, k_uptime_get_32());
	uint32_t write_failures = 0;
	uint32_t blocks         = (total_ms + RESUME_BLOCK_MS / 2u) / RESUME_BLOCK_MS;
	size_t   next_cp        = 0;

	for (uint32_t b = 0; b < blocks; b++) {
		if (resume_write_sine_block(spk, buf, phase_acc, samples_per_cycle) != ALP_OK) {
			write_failures++;
		}
		if (next_cp < n_checkpoints) {
			uint32_t cp_block = (checkpoints_ms[next_cp] + RESUME_BLOCK_MS / 2u) / RESUME_BLOCK_MS;
			if (b == cp_block) {
				resume_read_full(label, amps);
				next_cp++;
			}
		}
	}
	printf("[resume] TONE OFF %s (uptime=%u ms)\n", label, k_uptime_get_32());
	return write_failures;
}

static int resume_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_RESUME): bench-verify #2146 ===\n");
	(void)alp_init();

	/* --- Same bring-up as PROBE_LISTEN/PROBE_MELODY/PROBE_LOOPBACK:
	 * bridge, mux, AMP_ENABLE, tas2563_init x2, level MIN, configure_i2s
	 * x2 -- ACTIVE is NOT set here; step A below sets it via
	 * tas2563_resume(), not tas2563_set_mode(). ---------------------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[resume] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[resume] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[resume] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[resume] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[resume] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[resume] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[resume] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[resume] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[resume] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[resume] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[resume] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u];
	uint32_t       phase_acc      = 0;
	uint32_t       write_failures = 0;

	/* --- A. Ordered start: open+start, ONE silent block, tas2563_resume()
	 * on both amps (NOT set_mode ACTIVE), read, 3 s tone, read. -------- */
	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[resume] A: alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_set_volume(spk, RESUME_VOLUME);
	printf("[resume] A: alp_audio_out_set_volume(%u) -> %d\n", RESUME_VOLUME, (int)spk_rc);

	if (spk_rc != ALP_OK) {
		printf("[resume] I2S3 did not start -- aborting before touching the amps\n");
	} else {
		static int16_t silence_block[SOUND_FRAMES_PER_BLOCK * 2u]; /* memset below, once. */
		for (size_t i = 0; i < ARRAY_SIZE(silence_block); i++)
			silence_block[i] = 0;
		if (alp_audio_out_write(spk, silence_block, SOUND_FRAMES_PER_BLOCK, NULL, 200u) != ALP_OK) {
			write_failures++;
		}

		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t rrc = tas2563_resume(&amps[i]);
			printf("[resume] A: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
		}
		resume_read_full("A: after resume", amps);

		const uint32_t no_checkpoints[1] = { 0 };
		write_failures += resume_play_tone(spk,
		                                   amps,
		                                   tone_buf,
		                                   &phase_acc,
		                                   SOUND_SAMPLE_RATE_HZ / 1000u,
		                                   "A",
		                                   3000u,
		                                   no_checkpoints,
		                                   0);
		resume_read_full("A: after tone", amps);

		/* --- B. Halt-time measurement: stop, poll PWR_CTL every 100 ms
		 * for 3 s. -------------------------------------------------- */
		alp_status_t stop_rc = alp_audio_out_stop(spk);
		printf("[resume] B: alp_audio_out_stop -> %d\n", (int)stop_rc);
		bool     shutdown_seen[AMP_COUNT]  = { false, false };
		uint32_t shutdown_at_ms[AMP_COUNT] = { 0, 0 };
		for (uint32_t elapsed = 0; elapsed <= 3000u; elapsed += 100u) {
			uint8_t pwr[AMP_COUNT];
			resume_read_pwr_ctl_pair(amps, pwr);
			printf("[resume] B: +%u ms  uptime=%u ms  PWR_CTL=0x%02x/0x%02x\n",
			       elapsed,
			       k_uptime_get_32(),
			       pwr[0],
			       pwr[1]);
			for (size_t c = 0; c < AMP_COUNT; c++) {
				if (!shutdown_seen[c] && pwr[c] == 0x0eu) {
					shutdown_seen[c]  = true;
					shutdown_at_ms[c] = elapsed;
				}
			}
			if (elapsed < 3000u) k_msleep(100);
		}
		for (size_t c = 0; c < AMP_COUNT; c++) {
			if (shutdown_seen[c]) {
				printf("[resume] shutdown observed at +%u ms after stop (0x%02x)\n",
				       shutdown_at_ms[c],
				       amp_addrs[c]);
			} else {
				printf("[resume] shutdown observed at +ms after stop (0x%02x): never\n",
				       amp_addrs[c]);
			}
		}

		/* --- C. Restart WITHOUT resume -- reproduces the bug on purpose. */
		printf("[resume] C: restart without resume\n");
		alp_status_t restart_rc = alp_audio_out_start(spk);
		printf("[resume] C: alp_audio_out_start -> %d\n", (int)restart_rc);
		const uint32_t c_checkpoints[2] = { 1000u, 2500u };
		write_failures += resume_play_tone(spk,
		                                   amps,
		                                   tone_buf,
		                                   &phase_acc,
		                                   SOUND_SAMPLE_RATE_HZ / 1000u,
		                                   "C",
		                                   3000u,
		                                   c_checkpoints,
		                                   ARRAY_SIZE(c_checkpoints));

		/* --- D. Resume after restart -- I2S is already running from C. */
		printf("[resume] D: after tas2563_resume\n");
		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t rrc = tas2563_resume(&amps[i]);
			printf("[resume] D: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
		}
		resume_read_full("D: after resume", amps);
		const uint32_t d_checkpoints[2] = { 1000u, 2500u };
		write_failures += resume_play_tone(spk,
		                                   amps,
		                                   tone_buf,
		                                   &phase_acc,
		                                   SOUND_SAMPLE_RATE_HZ / 1000u,
		                                   "D",
		                                   3000u,
		                                   d_checkpoints,
		                                   ARRAY_SIZE(d_checkpoints));

		/* --- E. Short-gap sweep, no resume -- self-heal threshold. ---- */
		static const uint32_t gaps_ms[] = { 200u, 500u, 1500u };
		for (size_t g = 0; g < ARRAY_SIZE(gaps_ms); g++) {
			alp_status_t gap_stop_rc = alp_audio_out_stop(spk);
			k_msleep(gaps_ms[g]);
			alp_status_t gap_start_rc = alp_audio_out_start(spk);
			printf("[resume] E: gap=%u ms stop -> %d, start -> %d\n",
			       gaps_ms[g],
			       (int)gap_stop_rc,
			       (int)gap_start_rc);

			char label[24];
			snprintf(label, sizeof(label), "E gap=%u", gaps_ms[g]);
			uint32_t cp_1s         = 1000u;
			uint32_t before_uptime = k_uptime_get_32();
			(void)before_uptime;
			write_failures += resume_play_tone(spk,
			                                   amps,
			                                   tone_buf,
			                                   &phase_acc,
			                                   SOUND_SAMPLE_RATE_HZ / 1000u,
			                                   label,
			                                   2000u,
			                                   &cp_1s,
			                                   1);

			uint8_t pwr[AMP_COUNT];
			resume_read_pwr_ctl_pair(amps, pwr);
			printf("[resume] gap=%u PWR_CTL=0x%02x/0x%02x\n", gaps_ms[g], pwr[0], pwr[1]);

			for (size_t i = 0; i < AMP_COUNT; i++) {
				alp_status_t rrc = tas2563_resume(&amps[i]);
				printf("[resume] E: tas2563_resume(0x%02x) -> %d [re-arm for next gap]\n",
				       amp_addrs[i],
				       (int)rrc);
			}
		}
	}

	/* --- F. Teardown. --------------------------------------------------- */
	alp_status_t final_stop_rc = (spk != NULL) ? alp_audio_out_stop(spk) : ALP_ERR_NOT_READY;
	printf("[resume] F: alp_audio_out_stop -> %d\n", (int)final_stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[resume] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
		tas2563_deinit(&amps[i]);
	}
	if (spk != NULL) alp_audio_out_close(spk);
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	printf("[resume] total alp_audio_out_write() failures this run: %u\n", write_failures);
	printf("[resume] done\n");
	return 0;
}

#endif /* PROBE_RESUME */

/* ================================================================== */
/* PROBE_RESUME2 -- PROBE_RESUME + mic-measured pitch, silence-separated */
/* ================================================================== */
/* Compile-time mode, same shape as the four above: `west build ... --
 * -DPROBE_RESUME2=1` replaces main()'s ENTIRE body with resume2_main();
 * with none of PROBE_LISTEN/PROBE_MELODY/PROBE_LOOPBACK/PROBE_RESUME/
 * PROBE_RESUME2 defined, main() is unchanged, and with exactly one of the
 * other four defined, this whole block is preprocessed away -- so none of
 * the four existing builds' object code moves by adding this one.
 * resume2_main() duplicates the other four modes' bring-up rather than
 * sharing a helper with any of them, for the same byte-identity reason.
 *
 * PROBE_RESUME (this file, above) proved #2146's fix in registers, but
 * the maintainer heard "different pitches" across its six back-to-back
 * 1 kHz tones. Every tone in PROBE_RESUME asks for the SAME 1000 Hz (see
 * resume_play_tone()'s callers -- samples_per_cycle is always
 * SOUND_SAMPLE_RATE_HZ/1000u), so if the pitch genuinely changed it did
 * not come from this app's C code; this mode adds an OBJECTIVE measurement
 * (the PDM mics, proven live by PROBE_LOOPBACK: 1000 Hz and 500 Hz both
 * >50 dB above silence there) plus >=3 s of silence between every tone so
 * a listener can tell the tones apart by ear too.
 *
 * Same A-F sequence as PROBE_RESUME (ordered start with tas2563_resume(),
 * a halt-time measurement, restart WITHOUT resume to reproduce #2146 on
 * purpose, resume after that restart, a short-gap self-heal sweep, then
 * teardown) at the SAME SOUND_VOL_MAX-independent RESUME_VOLUME (128) and
 * the SAME PWR_CTL/INT_LTCH0/INT_LTCH3/INT_LTCH4 checkpoints -- only the
 * pacing (silence added) and the mic measurement are new. */
#if defined(PROBE_RESUME2)

#include <string.h> /* memset(), used by the silence writer below. */

#define RESUME2_VOLUME   128u
#define RESUME2_BLOCK_MS (SOUND_FRAMES_PER_BLOCK * 1000u / SOUND_SAMPLE_RATE_HZ) /* 16 ms. */

/* Register addresses, copied verbatim from chips/tas2563/tas2563.c's own
 * (private) TAS2563_REG_* -- see PROBE_RESUME's RESUME_REG_* above for the
 * same values/citations. resume2_reg_read() mirrors that block's own read
 * helper. */
#define RESUME2_REG_PWR_CTL 0x02u /* Power control        (SLASET3D §7.5.4,  p.65). */
#define RESUME2_REG_INT_LTCH0 \
	0x24u /* Latched interrupts 0 (SLASET3D §7.5.36, p.82) -- TDM_CLOCK. */
#define RESUME2_REG_INT_LTCH3 \
	0x26u /* Latched interrupts 2 (SLASET3D §7.5.38, p.84) -- BOOST_CLOCK. */
#define RESUME2_REG_INT_LTCH4 \
	0x27u /* Latched interrupts 3 (SLASET3D §7.5.39, p.84) -- POWER_DOWN. */

static uint8_t resume2_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

static void resume2_read_full(const char *label, tas2563_t amps[AMP_COUNT])
{
	printf("[r2] %-20s uptime=%u ms  PWR_CTL=0x%02x/0x%02x  INT_LTCH0=0x%02x/0x%02x  "
	       "INT_LTCH3=0x%02x/0x%02x  INT_LTCH4=0x%02x/0x%02x\n",
	       label,
	       k_uptime_get_32(),
	       resume2_reg_read(&amps[0], RESUME2_REG_PWR_CTL),
	       resume2_reg_read(&amps[1], RESUME2_REG_PWR_CTL),
	       resume2_reg_read(&amps[0], RESUME2_REG_INT_LTCH0),
	       resume2_reg_read(&amps[1], RESUME2_REG_INT_LTCH0),
	       resume2_reg_read(&amps[0], RESUME2_REG_INT_LTCH3),
	       resume2_reg_read(&amps[1], RESUME2_REG_INT_LTCH3),
	       resume2_reg_read(&amps[0], RESUME2_REG_INT_LTCH4),
	       resume2_reg_read(&amps[1], RESUME2_REG_INT_LTCH4));
}

static void resume2_read_pwr_ctl_pair(tas2563_t amps[AMP_COUNT], uint8_t out[AMP_COUNT])
{
	for (size_t c = 0; c < AMP_COUNT; c++)
		out[c] = resume2_reg_read(&amps[c], RESUME2_REG_PWR_CTL);
}

/* --- I2S TX: a continuous 1 kHz sine or silence, one block at a time --- */
static alp_status_t resume2_write_sine_block(alp_audio_out_t *spk,
                                             int16_t         *buf,
                                             uint32_t        *phase_acc,
                                             uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		float theta =
		    GOERTZEL_TWO_PI * (float)(*phase_acc % samples_per_cycle) / (float)samples_per_cycle;
		int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * sinf(theta));
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

static alp_status_t resume2_write_silence_block(alp_audio_out_t *spk, int16_t *buf)
{
	memset(buf, 0, SOUND_FRAMES_PER_BLOCK * 2u * sizeof(int16_t));
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* tone_freq_hz == 0 means silence; phase_acc is the SAME variable across
 * every call this run -- see resume2_main()'s "tone generator" print and
 * its own comment for why that makes this generator phase-continuous. */
static alp_status_t
resume2_write_block(alp_audio_out_t *spk, int16_t *buf, uint32_t tone_freq_hz, uint32_t *phase_acc)
{
	if (tone_freq_hz == 0u) return resume2_write_silence_block(spk, buf);
	return resume2_write_sine_block(spk, buf, phase_acc, SOUND_SAMPLE_RATE_HZ / tone_freq_hz);
}

/* --- Mic-side pitch measurement: 200-4000 Hz Goertzel scan in 25 Hz steps,
 * ch0 only, run ONCE after capture (never per-block/per-sample) -- see
 * r2_scan() below. --------------------------------------------------- */
#define R2_DISCARD_BLOCKS 19u /* 304 ms -- decimator/restart settle, discarded. */
#define R2_CAPTURE_BLOCKS 31u /* 496 ms -- "about 0.5 s". */
#define R2_MEASURE_BLOCKS (R2_DISCARD_BLOCKS + R2_CAPTURE_BLOCKS)    /* 50 blocks, 800 ms. */
#define R2_CAPTURE_FRAMES (R2_CAPTURE_BLOCKS * MIC_FRAMES_PER_BLOCK) /* 23808. */
#define R2_SCAN_MIN_HZ    200u
#define R2_SCAN_MAX_HZ    4000u
#define R2_SCAN_STEP_HZ   25u
#define R2_SCAN_BINS      ((R2_SCAN_MAX_HZ - R2_SCAN_MIN_HZ) / R2_SCAN_STEP_HZ + 1u) /* 153. */
#define R2_SCAN_BIN_1000HZ \
	((1000u - R2_SCAN_MIN_HZ) / R2_SCAN_STEP_HZ) /* 32 -- exact, 200+32*25=1000. */

/* ch0-only raw capture buffer: 23808 * 2 bytes = 46 608 bytes. static: off
 * resume2_measure_window()'s stack. Per-sample capture work above (in
 * resume2_measure_window()) is a plain copy plus int16 min/max -- integer,
 * cheap; the float Goertzel scan below runs on this buffer ONCE the
 * capture loop (and therefore every alp_audio_in_read() for this window)
 * has already finished, so it can never be the reason a read starves the
 * mic's slab -- the same "cheap consumer" rule this app's original
 * acoustic-loopback fix and PROBE_LOOPBACK both already apply. */
static int16_t r2_ch0_buf[R2_CAPTURE_FRAMES];

typedef struct {
	float coeff;
	float s_prev;
	float s_prev2;
} r2_goertzel_t;

static void
r2_goertzel_reset(r2_goertzel_t *g, uint32_t freq_hz, uint32_t n_samples, uint32_t fs_hz)
{
	float k     = (float)n_samples * (float)freq_hz / (float)fs_hz;
	float omega = GOERTZEL_TWO_PI * k / (float)n_samples;
	g->coeff    = 2.0f * cosf(omega);
	g->s_prev   = 0.0f;
	g->s_prev2  = 0.0f;
}

static inline void r2_goertzel_step(r2_goertzel_t *g, float x)
{
	float s    = x + g->coeff * g->s_prev - g->s_prev2;
	g->s_prev2 = g->s_prev;
	g->s_prev  = s;
}

static float r2_goertzel_power(const r2_goertzel_t *g)
{
	return g->s_prev * g->s_prev + g->s_prev2 * g->s_prev2 - g->coeff * g->s_prev * g->s_prev2;
}

/* Scans R2_SCAN_BINS Goertzel bins (200-4000 Hz, 25 Hz steps) over the
 * first n_captured samples of r2_ch0_buf[]. Finds the peak bin and
 * expresses it (and the 1000 Hz bin) in dB above the MEDIAN bin power --
 * an insertion sort over 153 floats, done once, is cheap enough not to
 * matter here. */
static void
r2_scan(uint32_t n_captured, uint32_t *peak_hz_out, double *peak_db_out, double *f1000_db_out)
{
	float powers[R2_SCAN_BINS];
	for (uint32_t i = 0; i < R2_SCAN_BINS; i++) {
		r2_goertzel_t g;
		r2_goertzel_reset(&g, R2_SCAN_MIN_HZ + i * R2_SCAN_STEP_HZ, n_captured, MIC_SAMPLE_RATE_HZ);
		for (uint32_t s = 0; s < n_captured; s++)
			r2_goertzel_step(&g, (float)r2_ch0_buf[s]);
		powers[i] = r2_goertzel_power(&g);
	}

	uint32_t peak_i = 0;
	float    peak_p = powers[0];
	for (uint32_t i = 1; i < R2_SCAN_BINS; i++) {
		if (powers[i] > peak_p) {
			peak_p = powers[i];
			peak_i = i;
		}
	}

	float sorted[R2_SCAN_BINS];
	memcpy(sorted, powers, sizeof(powers));
	for (uint32_t i = 1; i < R2_SCAN_BINS; i++) {
		float key = sorted[i];
		int   j   = (int)i - 1;
		while (j >= 0 && sorted[j] > key) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}
	float median_p = sorted[R2_SCAN_BINS / 2u] + 1e-6f;

	*peak_hz_out  = R2_SCAN_MIN_HZ + peak_i * R2_SCAN_STEP_HZ;
	*peak_db_out  = 10.0 * log10((double)(peak_p + 1e-6f) / (double)median_p);
	*f1000_db_out = 10.0 * log10((double)(powers[R2_SCAN_BIN_1000HZ] + 1e-6f) / (double)median_p);
}

typedef struct {
	uint32_t peak_hz;
	double   peak_db;
	double   f1000_db;
	int32_t  p2p;
} r2_measure_result_t;

/* Runs R2_MEASURE_BLOCKS I2S blocks (tone if tone_freq_hz != 0, else
 * silence), reading one PDM block per I2S block in lock-step. The first
 * R2_DISCARD_BLOCKS are read-and-discarded (decimator/restart settle,
 * zero per-sample work); the next R2_CAPTURE_BLOCKS are copied into
 * r2_ch0_buf (ch0 only) plus int16 min/max, also cheap/integer. Retries
 * once via STOP/START on a dmic -EIO (the round-4b overrun contract,
 * same as PROBE_LOOPBACK), counting it into *overrun_count. Returns false
 * only if the retry also fails. */
static bool resume2_measure_window(alp_audio_out_t     *spk,
                                   alp_audio_in_t      *mic,
                                   int16_t             *out_buf,
                                   int16_t             *mic_scratch,
                                   uint32_t             tone_freq_hz,
                                   uint32_t            *phase_acc,
                                   uint32_t            *write_failures,
                                   uint32_t            *overrun_count,
                                   r2_measure_result_t *result)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		int16_t      minv       = INT16_MAX;
		int16_t      maxv       = INT16_MIN;
		uint32_t     cap_n      = 0;
		bool         mic_error  = false;
		alp_status_t mic_err_rc = ALP_OK;

		for (uint32_t b = 0; b < R2_MEASURE_BLOCKS; b++) {
			if (resume2_write_block(spk, out_buf, tone_freq_hz, phase_acc) != ALP_OK) {
				(*write_failures)++;
			}

			size_t       got = 0;
			alp_status_t rrc = alp_audio_in_read(
			    mic, mic_scratch, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
			if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
				mic_error  = true;
				mic_err_rc = rrc;
				break;
			}

			if (b >= R2_DISCARD_BLOCKS) {
				for (uint32_t f = 0; f < MIC_FRAMES_PER_BLOCK; f++) {
					int16_t x = mic_scratch[f * MIC_CHANNELS + 0];
					if (x < minv) minv = x;
					if (x > maxv) maxv = x;
					r2_ch0_buf[cap_n++] = x;
				}
			}
		}

		if (mic_error) {
			bool dropped = (mic_err_rc == ALP_ERR_IO);
			printf("[r2] mic %s rc=%d -- recovering (STOP/START), attempt %d/2\n",
			       dropped ? "reports a DROPPED burst (-EIO, overrun)" : "read FAILED",
			       (int)mic_err_rc,
			       attempt + 1);
			if (dropped) (*overrun_count)++;
			alp_status_t stop_rc  = alp_audio_in_stop(mic);
			alp_status_t start_rc = alp_audio_in_start(mic);
			printf("[r2] mic recovery alp_audio_in_stop -> %d, alp_audio_in_start -> %d\n",
			       (int)stop_rc,
			       (int)start_rc);
			if (attempt == 0 && stop_rc == ALP_OK && start_rc == ALP_OK) continue;
			return false;
		}

		r2_scan(cap_n, &result->peak_hz, &result->peak_db, &result->f1000_db);
		result->p2p = (int32_t)maxv - (int32_t)minv;
		return true;
	}
	return false;
}

/* Keeps writing tone/silence blocks and draining one PDM block per I2S
 * block (best-effort STOP/START on an overrun -- not a measurement, so no
 * retry-the-whole-span logic), with no per-sample work at all -- used for
 * every span this mode does NOT need to measure (the silence padding
 * beyond a window's own SILENCE-before-N measurement, and the rest of a
 * tone after its own TONE-n measurement). Returns the write-failure
 * count. */
static uint32_t resume2_drain_blocks(alp_audio_out_t *spk,
                                     alp_audio_in_t  *mic,
                                     int16_t         *out_buf,
                                     int16_t         *mic_scratch,
                                     uint32_t         tone_freq_hz,
                                     uint32_t         blocks,
                                     uint32_t        *phase_acc,
                                     uint32_t        *overrun_count)
{
	uint32_t write_failures = 0;
	for (uint32_t b = 0; b < blocks; b++) {
		if (resume2_write_block(spk, out_buf, tone_freq_hz, phase_acc) != ALP_OK) write_failures++;
		size_t       got = 0;
		alp_status_t rrc =
		    alp_audio_in_read(mic, mic_scratch, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
		if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
			if (rrc == ALP_ERR_IO) (*overrun_count)++;
			(void)alp_audio_in_stop(mic);
			(void)alp_audio_in_start(mic);
		}
	}
	return write_failures;
}

/* >=3 s of silence before tone n (this task's requirement 1), with the
 * SILENCE-before-n mic measurement (requirement 2) inside its first
 * R2_MEASURE_BLOCKS (800 ms) -- the remaining ~2.2 s is drained (cheap,
 * no measurement). 188 blocks = 3008 ms is the smallest block count
 * >= 3000 ms (187*16=2992 < 3000). Returns this call's OWN write-failure
 * count (not accumulated) so callers that need to gate tas2563_resume()
 * on "did every priming write actually succeed" (the #2146 round-2
 * pattern examples/aen/aen-evk-demo/src/main.c now uses) can check it. */
#define R2_SILENCE_BLOCKS 188u /* 3008 ms, >= the required 3000 ms. */

static uint32_t resume2_pre_tone_silence(alp_audio_out_t *spk,
                                         alp_audio_in_t  *mic,
                                         int16_t         *out_buf,
                                         int16_t         *mic_scratch,
                                         uint32_t        *phase_acc,
                                         unsigned         n,
                                         uint32_t        *overrun_count)
{
	uint32_t            write_failures = 0;
	r2_measure_result_t mres;
	bool                ok = resume2_measure_window(
	    spk, mic, out_buf, mic_scratch, 0u, phase_acc, &write_failures, overrun_count, &mres);
	if (ok) {
		printf(
		    "[r2] SILENCE-before-%u mic peak=%u Hz  peak_dB=%.1f\n", n, mres.peak_hz, mres.peak_db);
	} else {
		printf("[r2] SILENCE-before-%u mic measurement FAILED (overrun recovery exhausted)\n", n);
	}
	if (R2_SILENCE_BLOCKS > R2_MEASURE_BLOCKS) {
		write_failures += resume2_drain_blocks(spk,
		                                       mic,
		                                       out_buf,
		                                       mic_scratch,
		                                       0u,
		                                       R2_SILENCE_BLOCKS - R2_MEASURE_BLOCKS,
		                                       phase_acc,
		                                       overrun_count);
	}
	return write_failures;
}

/* TONE n/6 ON <label> ... TONE n/6 OFF -- this task's requirement 1's
 * exact print pair. The mic measurement (requirement 2) runs in the
 * tone's own first R2_MEASURE_BLOCKS (800 ms); any read-full()
 * checkpoints (requirement 3, same as PROBE_RESUME's A/C/D/E) are timed
 * from TONE START, so they land correctly in the "remainder" span after
 * the measurement window. Returns this call's own write-failure count. */
static uint32_t resume2_play_tone(alp_audio_out_t *spk,
                                  alp_audio_in_t  *mic,
                                  tas2563_t        amps[AMP_COUNT],
                                  int16_t         *out_buf,
                                  int16_t         *mic_scratch,
                                  uint32_t        *phase_acc,
                                  const char      *label,
                                  unsigned         n,
                                  uint32_t         total_ms,
                                  const uint32_t  *checkpoints_ms,
                                  size_t           n_checkpoints,
                                  uint32_t        *overrun_count)
{
	printf("[r2] TONE %u/6 ON %s (expect 1000 Hz)\n", n, label);
	uint32_t write_failures = 0;

	r2_measure_result_t mres;
	bool                ok = resume2_measure_window(
	    spk, mic, out_buf, mic_scratch, 1000u, phase_acc, &write_failures, overrun_count, &mres);
	if (ok) {
		printf("[r2] TONE %u mic peak=%u Hz  peak_dB=%.1f  f1000_dB=%.1f  p2p=%d\n",
		       n,
		       mres.peak_hz,
		       mres.peak_db,
		       mres.f1000_db,
		       mres.p2p);
	} else {
		printf("[r2] TONE %u mic measurement FAILED (overrun recovery exhausted)\n", n);
	}

	uint32_t total_blocks   = (total_ms + RESUME2_BLOCK_MS / 2u) / RESUME2_BLOCK_MS;
	uint32_t elapsed_blocks = R2_MEASURE_BLOCKS;
	size_t   next_cp        = 0;
	while (elapsed_blocks < total_blocks) {
		uint32_t run_blocks = total_blocks - elapsed_blocks;
		if (next_cp < n_checkpoints) {
			uint32_t cp_block =
			    (checkpoints_ms[next_cp] + RESUME2_BLOCK_MS / 2u) / RESUME2_BLOCK_MS;
			if (cp_block > elapsed_blocks && cp_block < total_blocks) {
				run_blocks = cp_block - elapsed_blocks;
			}
		}
		write_failures += resume2_drain_blocks(
		    spk, mic, out_buf, mic_scratch, 1000u, run_blocks, phase_acc, overrun_count);
		elapsed_blocks += run_blocks;

		if (next_cp < n_checkpoints) {
			uint32_t cp_block =
			    (checkpoints_ms[next_cp] + RESUME2_BLOCK_MS / 2u) / RESUME2_BLOCK_MS;
			if (elapsed_blocks == cp_block) {
				resume2_read_full(label, amps);
				next_cp++;
			}
		}
	}
	printf("[r2] TONE %u/6 OFF\n", n);
	return write_failures;
}

static int resume2_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_RESUME2): #2146 + mic-measured pitch ===\n");
	(void)alp_init();

	/* --- Same bring-up as the other PROBE_* modes: bridge, mux,
	 * AMP_ENABLE, tas2563_init x2, level MIN, configure_i2s x2. -------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[r2] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[r2] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[r2] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[r2] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[r2] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[r2] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[r2] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[r2] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[r2] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[r2] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[r2] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	/* --- PDM mic open+start BEFORE phase A, kept running throughout, as
	 * PROBE_LOOPBACK does -- proven live there (1000/500 Hz each >50 dB
	 * above silence). ---------------------------------------------------- */
	alp_audio_in_t *mic    = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = MIC_SAMPLE_RATE_HZ,
	    .channels         = MIC_CHANNELS,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = MIC_FRAMES_PER_BLOCK,
	});
	alp_status_t    mic_rc = (mic != NULL) ? alp_audio_in_start(mic) : alp_last_error();
	bool            mic_ok = (mic != NULL) && (mic_rc == ALP_OK);
	printf("[r2] alp_audio_in_open+start(PDM, U19 LEFT/U20 RIGHT) -> %d\n", (int)mic_rc);

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u];
	static int16_t mic_scratch[MIC_FRAMES_PER_BLOCK * MIC_CHANNELS];
	uint32_t       phase_acc      = 0;
	uint32_t       write_failures = 0;
	uint32_t       overrun_count  = 0;

	/* This task's requirement 4: the tone generator's own parameters,
	 * printed once. phase_acc above is declared ONCE for this entire run
	 * and threaded by pointer through every resume2_write_sine_block()
	 * call (inside resume2_measure_window()/resume2_drain_blocks(), for
	 * every tone AND every silence span) -- it is never reset per block
	 * or per call, so this generator is PHASE-CONTINUOUS across the whole
	 * run, not just within one tone. A phase reset per block would cause
	 * an audible buzz/click train at the block rate (16 ms, 62.5 Hz),
	 * not a pitch change -- reported here either way, per this task's ask. */
	printf("[r2] tone generator: sample_rate=%u Hz  frames_per_block=%u  tone_freq=1000 Hz  "
	       "samples_per_cycle=%u  phase_continuous=YES (single phase_acc threaded through every "
	       "write this run, never reset)\n",
	       SOUND_SAMPLE_RATE_HZ,
	       SOUND_FRAMES_PER_BLOCK,
	       SOUND_SAMPLE_RATE_HZ / 1000u);

	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[r2] alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_set_volume(spk, RESUME2_VOLUME);
	printf("[r2] alp_audio_out_set_volume(%u) -> %d\n", RESUME2_VOLUME, (int)spk_rc);

	if (!mic_ok || spk_rc != ALP_OK) {
		printf("[r2] %s -- aborting before touching the amps\n",
		       !mic_ok ? "PDM mic never opened/started" : "I2S3 did not start");
	} else {
		/* --- A. Ordered start: >=3 s silence (+ SILENCE-before-1
		 * measurement), tas2563_resume() gated on that priming actually
		 * succeeding (the #2146 round-2 pattern), read, TONE 1/6, read. */
		uint32_t a_fail = resume2_pre_tone_silence(
		    spk, mic, tone_buf, mic_scratch, &phase_acc, 1u, &overrun_count);
		write_failures += a_fail;
		if (a_fail == 0) {
			for (size_t i = 0; i < AMP_COUNT; i++) {
				alp_status_t rrc = tas2563_resume(&amps[i]);
				printf("[r2] A: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
			}
		} else {
			printf("[r2] A: priming silence had %u write failure(s) -- skipping "
			       "tas2563_resume(), amps left in shutdown\n",
			       a_fail);
		}
		resume2_read_full("A: after resume", amps);
		const uint32_t no_checkpoints[1] = { 0 };
		write_failures += resume2_play_tone(spk,
		                                    mic,
		                                    amps,
		                                    tone_buf,
		                                    mic_scratch,
		                                    &phase_acc,
		                                    "A",
		                                    1u,
		                                    3000u,
		                                    no_checkpoints,
		                                    0,
		                                    &overrun_count);
		resume2_read_full("A: after tone", amps);

		/* --- B. Halt-time measurement: stop, poll PWR_CTL every 50 ms
		 * for 3 s (cheap enough to drop from PROBE_RESUME's 100 ms). --- */
		alp_status_t stop_rc = alp_audio_out_stop(spk);
		printf("[r2] B: alp_audio_out_stop -> %d\n", (int)stop_rc);
		bool     shutdown_seen[AMP_COUNT]  = { false, false };
		uint32_t shutdown_at_ms[AMP_COUNT] = { 0, 0 };
		for (uint32_t elapsed = 0; elapsed <= 3000u; elapsed += 50u) {
			uint8_t pwr[AMP_COUNT];
			resume2_read_pwr_ctl_pair(amps, pwr);
			printf("[r2] B: +%u ms  uptime=%u ms  PWR_CTL=0x%02x/0x%02x\n",
			       elapsed,
			       k_uptime_get_32(),
			       pwr[0],
			       pwr[1]);
			for (size_t c = 0; c < AMP_COUNT; c++) {
				if (!shutdown_seen[c] && pwr[c] == 0x0eu) {
					shutdown_seen[c]  = true;
					shutdown_at_ms[c] = elapsed;
				}
			}
			if (elapsed < 3000u) k_msleep(50);
		}
		for (size_t c = 0; c < AMP_COUNT; c++) {
			if (shutdown_seen[c]) {
				printf("[r2] shutdown observed at +%u ms after stop (0x%02x)\n",
				       shutdown_at_ms[c],
				       amp_addrs[c]);
			} else {
				printf("[r2] shutdown observed at +ms after stop (0x%02x): never\n", amp_addrs[c]);
			}
		}

		/* --- C. Restart WITHOUT resume -- reproduces #2146 on purpose. */
		alp_status_t restart_rc = alp_audio_out_start(spk);
		printf("[r2] C: restart without resume, alp_audio_out_start -> %d\n", (int)restart_rc);
		write_failures += resume2_pre_tone_silence(
		    spk, mic, tone_buf, mic_scratch, &phase_acc, 2u, &overrun_count);
		const uint32_t c_checkpoints[2] = { 1000u, 2500u };
		write_failures += resume2_play_tone(spk,
		                                    mic,
		                                    amps,
		                                    tone_buf,
		                                    mic_scratch,
		                                    &phase_acc,
		                                    "C",
		                                    2u,
		                                    3000u,
		                                    c_checkpoints,
		                                    ARRAY_SIZE(c_checkpoints),
		                                    &overrun_count);

		/* --- D. Resume after C -- I2S already running; the silence
		 * before this tone is written as zero blocks, NEVER a stop. --- */
		uint32_t d_fail = resume2_pre_tone_silence(
		    spk, mic, tone_buf, mic_scratch, &phase_acc, 3u, &overrun_count);
		write_failures += d_fail;
		if (d_fail == 0) {
			for (size_t i = 0; i < AMP_COUNT; i++) {
				alp_status_t rrc = tas2563_resume(&amps[i]);
				printf("[r2] D: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
			}
		} else {
			printf("[r2] D: priming silence had %u write failure(s) -- skipping "
			       "tas2563_resume(), amps left as-is\n",
			       d_fail);
		}
		resume2_read_full("D: after resume", amps);
		const uint32_t d_checkpoints[2] = { 1000u, 2500u };
		write_failures += resume2_play_tone(spk,
		                                    mic,
		                                    amps,
		                                    tone_buf,
		                                    mic_scratch,
		                                    &phase_acc,
		                                    "D",
		                                    3u,
		                                    3000u,
		                                    d_checkpoints,
		                                    ARRAY_SIZE(d_checkpoints),
		                                    &overrun_count);

		/* --- E. Short-gap sweep, no resume -- self-heal threshold. Each
		 * gap keeps ITS OWN stop semantics (a real stop, per this task's
		 * ask); the >=3 s of silence this task also requires before each
		 * gap's tone is added AFTER the restart, as zero-block writes. */
		static const uint32_t gaps_ms[] = { 200u, 500u, 1500u };
		for (size_t g = 0; g < ARRAY_SIZE(gaps_ms); g++) {
			unsigned     n           = 4u + (unsigned)g;
			alp_status_t gap_stop_rc = alp_audio_out_stop(spk);
			k_msleep(gaps_ms[g]);
			alp_status_t gap_start_rc = alp_audio_out_start(spk);
			printf("[r2] E: gap=%u ms stop -> %d, start -> %d\n",
			       gaps_ms[g],
			       (int)gap_stop_rc,
			       (int)gap_start_rc);

			write_failures += resume2_pre_tone_silence(
			    spk, mic, tone_buf, mic_scratch, &phase_acc, n, &overrun_count);

			char label[24];
			snprintf(label, sizeof(label), "E gap=%u", gaps_ms[g]);
			const uint32_t e_checkpoint[1] = { 1000u };
			write_failures += resume2_play_tone(spk,
			                                    mic,
			                                    amps,
			                                    tone_buf,
			                                    mic_scratch,
			                                    &phase_acc,
			                                    label,
			                                    n,
			                                    2000u,
			                                    e_checkpoint,
			                                    1,
			                                    &overrun_count);

			uint8_t pwr[AMP_COUNT];
			resume2_read_pwr_ctl_pair(amps, pwr);
			printf("[r2] gap=%u PWR_CTL=0x%02x/0x%02x\n", gaps_ms[g], pwr[0], pwr[1]);

			for (size_t i = 0; i < AMP_COUNT; i++) {
				alp_status_t rrc = tas2563_resume(&amps[i]);
				printf("[r2] E: tas2563_resume(0x%02x) -> %d [re-arm for next gap]\n",
				       amp_addrs[i],
				       (int)rrc);
			}
			/* This task's requirement 3: "After each E re-arm, also read
			 * PWR_CTL; the last run didn't print it." */
			resume2_read_full("E: after re-arm", amps);
		}
	}

	/* --- F. Teardown. ---------------------------------------------------- */
	alp_status_t final_stop_rc = (spk != NULL) ? alp_audio_out_stop(spk) : ALP_ERR_NOT_READY;
	printf("[r2] F: alp_audio_out_stop -> %d\n", (int)final_stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[r2] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
		tas2563_deinit(&amps[i]);
	}
	if (spk != NULL) alp_audio_out_close(spk);
	if (mic != NULL) alp_audio_in_close(mic);
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	printf("[r2] total alp_audio_out_write() failures this run: %u\n", write_failures);
	printf("[r2] total mic overrun count this run: %u\n", overrun_count);
	printf("[r2] done\n");
	return 0;
}

#endif /* PROBE_RESUME2 */

/* ================================================================== */
/* PROBE_UNDERRUN -- #2149 speaker-safe silicon verification            */
/* ================================================================== */
/* Compile-time mode, same shape as the five above: `west build ... --
 * -DPROBE_UNDERRUN=1` replaces main()'s ENTIRE body with ur_main(); with
 * none of the PROBE_* switches defined, main() is unchanged, and with
 * exactly one defined, every other mode's block (including this one) is
 * preprocessed away -- so none of the other builds' object code moves by
 * adding this one. ur_main() duplicates the other modes' bring-up rather
 * than sharing a helper with any of them, for the same byte-identity
 * reason PROBE_RESUME2's own header gives.
 *
 * WHAT THIS PROVES: issue #2149 keeps the DesignWare I2S bit clock (and
 * therefore the TDM/I2S clock the TAS2563 amps watch) running across a TX
 * UNDERRUN -- an unfed span this app creates on purpose by simply not
 * calling alp_audio_out_write() for a while, never by calling
 * alp_audio_out_stop() -- so the amps' own clock-loss shutdown (PWR_CTL ->
 * 0x0e, the #2146 failure mode PROBE_RESUME2 already proved for a
 * DELIBERATE stop) must NOT fire for an underrun. Step 5 is the negative
 * control: a REAL stop()/start() cycle without tas2563_resume() still has
 * to produce 0x0e, proving the register check itself can fail, not just
 * always read a comforting 0x0c.
 *
 * HARDWARE SAFETY: same two levers as every other mode --
 * TAS2563_AMP_LEVEL_MIN (analog) confirmed by the shared bring-up below,
 * and SOUND_VOL_MAX (48/255 digital, UR_VOLUME here) as the compile-time
 * capped ceiling for the whole run. See the file header's HARDWARE SAFETY
 * section for why that pair is speaker-safe.
 *
 * LOGGING: this mode is the one PROBE_* build that needs
 * CONFIG_LOG=y + CONFIG_LOG_DEFAULT_LEVEL=WRN so the alp_i2s_zephyr
 * backend's own `LOG_WRN_RATELIMIT("i2s: recovered from I2S_STATE_ERROR on
 * write()", ...)` (src/backends/i2s/zephyr_drv.c) actually reaches the
 * console -- every other mode leaves logging off (this app's own prj.conf
 * has no CONFIG_LOG at all). That is pulled in ONLY when PROBE_UNDERRUN is
 * defined, via underrun.conf + CMakeLists.txt's OVERLAY_CONFIG -- every
 * other mode's build/log budget is untouched. */
#if defined(PROBE_UNDERRUN)

#include <string.h> /* memset(), used by the zero/DC-step block writers below. */

#define UR_VOLUME   SOUND_VOL_MAX /* 48/255 digital -- the same hard safety cap as every mode. */
#define UR_BLOCK_MS (SOUND_FRAMES_PER_BLOCK * 1000u / SOUND_SAMPLE_RATE_HZ) /* 16 ms. */
#define UR_MS_TO_BLOCKS(ms) (((uint32_t)(ms) + UR_BLOCK_MS / 2u) / UR_BLOCK_MS)

/* Register addresses -- same values/citations as PROBE_RESUME2's
 * RESUME2_REG_*, copied verbatim rather than shared across #if blocks (see
 * this mode's own header for why). */
#define UR_REG_PWR_CTL   0x02u /* Power control        (SLASET3D §7.5.4,  p.65). */
#define UR_REG_INT_LTCH0 0x24u /* Latched interrupts 0 (SLASET3D §7.5.36, p.82) -- TDM_CLOCK. */
#define UR_REG_INT_LTCH3 0x26u /* Latched interrupts 2 (SLASET3D §7.5.38, p.84) -- DC_DETECT. */
#define UR_REG_INT_LTCH4 0x27u /* Latched interrupts 3 (SLASET3D §7.5.39, p.84) -- POWER_DOWN. */
#define UR_LTCH0_BIT2    (1u << 2) /* INT_LTCH0 bit2 -- must read clear for a PASS. */
#define UR_LTCH3_BIT3    (1u << 3) /* INT_LTCH3 bit3 (0x08) -- DC_DETECT. */

/* i2s3@49017000 -- the DesignWare I2S controller feeding both amps.
 * CER = Clock Enable Register, TER = Transmit Enable Register (DesignWare
 * I2S IP register map); addresses as given in this task. */
#define UR_I2S3_CER_ADDR 0x4901700Cu
#define UR_I2S3_TER_ADDR 0x4901702Cu

/* Priming/settle headroom, not measured constants -- see each call site's
 * own comment for what they bound. */
#define UR_DC_PRIME_BLOCKS 5u /* zero blocks before step 3's DC step, steady-stream headroom. */
#define UR_ANALYSIS_DISCARD_BLOCKS \
	6u /* ~96 ms, same DC-block-filter settle margin PROBE_RESUME2 uses. */

static uint8_t ur_reg_read(tas2563_t *ctx, uint8_t reg)
{
	uint8_t val = 0xFFu;
	(void)alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, &val, 1);
	return val;
}

typedef struct {
	uint8_t  pwr_ctl[AMP_COUNT];
	uint8_t  ltch0[AMP_COUNT];
	uint8_t  ltch3[AMP_COUNT];
	uint8_t  ltch4[AMP_COUNT];
	uint32_t cer;
	uint32_t ter;
} ur_snapshot_t;

/* ur_regs(label) -- reads PWR_CTL/INT_LTCH0/INT_LTCH3/INT_LTCH4 on both
 * amps plus I2S3's own CER/TER, prints one "[ur] <label> ..." line, and
 * returns the snapshot so a caller can gate a PASS/FAIL/STOP verdict on
 * the SAME read it just printed (no second round of I2C/MMIO traffic).
 * This helper NEVER clears latches itself -- every step that needs
 * "cleared" latches calls tas2563_clear_faults() (below) explicitly,
 * immediately before the pre-step ur_regs() call that is meant to observe
 * them cleared; see each step for exactly where. */
static ur_snapshot_t ur_regs(const char *label, tas2563_t amps[AMP_COUNT])
{
	ur_snapshot_t snap;
	for (size_t i = 0; i < AMP_COUNT; i++) {
		snap.pwr_ctl[i] = ur_reg_read(&amps[i], UR_REG_PWR_CTL);
		snap.ltch0[i]   = ur_reg_read(&amps[i], UR_REG_INT_LTCH0);
		snap.ltch3[i]   = ur_reg_read(&amps[i], UR_REG_INT_LTCH3);
		snap.ltch4[i]   = ur_reg_read(&amps[i], UR_REG_INT_LTCH4);
	}
	snap.cer = sys_read32(UR_I2S3_CER_ADDR);
	snap.ter = sys_read32(UR_I2S3_TER_ADDR);
	printf("[ur] %-20s uptime=%u ms  PWR_CTL=0x%02x/0x%02x  INT_LTCH0=0x%02x/0x%02x  "
	       "INT_LTCH3=0x%02x/0x%02x  INT_LTCH4=0x%02x/0x%02x  CER=0x%08x  TER=0x%08x\n",
	       label,
	       k_uptime_get_32(),
	       snap.pwr_ctl[0],
	       snap.pwr_ctl[1],
	       snap.ltch0[0],
	       snap.ltch0[1],
	       snap.ltch3[0],
	       snap.ltch3[1],
	       snap.ltch4[0],
	       snap.ltch4[1],
	       snap.cer,
	       snap.ter);
	return snap;
}

static void ur_clear_faults(tas2563_t amps[AMP_COUNT])
{
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t rc = tas2563_clear_faults(&amps[i]);
		printf("[ur] tas2563_clear_faults(0x%02x) -> %d\n", amp_addrs[i], (int)rc);
	}
}

/* --- I2S TX block writers: zero, a within-block DC step, and a continuous
 * 1 kHz sine (its own generator, not write_one_tone_block's square wave --
 * a sine keeps the Goertzel peak-search below clean of odd-harmonic
 * energy). ------------------------------------------------------------- */
static alp_status_t ur_write_zero_block(alp_audio_out_t *spk, int16_t *buf)
{
	memset(buf, 0, SOUND_FRAMES_PER_BLOCK * 2u * sizeof(int16_t));
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

static uint32_t ur_write_zero_blocks(alp_audio_out_t *spk, int16_t *buf, uint32_t n)
{
	uint32_t fail = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (ur_write_zero_block(spk, buf) != ALP_OK) fail++;
	}
	return fail;
}

#define UR_DC_VALUE 0x0400 /* constant PCM step level, both channels. */

/* One block, first half zero / second half a constant UR_DC_VALUE step on
 * both channels -- "last frames" per this task, isolating the DC
 * transition at a known point inside one ~16 ms block rather than
 * spreading it across the whole write. */
static alp_status_t ur_write_dc_step_block(alp_audio_out_t *spk, int16_t *buf)
{
	uint32_t half = SOUND_FRAMES_PER_BLOCK / 2u;
	memset(buf, 0, SOUND_FRAMES_PER_BLOCK * 2u * sizeof(int16_t));
	for (uint32_t f = half; f < SOUND_FRAMES_PER_BLOCK; f++) {
		buf[2u * f]     = (int16_t)UR_DC_VALUE;
		buf[2u * f + 1] = (int16_t)UR_DC_VALUE;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

static alp_status_t ur_write_sine_block(alp_audio_out_t *spk,
                                        int16_t         *buf,
                                        uint32_t        *phase_acc,
                                        uint32_t         samples_per_cycle)
{
	for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
		float theta =
		    GOERTZEL_TWO_PI * (float)(*phase_acc % samples_per_cycle) / (float)samples_per_cycle;
		int16_t sample  = (int16_t)((float)SOUND_TONE_AMPLITUDE * sinf(theta));
		buf[2u * f]     = sample;
		buf[2u * f + 1] = sample;
		(*phase_acc)++;
	}
	return alp_audio_out_write(spk, buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u);
}

/* --- Step 4's mic-side measurement: a streaming 200-4000 Hz/25 Hz-step
 * Goertzel bank + a running RMS accumulator, fed one sample at a time as
 * mic blocks arrive -- NOT a stored raw-sample buffer like PROBE_RESUME2's
 * r2_ch0_buf. At MIC_SAMPLE_RATE_HZ (48 kHz) this mode's longest window
 * (the 1.5 s post-gap tone) is ~72000 samples; buffering that raw would
 * cost ~144 KB, while UR_SCAN_BINS (153) persistent Goertzel states cost
 * under 2 KB regardless of window length. The expensive part (peak
 * search, median sort, sqrt) still runs exactly ONCE, after the capture
 * loop finishes -- same "cheap per-sample, expensive once at the end"
 * split as PROBE_RESUME2's r2_scan(), just streaming instead of
 * buffered. ---------------------------------------------------------- */
#define UR_SCAN_MIN_HZ  200u
#define UR_SCAN_MAX_HZ  4000u
#define UR_SCAN_STEP_HZ 25u
#define UR_SCAN_BINS    ((UR_SCAN_MAX_HZ - UR_SCAN_MIN_HZ) / UR_SCAN_STEP_HZ + 1u) /* 153. */
#define UR_SCAN_BIN_1000HZ \
	((1000u - UR_SCAN_MIN_HZ) / UR_SCAN_STEP_HZ) /* 32 -- exact, 200+32*25=1000. */

typedef struct {
	float coeff;
	float s_prev;
	float s_prev2;
} ur_goertzel_t;

static void
ur_goertzel_reset(ur_goertzel_t *g, uint32_t freq_hz, uint32_t n_samples, uint32_t fs_hz)
{
	float k     = (float)n_samples * (float)freq_hz / (float)fs_hz;
	float omega = GOERTZEL_TWO_PI * k / (float)n_samples;
	g->coeff    = 2.0f * cosf(omega);
	g->s_prev   = 0.0f;
	g->s_prev2  = 0.0f;
}

static inline void ur_goertzel_step(ur_goertzel_t *g, float x)
{
	float s    = x + g->coeff * g->s_prev - g->s_prev2;
	g->s_prev2 = g->s_prev;
	g->s_prev  = s;
}

static float ur_goertzel_power(const ur_goertzel_t *g)
{
	return g->s_prev * g->s_prev + g->s_prev2 * g->s_prev2 - g->coeff * g->s_prev * g->s_prev2;
}

typedef struct {
	ur_goertzel_t bins[UR_SCAN_BINS];
	double        sum_sq;
	uint32_t      n;
} ur_scan_t;

/* n_expected MUST be fixed before capture starts -- each bin's Goertzel
 * coefficient depends on it (k = n_expected * freq / fs). */
static void ur_scan_init(ur_scan_t *sc, uint32_t n_expected)
{
	for (uint32_t i = 0; i < UR_SCAN_BINS; i++) {
		ur_goertzel_reset(
		    &sc->bins[i], UR_SCAN_MIN_HZ + i * UR_SCAN_STEP_HZ, n_expected, MIC_SAMPLE_RATE_HZ);
	}
	sc->sum_sq = 0.0;
	sc->n      = 0;
}

static void ur_scan_feed(ur_scan_t *sc, int16_t sample)
{
	float x = (float)sample;
	for (uint32_t i = 0; i < UR_SCAN_BINS; i++)
		ur_goertzel_step(&sc->bins[i], x);
	sc->sum_sq += (double)sample * (double)sample;
	sc->n++;
}

static void ur_scan_finish(const ur_scan_t *sc,
                           uint32_t        *peak_hz_out,
                           double          *peak_db_out,
                           double          *f1000_db_out,
                           double          *ac_rms_out)
{
	float powers[UR_SCAN_BINS];
	for (uint32_t i = 0; i < UR_SCAN_BINS; i++)
		powers[i] = ur_goertzel_power(&sc->bins[i]);

	uint32_t peak_i = 0;
	float    peak_p = powers[0];
	for (uint32_t i = 1; i < UR_SCAN_BINS; i++) {
		if (powers[i] > peak_p) {
			peak_p = powers[i];
			peak_i = i;
		}
	}

	float sorted[UR_SCAN_BINS];
	memcpy(sorted, powers, sizeof(powers));
	for (uint32_t i = 1; i < UR_SCAN_BINS; i++) {
		float key = sorted[i];
		int   j   = (int)i - 1;
		while (j >= 0 && sorted[j] > key) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}
	float median_p = sorted[UR_SCAN_BINS / 2u] + 1e-6f;

	*peak_hz_out  = UR_SCAN_MIN_HZ + peak_i * UR_SCAN_STEP_HZ;
	*peak_db_out  = 10.0 * log10((double)(peak_p + 1e-6f) / (double)median_p);
	*f1000_db_out = 10.0 * log10((double)(powers[UR_SCAN_BIN_1000HZ] + 1e-6f) / (double)median_p);
	*ac_rms_out   = (sc->n > 0) ? sqrt(sc->sum_sq / (double)sc->n) : 0.0;
}

/* Runs total_blocks iterations of (write a 1 kHz sine block, read one mic
 * block) in lock-step -- the same 16 ms/block pairing the file header's
 * BUILD_ASSERT guarantees. The first discard_blocks blocks' mic samples
 * are read and thrown away (decimator/tone-onset settle); the rest feed
 * *sc. A mic read failure (including a -EIO overrun) is recovered via
 * STOP/START and counted into *overrun_count -- that block's samples are
 * simply not fed to *sc, never silently substituted. Returns the write-
 * failure count for this call only (not accumulated across calls). */
static uint32_t ur_capture_tone_window(alp_audio_out_t *spk,
                                       alp_audio_in_t  *mic,
                                       int16_t         *out_buf,
                                       int16_t         *mic_scratch,
                                       uint32_t        *phase_acc,
                                       uint32_t         total_blocks,
                                       uint32_t         discard_blocks,
                                       ur_scan_t       *sc,
                                       uint32_t        *overrun_count)
{
	uint32_t write_failures = 0;
	ur_scan_init(sc, (total_blocks - discard_blocks) * MIC_FRAMES_PER_BLOCK);
	for (uint32_t b = 0; b < total_blocks; b++) {
		if (ur_write_sine_block(spk, out_buf, phase_acc, SOUND_SAMPLE_RATE_HZ / SOUND_TONE_HZ) !=
		    ALP_OK) {
			write_failures++;
		}
		size_t       got = 0;
		alp_status_t rrc =
		    alp_audio_in_read(mic, mic_scratch, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
		if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
			if (rrc == ALP_ERR_IO) (*overrun_count)++;
			(void)alp_audio_in_stop(mic);
			(void)alp_audio_in_start(mic);
			continue; /* this block's samples are lost -- best-effort, like PROBE_RESUME2. */
		}
		if (b >= discard_blocks) {
			for (uint32_t f = 0; f < MIC_FRAMES_PER_BLOCK; f++) {
				ur_scan_feed(sc, mic_scratch[f * MIC_CHANNELS + 0]);
			}
		}
	}
	return write_failures;
}

/* Step 4's long stall itself: no writes at all (the INTENDED unfed span),
 * just enough mic reads to keep its slab from overrunning -- every block
 * is discarded immediately, never analyzed, per this task's hard rule
 * that mic analysis never runs inside a non-intended gap. */
static void ur_drain_mic_discard(alp_audio_in_t *mic,
                                 int16_t        *mic_scratch,
                                 uint32_t        blocks,
                                 uint32_t       *overrun_count)
{
	for (uint32_t b = 0; b < blocks; b++) {
		size_t       got = 0;
		alp_status_t rrc =
		    alp_audio_in_read(mic, mic_scratch, MIC_FRAMES_PER_BLOCK, &got, MIC_READ_TIMEOUT_MS);
		if (rrc != ALP_OK || got != MIC_FRAMES_PER_BLOCK) {
			if (rrc == ALP_ERR_IO) (*overrun_count)++;
			(void)alp_audio_in_stop(mic);
			(void)alp_audio_in_start(mic);
		}
	}
}

/* Step 3's one attempt (200 ms then, if it didn't already STOP, 1 s):
 * clears faults, primes with a few zero blocks, writes the one DC-step
 * block, stalls (the INTENDED unfed span), reads back, and applies the
 * shared STOP criterion. Returns true if the run should abort the
 * long-stall steps (4 and 5) and skip straight to teardown. */
static bool ur_step3_attempt(alp_audio_out_t *spk,
                             tas2563_t        amps[AMP_COUNT],
                             int16_t         *buf,
                             uint32_t         stall_ms,
                             const char      *label,
                             uint32_t        *write_failures)
{
	ur_clear_faults(amps);
	*write_failures += ur_write_zero_blocks(spk, buf, UR_DC_PRIME_BLOCKS);
	if (ur_write_dc_step_block(spk, buf) != ALP_OK) (*write_failures)++;
	k_msleep(stall_ms);
	ur_snapshot_t snap = ur_regs(label, amps);
	bool          dc_detect =
	    ((snap.ltch3[0] & UR_LTCH3_BIT3) != 0u) || ((snap.ltch3[1] & UR_LTCH3_BIT3) != 0u);
	bool shutdown = (snap.pwr_ctl[0] != 0x0cu) || (snap.pwr_ctl[1] != 0x0cu);
	return dc_detect || shutdown;
}

static int ur_main(void)
{
	printf("\n=== aen-i2s-tas2563-probe (PROBE_UNDERRUN): #2149 speaker-safe silicon "
	       "verification ===\n");
	(void)alp_init();

	/* --- Same bring-up as every other PROBE_* mode: bridge, mux,
	 * AMP_ENABLE, tas2563_init x2, level MIN, configure_i2s x2. -------- */
	static cc3501e_t fw;
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[ur] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) return 0;

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[ur] alp_gpio_open(mux SELECT/ENABLE) -> NULL\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	alp_status_t mux_rc = alp_gpio_configure(mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_sel, false);
	printf("[ur] I2S_SELECT (0=amps) -> %d\n", (int)mux_rc);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (mux_rc == ALP_OK) mux_rc = alp_gpio_write(mux_en, false);
	printf("[ur] I2S_EN (active low) -> %d\n", (int)mux_rc);
	if (mux_rc != ALP_OK) {
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[ur] gpio5 not ready\n");
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[ur] AMP_ENABLE (SD_N) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		printf("[ur] AMP_ENABLE/AMP_FAULT not fully drivable (rc=%d)\n", grc);
		mux_disable(mux_sel, mux_en);
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US);

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	int        ok_amps = 0;
	for (size_t i = 0; i < AMP_COUNT && bus != NULL; i++) {
		alp_status_t irc = tas2563_init(&amps[i], bus, amp_addrs[i], NULL);
		printf("[ur] tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)irc);
		if (irc == ALP_OK) ok_amps++;
	}
	if (ok_amps != (int)AMP_COUNT) {
		printf("[ur] %d/%zu amp(s) answered -- aborting\n", ok_amps, (size_t)AMP_COUNT);
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		if (bus != NULL) alp_i2c_close(bus);
		return 0;
	}

	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[ur] tas2563_set_amp_level(0x%02x, MIN) -> %d\n", amp_addrs[i], (int)lrc);
	}

	const alp_i2s_config_t amp_i2s_cfg = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = SOUND_SAMPLE_RATE_HZ,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = SOUND_FRAMES_PER_BLOCK,
	};
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[ur] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u];
	uint32_t       phase_acc      = 0;
	uint32_t       write_failures = 0;
	uint32_t       overrun_count  = 0;

	/* --- 1. Open, prime with one zero block, resume, checkpoint. ------ */
	alp_audio_out_t *spk    = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc = (spk != NULL) ? alp_audio_out_start(spk) : alp_last_error();
	printf("[ur] alp_audio_out_open+start(I2S3) -> %d\n", (int)spk_rc);
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_set_volume(spk, UR_VOLUME);
	printf("[ur] alp_audio_out_set_volume(%u) -> %d\n", UR_VOLUME, (int)spk_rc);

	if (spk_rc != ALP_OK) {
		printf("[ur] I2S3 did not start -- aborting before touching the amps\n");
	} else {
		if (ur_write_zero_block(spk, tone_buf) != ALP_OK) write_failures++;
		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t rrc = tas2563_resume(&amps[i]);
			printf("[ur] step1: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
		}
		ur_regs("after resume", amps); /* expect PWR_CTL 0x0c/0x0c. */

		/* --- 2. Zero-block stall. -------------------------------------- */
		ur_clear_faults(amps);
		write_failures += ur_write_zero_blocks(spk, tone_buf, 25u);
		/* STOP writing for 200 ms -- the INTENDED unfed span. At ~100 ms
		 * in, expect CER bit0=1 (clock still enabled -- #2149) and TER
		 * bit0=0 (TX disabled by the underrun itself). */
		k_msleep(100);
		uint32_t cer_100ms = sys_read32(UR_I2S3_CER_ADDR);
		uint32_t ter_100ms = sys_read32(UR_I2S3_TER_ADDR);
		printf("[ur] step2 +100ms  CER=0x%08x (bit0=%u, expect 1)  TER=0x%08x (bit0=%u, "
		       "expect 0)\n",
		       cer_100ms,
		       (unsigned)(cer_100ms & 1u),
		       ter_100ms,
		       (unsigned)(ter_100ms & 1u));
		k_msleep(100);
		write_failures += ur_write_zero_blocks(spk, tone_buf, UR_MS_TO_BLOCKS(500u));
		ur_snapshot_t s2 = ur_regs("step2", amps);
		bool          step2_pass =
		    (s2.pwr_ctl[0] == 0x0cu && s2.pwr_ctl[1] == 0x0cu) &&
		    ((s2.ltch0[0] & UR_LTCH0_BIT2) == 0u) && ((s2.ltch0[1] & UR_LTCH0_BIT2) == 0u) &&
		    ((s2.ltch3[0] & UR_LTCH3_BIT3) == 0u) && ((s2.ltch3[1] & UR_LTCH3_BIT3) == 0u);
		printf("[ur] step2 %s\n", step2_pass ? "PASS" : "FAIL");

		/* --- 3. DC check: 200 ms stall, then (if that didn't already
		 * STOP) a repeat at 1 s. -------------------------------------- */
		bool step3_stop = ur_step3_attempt(spk, amps, tone_buf, 200u, "step3a", &write_failures);
		if (!step3_stop) {
			step3_stop = ur_step3_attempt(spk, amps, tone_buf, 1000u, "step3b", &write_failures);
		}
		if (step3_stop) {
			printf("[ur] STOP: DC_DETECT or shutdown after non-zero stall -- aborting "
			       "long-stall steps\n");
		}

		bool run_step4 = step2_pass && !step3_stop;
		bool run_step5 = !step3_stop;

		/* --- 4. Long stall (only if steps 2 and 3 passed). ------------- */
		if (run_step4) {
			ur_clear_faults(amps);

			alp_audio_in_t *mic    = alp_audio_in_open(&(alp_audio_config_t){
			    .peripheral_id    = 0,
			    .sample_rate_hz   = MIC_SAMPLE_RATE_HZ,
			    .channels         = MIC_CHANNELS,
			    .format           = ALP_AUDIO_FMT_S16_LE,
			    .frames_per_block = MIC_FRAMES_PER_BLOCK,
			});
			alp_status_t    mic_rc = (mic != NULL) ? alp_audio_in_start(mic) : alp_last_error();
			printf("[ur] step4: alp_audio_in_open+start(PDM) -> %d\n", (int)mic_rc);

			if (mic == NULL || mic_rc != ALP_OK) {
				printf("[ur] step4: PDM mic never opened/started -- skipping "
				       "long-stall measurement\n");
			} else {
				static int16_t   mic_scratch[MIC_FRAMES_PER_BLOCK * MIC_CHANNELS];
				static ur_scan_t pregap_scan, postgap_scan;
				uint32_t         step4_fail = 0;

				step4_fail += ur_capture_tone_window(spk,
				                                     mic,
				                                     tone_buf,
				                                     mic_scratch,
				                                     &phase_acc,
				                                     UR_MS_TO_BLOCKS(1000u),
				                                     UR_ANALYSIS_DISCARD_BLOCKS,
				                                     &pregap_scan,
				                                     &overrun_count);
				uint32_t pregap_hz;
				double   pregap_peak_db, pregap_f1000_db, pregap_rms;
				ur_scan_finish(
				    &pregap_scan, &pregap_hz, &pregap_peak_db, &pregap_f1000_db, &pregap_rms);
				printf("[ur] step4 pre-gap  mic peak=%u Hz  peak_dB=%.1f  f1000_dB=%.1f  "
				       "AC_RMS=%.1f\n",
				       pregap_hz,
				       pregap_peak_db,
				       pregap_f1000_db,
				       pregap_rms);

				/* The INTENDED unfed span -- 4.6 s, long enough for a
				 * real TX underrun. Mic keeps draining so its own slab
				 * does not overrun, discarded immediately. */
				ur_drain_mic_discard(mic, mic_scratch, UR_MS_TO_BLOCKS(4600u), &overrun_count);

				/* Resume writing the tone WITHOUT tas2563_resume() --
				 * the whole point of #2149: the amp should need no
				 * re-arming because its own clock never stopped. */
				step4_fail += ur_capture_tone_window(spk,
				                                     mic,
				                                     tone_buf,
				                                     mic_scratch,
				                                     &phase_acc,
				                                     UR_MS_TO_BLOCKS(1500u),
				                                     UR_ANALYSIS_DISCARD_BLOCKS,
				                                     &postgap_scan,
				                                     &overrun_count);
				uint32_t postgap_hz;
				double   postgap_peak_db, postgap_f1000_db, postgap_rms;
				ur_scan_finish(
				    &postgap_scan, &postgap_hz, &postgap_peak_db, &postgap_f1000_db, &postgap_rms);
				printf("[ur] step4 post-gap mic peak=%u Hz  peak_dB=%.1f  f1000_dB=%.1f  "
				       "AC_RMS=%.1f\n",
				       postgap_hz,
				       postgap_peak_db,
				       postgap_f1000_db,
				       postgap_rms);

				/* Ordering contract: read the latches BEFORE muting. */
				ur_snapshot_t s4 = ur_regs("step4 post-gap", amps);

				/* Mute BEFORE any stop -- <alp/audio.h>'s
				 * alp_audio_out_stop() doc states this explicitly
				 * (issue #2146). */
				for (size_t i = 0; i < AMP_COUNT; i++) {
					alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
					printf("[ur] step4: tas2563_set_mode(0x%02x, SHUTDOWN) -> "
					       "%d\n",
					       amp_addrs[i],
					       (int)srr);
				}

				bool freq_ok    = (postgap_hz + UR_SCAN_STEP_HZ >= 1000u) &&
				                  (postgap_hz <= 1000u + UR_SCAN_STEP_HZ);
				bool level_ok   = fabs(postgap_peak_db - pregap_peak_db) <= 6.0;
				bool regs_ok    = (s4.pwr_ctl[0] == 0x0cu && s4.pwr_ctl[1] == 0x0cu) &&
				                  ((s4.ltch0[0] & UR_LTCH0_BIT2) == 0u) &&
				                  ((s4.ltch0[1] & UR_LTCH0_BIT2) == 0u) &&
				                  ((s4.ltch3[0] & UR_LTCH3_BIT3) == 0u) &&
				                  ((s4.ltch3[1] & UR_LTCH3_BIT3) == 0u);
				bool step4_pass = freq_ok && level_ok && regs_ok;
				printf("[ur] step4 %s (write failures this step: %u)\n",
				       step4_pass ? "PASS" : "FAIL",
				       step4_fail);
				write_failures += step4_fail;
			}
			alp_audio_in_close(mic); /* NULL is a documented no-op. */
		}

		/* --- 5. Negative control: a real stop()/start() cycle without
		 * tas2563_resume() -- proves the register check itself can
		 * fail, not just always read a comforting 0x0c. -------------- */
		if (run_step5) {
			for (size_t i = 0; i < AMP_COUNT; i++) {
				alp_status_t rrc = tas2563_resume(&amps[i]);
				printf("[ur] step5: tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rrc);
			}
			write_failures += ur_write_zero_blocks(spk, tone_buf, UR_MS_TO_BLOCKS(200u));
			alp_status_t stop_rc = alp_audio_out_stop(spk);
			printf("[ur] step5: alp_audio_out_stop -> %d\n", (int)stop_rc);
			k_msleep(500);
			alp_status_t start_rc = alp_audio_out_start(spk);
			printf("[ur] step5: alp_audio_out_start -> %d\n", (int)start_rc);
			write_failures += ur_write_zero_blocks(spk, tone_buf, UR_MS_TO_BLOCKS(200u));
			ur_regs("step5", amps);
			printf("[ur] step5: expect PWR_CTL 0x0e/0x0e (clock-loss shutdown, amp never "
			       "re-armed -- this stop leaves the amp ACTIVE first and may thump at "
			       "volume %u / TAS2563_AMP_LEVEL_MIN; that is acceptable, already "
			       "observed as a kick at this level)\n",
			       UR_VOLUME);
		}
	}

	/* --- 6. Teardown: mute BEFORE stop (the ordering contract), then
	 * close, then the final CER check. ---------------------------------- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t srr = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[ur] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)srr);
	}
	alp_status_t final_stop_rc = (spk != NULL) ? alp_audio_out_stop(spk) : ALP_ERR_NOT_READY;
	printf("[ur] alp_audio_out_stop -> %d\n", (int)final_stop_rc);
	for (size_t i = 0; i < AMP_COUNT; i++)
		tas2563_deinit(&amps[i]);
	if (spk != NULL) alp_audio_out_close(spk);
	uint32_t cer_final = sys_read32(UR_I2S3_CER_ADDR);
	printf("[ur] teardown CER=0x%08x (bit0=%u, expect 0)\n", cer_final, (unsigned)(cer_final & 1u));
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	printf("[ur] total alp_audio_out_write() failures this run: %u\n", write_failures);
	printf("[ur] total mic overrun count this run: %u\n", overrun_count);
	printf("[ur] done\n");
	return 0;
}

#endif /* PROBE_UNDERRUN */

int main(void)
{
#if defined(PROBE_LISTEN)
	return listen_main();
#elif defined(PROBE_MELODY)
	return melody_main();
#elif defined(PROBE_LOOPBACK)
	return loop_main();
#elif defined(PROBE_RESUME)
	return resume_main();
#elif defined(PROBE_RESUME2)
	return resume2_main();
#elif defined(PROBE_UNDERRUN)
	return ur_main();
#else
	printf("\n=== aen-i2s-tas2563-probe: I2S0 through the reworked U46 mux ===\n");
	(void)alp_init();

	/* --- 1. CC3501E bridge, then mux SELECT (amps), THEN mux ENABLE ---- */
	static cc3501e_t fw; /* static: ~32 KB, would blow PSPLIM on main()'s stack. */
	alp_status_t     rc = cc3501e_bridge_bringup(&fw);
	printf("[probe] cc3501e_bridge_bringup() -> %d\n", (int)rc);
	if (rc != ALP_OK) {
		print_early_exit_verdicts(VERDICT_BRIDGE_MUX_FAILED, "bridge did not come up");
		return 0;
	}

	alp_gpio_t *mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	alp_gpio_t *mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (mux_sel == NULL || mux_en == NULL) {
		printf("[probe] alp_gpio_open(mux SELECT/ENABLE) -> NULL -- check "
		       "CONFIG_ALP_SDK_GPIO_CC3501E_PROXY and src/cc3501e_gpio_routes.c "
		       "carry the IO8/IO13 routes\n");
		mux_disable(mux_sel, mux_en); /* nothing open yet on either handle if this fired. */
		print_early_exit_verdicts(VERDICT_BRIDGE_MUX_FAILED, "mux GPIO open failed");
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
		print_early_exit_verdicts(VERDICT_BRIDGE_MUX_FAILED, "mux SELECT/ENABLE write failed");
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	/* --- 2. AMP_ENABLE (SD_N) hardware reset + AMP_FAULT as input -------- */
	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[probe] gpio5 not ready -- AMP_ENABLE/AMP_FAULT (P5_2/P5_0) unreachable\n");
		mux_disable(mux_sel, mux_en); /* EP2: AMP_ENABLE was never touched -- gpio5 unusable. */
		print_early_exit_verdicts(VERDICT_AMP_CONTROL_GPIO_FAILED, "gpio5 not ready");
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
		print_early_exit_verdicts(VERDICT_AMP_CONTROL_GPIO_FAILED,
		                          "AMP_ENABLE/AMP_FAULT pinctrl/configure failed");
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
		print_early_exit_verdicts(VERDICT_TAS2563_NOT_RESPONDING,
		                          "not every TAS2563 answered tas2563_init()");
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
		print_early_exit_verdicts(VERDICT_TAS2563_NOT_RESPONDING,
		                          "level readback and/or I2S configure not confirmed on both amps");
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
	    .frames_per_block = MIC_FRAMES_PER_BLOCK,
	});
	alp_status_t    mic_rc = (mic != NULL) ? alp_audio_in_start(mic) : alp_last_error();
	bool            mic_ok = (mic != NULL) && (mic_rc == ALP_OK);
	printf("[probe] alp_audio_in_open+start(PDM, U19 LEFT/U20 RIGHT) -> %d%s\n",
	       (int)mic_rc,
	       mic_ok ? "" : " -- acoustic capture skipped, EVIDENCE 1 (TDM_CLOCK) still runs");
	/* Latches true on the FIRST mic read failure anywhere in this run (set
	 * by run_acoustic_window() below) -- once true, every LATER window
	 * skips the mic entirely rather than risk another MIC_READ_TIMEOUT_MS
	 * stall that could starve the I2S TX slab. See run_acoustic_window()'s
	 * own comment and the file header's TONE-ON BUDGET note. */
	bool mic_dead = false;

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
	baseline_ok = run_acoustic_window("BASELINE",
	                                  mic,
	                                  &mic_dead,
	                                  mic_ok,
	                                  NULL,
	                                  tone_buf,
	                                  &phase_acc,
	                                  samples_per_cycle,
	                                  baseline_r);

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

	/* --- 12. EVIDENCE 1's DURING/STOPPED check -- ITS OWN tone-only
	 * segment, entirely before any mic activity. See run_tdm_clock_check()
	 * above and the file header's TONE-ON BUDGET note: this used to be
	 * interleaved with the acoustic volume-step windows below, and the
	 * mic's up-to-MIC_READ_TIMEOUT_MS blocking read between tone writes
	 * drained the 2-block I2S TX slab, breaking the NEXT write and making
	 * the clock verdict depend on mic timing rather than the electrical
	 * signal. Only runs if active_set (both amps genuinely ACTIVE). ------ */
	uint32_t     faults_during[AMP_COUNT]  = { 0 };
	uint32_t     faults_stopped[AMP_COUNT] = { 0 };
	bool         during_valid              = false;
	alp_status_t during_fail_rc            = ALP_OK;
	bool         stopped_valid             = false;
	alp_status_t stop_rc                   = ALP_ERR_NOT_READY;
	if (active_set) {
		run_tdm_clock_check(amps,
		                    amp_addrs,
		                    spk,
		                    tone_buf,
		                    &phase_acc,
		                    samples_per_cycle,
		                    faults_during,
		                    &during_valid,
		                    &during_fail_rc,
		                    faults_stopped,
		                    &stopped_valid,
		                    &stop_rc);
	}
	/* run_tdm_clock_check() always attempts alp_audio_out_stop() once it
	 * gets as far as a successful DURING read (during_valid), regardless
	 * of whether the stop itself succeeds -- so "was a stop ever
	 * attempted" is exactly during_valid, which the final teardown below
	 * needs to decide whether it still owes the stream a stop() call. */
	bool stream_stop_attempted = during_valid;

	/* --- 13. Acoustic volume-step windows (VOL=4/16/48) -- SKIPPED
	 * entirely once the mic is known dead (mic_dead, most likely already
	 * true from step 8's BASELINE on this driver -- see #2133): no benefit
	 * replaying the tone with nothing able to listen, and skipping bounds
	 * this run's total tone-on time to the fixed TDM-check-only cost in
	 * that (currently the common) case. The stream was drained by the TDM
	 * check above, so it is explicitly restarted here -- the #2132
	 * deferred-start fix means alp_audio_out_start() just re-arms the
	 * real trigger for the next write, it does not fail merely because
	 * the stream was stopped before. */
	bool run_acoustic_windows = active_set && mic_ok && !mic_dead;
	if (run_acoustic_windows) {
		alp_status_t restart_rc = alp_audio_out_start(spk);
		printf("[probe] alp_audio_out_start(I2S3) [restart for acoustic windows] -> %d\n",
		       (int)restart_rc);
		run_acoustic_windows = (restart_rc == ALP_OK);
	}
	if (run_acoustic_windows) {
		for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
			alp_status_t vrc = alp_audio_out_set_volume(spk, sound_vol_steps[step]);
			printf("[probe] alp_audio_out_set_volume(%u) [step %zu/%zu] -> %d\n",
			       sound_vol_steps[step],
			       step + 1,
			       (size_t)SOUND_VOL_STEP_COUNT,
			       (int)vrc);
			if (vrc != ALP_OK) continue; /* vol_ok[step] stays false; keep trying later steps. */
			char label[16];
			snprintf(label, sizeof(label), "VOL=%u", sound_vol_steps[step]);
			vol_ok[step] = run_acoustic_window(label,
			                                   mic,
			                                   &mic_dead,
			                                   true,
			                                   spk,
			                                   tone_buf,
			                                   &phase_acc,
			                                   samples_per_cycle,
			                                   vol_r[step]);
		}
	} else if (active_set) {
		printf("[probe] acoustic volume-step windows skipped -- %s\n",
		       !mic_ok ? "PDM mic never opened/started" : "mic already failed earlier this run");
	}

	/* --- 14. Acoustic STOPPED window: mic-only, no tone (I2S3 halted by
	 * the TDM check's own stop-control above). ----------------------------*/
	stopped_ok = run_acoustic_window("STOPPED",
	                                 mic,
	                                 &mic_dead,
	                                 mic_ok && stopped_valid,
	                                 NULL,
	                                 tone_buf,
	                                 &phase_acc,
	                                 samples_per_cycle,
	                                 stopped_r);

	/* --- 15. Teardown, mute FIRST -- confirmed SHUTDOWN before anything
	 * else stops (both for EVIDENCE 1's safety contract and so the
	 * acoustic SHUTDOWN window below genuinely observes amps off). Print
	 * the rc, not discard it -- a SHUTDOWN write failure here would leave
	 * an amp's true mode unknown at exactly the point a bench reader most
	 * needs to trust it. ---------------------------------------------------*/
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t src = tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
		printf("[probe] tas2563_set_mode(0x%02x, SHUTDOWN) -> %d\n", amp_addrs[i], (int)src);
	}

	/* --- 16. Acoustic SHUTDOWN window: mic-only, amps now off ------------ */
	shutdown_ok = run_acoustic_window("SHUTDOWN",
	                                  mic,
	                                  &mic_dead,
	                                  mic_ok,
	                                  NULL,
	                                  tone_buf,
	                                  &phase_acc,
	                                  samples_per_cycle,
	                                  shutdown_r);

	/* --- 17. Rest of teardown -- this is the only remaining exit path. --- */
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

	/* --- 18. ACOUSTIC MEASUREMENT TABLE ----------------------------------- */
	printf("[probe] === ACOUSTIC MEASUREMENT TABLE ===\n");
	print_acoustic_row("BASELINE", baseline_r, baseline_ok);
	for (size_t step = 0; step < SOUND_VOL_STEP_COUNT; step++) {
		char label[16];
		snprintf(label, sizeof(label), "VOL=%u", sound_vol_steps[step]);
		print_acoustic_row(label, vol_r[step], vol_ok[step]);
	}
	print_acoustic_row("STOPPED", stopped_r, stopped_ok);
	print_acoustic_row("SHUTDOWN", shutdown_r, shutdown_ok);

	/* --- 19. ACOUSTIC VERDICT ---------------------------------------------- */
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
		av_reason = "one or more capture windows failed or were skipped after an earlier mic "
		            "failure -- see the table above";
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

	/* --- 20. EVIDENCE 1 (TDM_CLOCK) verdict, per amp then combined ------- */
	/* FIX 1 (bench issue, round 1): this branch used to print
	 * VERDICT_CLOCKS_NOT_REACHING_AMP here -- a claim that SCLK/WS do not
	 * reach the amp -- even though the DURING read (the only thing that
	 * could measure that) never ran.
	 * FIX 4 (bench issue, round 2): a SECOND, subtler version of the same
	 * bug survived that fix -- these two branches called
	 * verdict_str(VERDICT_INCONCLUSIVE), whose string is
	 * "inconclusive (TDM_CLOCK flag did not discriminate -- see the
	 * per-amp reads above)" -- a claim that BOTH reads happened and
	 * disagreed, which is false here: NEITHER read happened. That
	 * canned string is reserved for the per-amp combine below, where
	 * both reads genuinely did happen. These two branches print a plain,
	 * literal "DURING/STOPPED was never read" instead, plus the exact rc
	 * that stopped it, and do not depend on mic status at all -- only on
	 * `during_valid`/`stopped_valid`, which run_tdm_clock_check() sets
	 * from tone/amp/stop success alone. */
	if (!during_valid) {
		printf("[probe] I2S3/ACTIVE never reached a clean playing state (see the TDM check's "
		       "own write-rc prints above) -- this is independent of the PDM mic\n");
		printf("[probe] TDM_CLOCK VERDICT: INCONCLUSIVE -- DURING was never read (rc=%d)\n",
		       (int)during_fail_rc);
		printf("[probe] done\n");
		return 0;
	}
	if (!stopped_valid) {
		printf("[probe] TDM_CLOCK VERDICT: INCONCLUSIVE -- STOPPED was never read "
		       "(alp_audio_out_stop rc=%d)\n",
		       (int)stop_rc);
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
#endif /* !PROBE_LISTEN */
}

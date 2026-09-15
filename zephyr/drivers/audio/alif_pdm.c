/*
 * Copyright (C) 2025 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM -> retire onto
 * sdk-alif fork, PARTIALLY BENCH-VERIFIED) ======
 * The Alif Ensemble PDM (pulse-density-modulation microphone) block is driven by
 * a vendored copy of the Apache-2.0 zephyr_alif fork driver
 * (drivers/audio/alif_pdm.c, compatible "alif,alif_pdm").  hal_alif ships no PDM
 * / DMIC class driver, so this is a genuine fork-driver copy carried in-tree so
 * it survives a `west update`.  Retire onto the opt-in sdk-alif fork compatible
 * once the pdm node is repointed AND bench-verified.  See
 * docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * Silicon status (issue #2133, e1m-aen-evk-03): register-level
 * configuration verified; 48 kHz mode 7 capture rate verified exact with no
 * drops (commit 68a169977). Acoustic capture at 48 kHz VERIFIED on mic
 * ch0/ch1 (PDM controller 0) ONLY, by a speaker-to-mic loopback
 * (2026-09-15 14:49Z, TAS2563 speakers -> PDM mics, gain 0x200
 * readback-confirmed; PROBE_LOOPBACK mode of
 * examples/aen/aen-i2s-tas2563-probe on branch
 * test/u46-i2s-tas2563-on-reworked-mux, issue #2143 -- not this example):
 * Goertzel-bin analysis found the 1 kHz bin at 9.3/0.1 dB (ch0/ch1) in
 * silence rising to 43.4/48.8 dB at volume 16 and 57.8/57.6 dB at volume
 * 48, a 500 Hz bin at 54.7/56.6 dB that lit up only during the 500 Hz
 * stimulus, and peak-to-peak rising from 128/128 in silence to 651/652 at
 * 1 kHz/volume 48. The D2 pair (HW 4/5) is register-level verified only --
 * never acoustically tested. NOT verified: full-scale headroom at the
 * current provisional gain default (0x200, issue #2143) -- unmeasured.
 * ==================================================================
 *
 * Vendored from the fork with this provenance header added, plus the
 * documented divergences below; the register map lives in the companion
 * alif_pdm_reg.h.  vendor-ext, PARTIALLY BENCH-VERIFIED (see silicon status
 * above).
 *
 * ------------------------- alp-sdk divergence -------------------------
 * pdm_channel_config()'s FIR-coefficient store loop (issue #758) was
 * changed from the fork's raw `*ptr++ = value` walk to sys_write32() per
 * word. A plain pointer store gives the compiler no volatile/ordering
 * guarantee for a peripheral register bank; at -O3 the fork's original
 * loop merges pairs of writes into 8x strd (64-bit double-word MMIO
 * stores) plus an alias-check branch against the FIR_COEF/IIR_COEF_SEL
 * split -- a correctness risk even though no in-tree build currently
 * reaches -O3. Reapply this divergence if the file is ever re-synced
 * from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (2) ----------------------
 * alif_pdm_warning_isr()'s slab rollover (issue #1122) allocated exactly one
 * replacement block and copied the whole remainder of the IRQ burst into it
 * without checking the block was large enough -- a burst (up to
 * MAX_DATA_ITEMS * MAX_NUM_CHANNELS * sizeof(uint16_t) = 128 bytes) can
 * exceed a configured slab block_size, corrupting adjacent slab memory.
 * Rewritten to split the remainder across as many fresh blocks as needed
 * (never copying more than one block's worth per allocation), and
 * dmic_alif_pdm_configure() now rejects a zero block_size so the split loop
 * can never divide-by/against zero. Reapply this divergence if the file is
 * ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (3) ----------------------
 * dmic_alif_pdm_configure() (issue #2133) broke Zephyr's standard `dmic`
 * contract two ways: it read req_chan_map_lo's low byte VERBATIM as the
 * hardware channel-enable mask instead of decoding the standard
 * dmic_build_channel_map() nibble encoding (every dmic_build_channel_map()
 * caller, incl. the portable src/backends/audio/zephyr_drv.c backend,
 * enabled the wrong hardware channel), OR'd that mask into whatever the
 * register already held instead of replacing it, and never left the PDM
 * clock-mode field MICROPHONE_SLEEP nor programmed per-channel FIR/gain --
 * only the Alif-specific pdm_mode()/pdm_channel_config(), which a standard
 * dmic consumer has no reason to call, did that. Fixed by decoding the
 * channel map with alif_pdm_chanmap_translate() (alif_pdm_chanmap.h,
 * which also rejects a duplicate/out-of-order map the ISR cannot honour),
 * replacing (not OR-ing) the channel-enable field, validating the
 * requested pcm_rate + the caller's io clock window against a
 * HWRM/DFP-grounded mode table, and applying per-channel defaults in
 * configure() -- deferring the actual PDM_MODE write to
 * DMIC_TRIGGER_START (see divergence (4)).
 *
 * Round 1 of #2133 shipped this divergence with PDM_MODE_STANDARD_VOICE_512
 * mis-keyed to 16000 Hz; HWRM Table 15-118 and bench evidence
 * (PDM_CONFIG_REGISTER read back 0x00010033 on e1m-aen-evk-03) both show
 * mode 1 is 512 kHz clk / decimation 64 / 8 kHz Fs. The table now keys mode
 * 1 to 8000 Hz and adds mode 4 (HIGH_QUALITY_1024, 1024 kHz clk / decimation
 * 64 -- the SAME ratio as mode 1) for 16000 Hz; the FIR reuse across modes
 * 1-9 is grounded in the Alif reference's own driver test (sdk-alif
 * tests/drivers/pdm/src/alif_test_pdm.c), but mode 4 itself is a hypothesis
 * pending its own silicon bench run -- see pdm_clock_modes' comment.
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (4) ----------------------
 * Three more issue #2133 round-2 findings, all in the same configure/
 * trigger path:
 *  - HWRM 15.7.5.3.1 warns against back-to-back PDM_CTL0
 *    (PDM_CONFIG_REGISTER) writes with no APB transactions between them;
 *    the channel-enable write in configure() followed by the clock-mode
 *    write (now in DMIC_TRIGGER_START) had only the per-channel-defaults
 *    loop's writes to OTHER registers between them, and a second
 *    dmic_configure() call has even less. pdm_ctl0_write_spacer() (>= 4
 *    dummy reads of a harmless read-only register) now runs before every
 *    PDM_CONFIG_REGISTER write.
 *  - configure() used to write the clock-mode field itself, so the block
 *    started sampling (and clocking PDM_C0/PDM_C2) the moment configure()
 *    returned, and DMIC_TRIGGER_STOP never wrote MICROPHONE_SLEEP back --
 *    the mics stayed clocked forever after alp_audio_in_stop()/close().
 *    The resolved mode is now stored in pdm_data and written on
 *    DMIC_TRIGGER_START only; STOP and a PM_DEVICE_ACTION_SUSPEND both
 *    write MICROPHONE_SLEEP back.
 *  - HWRM 15.7.5.3.5: PDM_ERROR_IRQ/PDM_WARN_IRQ are edge-triggered,
 *    sticky, clear-on-read. Because the block starts sampling the instant
 *    the mode write leaves MICROPHONE_SLEEP, and the 4-bit FIFO count
 *    field overflows in ~1.9 ms at 8 kHz, a few ms of scheduling/printf
 *    delay before enable_interrupt() could latch an already-serviced
 *    overflow -- enable_interrupt()'s own write would then have
 *    pdm_error_handler() silently clear the overflow-enable bit for the
 *    rest of the session (a REAL later overflow then goes unreported),
 *    with block 0 starting with stale pre-START samples followed by a
 *    gap. DMIC_TRIGGER_START now writes the mode, THEN read-clears both
 *    sticky status registers and pulses FIFO_CLR, THEN enables
 *    interrupts -- clearing has to follow the mode write (nothing to
 *    clear before the block is sampling) and precede enable_interrupt()
 *    (so its own write can't latch stale status).
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (5) ----------------------
 * dmic_alif_pdm_read() (issue #2133 round 4b) used to hand back whatever
 * the queue held next even after the ISR silently dropped a burst -- a
 * slab-alloc miss (get_slab()) or a full delivery queue evicting its oldest
 * undelivered block -- so a caller reading a steady stream of blocks had no
 * way to know one was missing from the middle of it. pdm_data now carries
 * an `overrun` flag set at both drop sites; dmic_alif_pdm_read() checks it
 * before every k_msgq_get() and returns -EIO, sticky until the next
 * DMIC_TRIGGER_START, following the precedent in the upstream nxp dmic
 * driver (dmic_mcux.c's DMIC_STATE_ERROR, checked first thing in
 * dmic_mcux_read() before its own k_msgq_get() -- ZEPHYR_BASE
 * zephyr/drivers/audio/dmic_mcux.c). Reapply this divergence if the file is
 * ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (6) ----------------------
 * issue #2133 round 3: a mic's PDM clock spec is a BOARD fact the fork
 * never modelled -- dmic_alif_pdm_configure() used to accept any
 * HWRM/DFP-grounded mode regardless of whether the ATTACHED mic could
 * actually run at that clock (round 1/2 both silently under-clocked the
 * EVK's MP34DT05TR-A mics below their 1.2 MHz minimum). struct pdm_config
 * gains clk_frequency_min/clk_frequency_max (alif,alif-pdm.yaml, optional,
 * 0/UINT32_MAX when unset), and configure() rejects a mode outside the
 * INTERSECTION of that DT range and the caller's dmic_cfg.io window. Board
 * overlays that fit a known mic now declare its range. Reapply this
 * divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (7) ----------------------
 * issue #2133 round 4a: PDM_CONFIG_REGISTER read back 0x00010033 (mode 1)
 * from a PREVIOUS image on e1m-aen-evk-03, survived a `loadbin` reset, and
 * was still set after a refused dmic_configure() in the NEXT image --
 * pdm_initialize() (the fork's init function) only ever wrote
 * PDM_CTL_REGISTER/PDM_THRESHOLD_REGISTER, never PDM_CONFIG_REGISTER, so a
 * fresh image silently inherited whatever clock mode + channels the last
 * one left running. pdm_force_sleep() (clears the channel-enable AND
 * clock-mode fields, through the APB write-spacing helper) now runs at
 * cold init and on every refused configure(). PDM_INIT also gains a
 * per-instance BUILD_ASSERT: an inverted DT range
 * (clk_frequency_min > clk_frequency_max) fails the BUILD with an explicit
 * message instead of dmic_alif_pdm_configure() rejecting every mode with
 * an opaque per-attempt bound message at runtime. Reapply this divergence
 * if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (8) ----------------------
 * issue #2133 round 4c, three more silent-drop/state findings on top of
 * divergence (5)'s overrun flag:
 *  - DMIC_TRIGGER_START used to clear `overrun` itself without freeing the
 *    in-progress block or draining the queue, so a caller that restarted
 *    straight after an errored read (instead of stopping first) got a
 *    leaked slab block per recovery and never saw the -EIO the drop
 *    deserved. START now refuses (-EIO) when unconfigured or still
 *    flagged `overrun`, and is a no-op when already running -- mirroring
 *    dmic_mcux_trigger()'s DMIC_TRIGGER_START (ZEPHYR_BASE
 *    zephyr/drivers/audio/dmic_mcux.c:606-616). STOP is now the only place
 *    that clears `overrun`, after it frees/drains.
 *  - pdm_error_handler() read-cleared PDM_ERROR_IRQ without checking it, so
 *    a genuine hardware FIFO overflow was a THIRD silent drop alongside the
 *    slab-alloc-miss and queue-full paths -- now sets `overrun` too. Runs
 *    on both the HP PDM's dedicated error IRQ and, inline, LPPDM's shared
 *    warning IRQ. Round 4d correction: this originally tested bit 1
 *    (PDM_FIFO_OVERFLOW_IRQ), which is PDM_INTERRUPT_REGISTER's (enable)
 *    bit layout, not PDM_ERROR_IRQ's -- the SVD places FIFO_OVERFLOW_IRQ
 *    at bit 0 of the STATUS register, confirmed by the Alif DFP's own
 *    equality check against it (see pdm_error_handler()'s comment and
 *    PDM_ERROR_IRQ_FIFO_OVERFLOW_STAT, alif_pdm_reg.h). The wrong bit
 *    never actually detected a real overflow.
 *  - A refused configure() left pdata->clk_mode/channel_map at whatever a
 *    PRIOR successful configure() set, so a refused reconfigure followed
 *    by START would restart the OLD clock mode over hardware channels
 *    pdm_force_sleep() had just cleared. The `invalid:` path now resets
 *    both to their inert values, and PDM_MODE_MICROPHONE_SLEEP (0) doubles
 *    as START's "not configured" check.
 *  - pdm_initialize() force-sleeping on EVERY PM_DEVICE_ACTION_RESUME
 *    (divergence (7)) wiped a prior session's hardware channel-enable
 *    field on every resume. Split into pdm_hw_bringup(dev, cold_init) --
 *    only a genuine cold boot force-sleeps; resume does not.
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (9) ----------------------
 * issue #2133 round 4d, two more state findings, plus issue #2143's gain fix:
 *  - dmic_alif_pdm_configure() never checked whether a capture session was
 *    already active (pdata->record_data != 0), so a reconfigure mid-session
 *    could force-sleep the hardware / reset clk_mode out from under a
 *    still-running capture, or swap pdata->mem_slab out from under an
 *    in-progress data_buffer (the next STOP would then free it into the
 *    WRONG slab). pdm_configure_allowed() (alif_pdm_trigger_state.h) now
 *    refuses (-EBUSY) before touching any state, mirroring
 *    dmic_mcux_configure()'s DMIC_STATE_ACTIVE refusal (ZEPHYR_BASE
 *    zephyr/drivers/audio/dmic_mcux.c:421-424).
 *  - PM_DEVICE_ACTION_SUSPEND left record_data/overrun/the in-progress
 *    block/the queue exactly as an active session left them, so a
 *    post-resume DMIC_TRIGGER_START silently NOOPed over a block nothing
 *    was sampling into (dead capture, no error), and pdm_hw_bringup()'s
 *    k_msgq_init() on resume orphaned any still-queued blocks. SUSPEND now
 *    runs the same free/drain/record_data=0/overrun=false cleanup
 *    DMIC_TRIGGER_STOP does.
 *  - issue #2143: PDM_DEFAULT_CH_GAIN was 0x0000000D, Alif's register-level
 *    driver test value (sdk-alif tests/drivers/pdm/src/alif_test_pdm.h:
 *    88-123, and samples/drivers/audio/dmic_alif/src/main.c:22), never an
 *    audio-tuned value -- silicon measured this ~40 dB too quiet on the
 *    EVK's MP34DT05TR-A mics. The driver now reads a per-instance,
 *    DT-configurable `channel-gain` property (alif,alif-pdm.yaml) instead.
 *    PDM_CH_GAIN's GAIN field (bits [11:0]) is UNSIGNED 8.4 fixed-point
 *    (Alif SVD AE822FA0E5597BS0_CM55_HP_View.svd, PDM_CH_GAIN register,
 *    lines ~19081-19092; DFP PDM_MAX_GAIN_CTRL 0xFFFU) -- round 4d's
 *    "register width/scale undocumented" claim was wrong; it IS documented,
 *    just not in a source this driver had already grounded elsewhere.
 *    Phase (0x1F), FIR (pdm_default_fir_voice512), and the IIR coefficient
 *    (0x4) are UNCHANGED by this divergence; whether the IIR actually
 *    filters depends on the board's `bypass_iir_filter` DT property, which
 *    every current overlay sets to 1 (bypassed) -- "matches Alif's
 *    examples" is not a precise claim for a coefficient that isn't in
 *    effect. Round 4e correction: the 0x800 default (from GitHub's
 *    alifsemi/alif_ml-embedded-evaluation-kit mic_listener.c, not vendored
 *    locally) clipped on silicon (see divergence (10)) -- replaced with a
 *    provisional 0x200, see alif,alif-pdm.yaml's channel-gain description
 *    for the full silicon reasoning.
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (10) ---------------------
 * issue #2133 round 4e -- adversarial re-review corrected two round-4d
 * claims and the gain default:
 *  - The "clap test" that appeared to confirm live mics (divergence (9)'s
 *    era) was NOT a clap test: nobody clapped during that capture. The
 *    recorded bursts (p2p <= 48 at gain 0x0D) are unidentified room sound
 *    or electrical interference, not proof of acoustic liveness. Every
 *    "mics confirmed live" claim in this file, the example, and the docs
 *    is removed as of this round; register-level configuration and the
 *    48 kHz rate remain proven, acoustic capture does not (round 4f later
 *    verifies acoustic capture by a different method -- see divergence
 *    (11) -- this round's "clap test" is still not evidence of anything).
 *  - PM_DEVICE_ACTION_SUSPEND (divergence (9)) freed data_buffer and
 *    drained buf_queue while interrupts were still enabled and
 *    record_data was still 1 -- the ISR could touch data_buffer/buf_queue
 *    concurrently with the free/drain, a use-after-free/double-free
 *    hazard DMIC_TRIGGER_STOP does not have (it disables interrupts and
 *    zeroes record_data FIRST). SUSPEND now does the same ordering.
 *  - A silicon run at gain 0x800 (quiet room, no controlled stimulus,
 *    e1m-aen-evk-03, image built from 7e2535d63) found every unclipped
 *    sample a multiple of 0x80 (the gain is a saturating multiply AFTER
 *    the datapath quantizes -- a coarser gain adds no resolution) and the
 *    start-of-capture / post-restart window pinned at +/-32767 (decimator
 *    settling, not signal) -- 0x800 was too high a default. Replaced with
 *    a provisional 0x200; see alif,alif-pdm.yaml's channel-gain
 *    description.
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (11) ---------------------
 * issue #2133 round 4f -- acoustic capture VERIFIED (by a real method,
 * unlike round 4d/4e's non-clap), plus three more corrections:
 *  - A speaker-to-mic loopback on `e1m-aen-evk-03` (PROBE_LOOPBACK mode of
 *    examples/aen/aen-i2s-tas2563-probe on branch
 *    test/u46-i2s-tas2563-on-reworked-mux, issue #2143 -- not this example;
 *    TAS2563 speakers, independently verified audible, playing known tones
 *    into the PDM mics at gain 0x200) found frequency-correct, level-correct
 *    signal on mic ch0/ch1 (PDM controller 0) via Goertzel-bin analysis --
 *    see this file's top-of-file STATUS comment for the numbers. Acoustic
 *    capture at 48 kHz on ch0/ch1 is now proven; the D2 pair (HW 4/5) is
 *    register-level only, never acoustically tested; full-scale headroom
 *    at gain 0x200 is unmeasured.
 *  - The example's per-channel signal-level thresholds (`MIN_SIGNAL_RMS_
 *    LSB`/`MIN_SIGNAL_PEAK_TO_PEAK_LSB`, examples/aen/aen-pdm-mic-alif/
 *    src/main.c) were sized at the OLD gain default (0x0D); at the current
 *    0x200 they sat BELOW idle noise (idle p2p ~96-128 vs. a 64 threshold),
 *    so a dead or disconnected channel could have printed as passing. Now
 *    scaled proportionally to `channel-gain`.
 *  - PM_DEVICE_ACTION_SUSPEND (divergence (10)) fixed interrupt/record_data
 *    ordering but left `pdata->clk_mode` untouched, matching resume's
 *    "don't wipe channel-enable" intent (divergence (7)) -- but that
 *    intent assumes register RETENTION across the low-power transition.
 *    If the domain actually powers down, `clk_mode` surviving in software
 *    while the hardware's channel-enable/FIR/GAIN registers do NOT survive
 *    means a post-resume DMIC_TRIGGER_START proceeds (pdm_decide_start()
 *    sees "configured") over dead register state instead of refusing.
 *    SUSPEND now also resets `clk_mode` to PDM_MODE_MICROPHONE_SLEEP, so
 *    START correctly refuses as "not configured" until the app calls
 *    configure() again -- fails safe regardless of whether the domain
 *    actually lost power. Latent today (CONFIG_PM_DEVICE is off on every
 *    board this driver ships on).
 * Reapply this divergence if the file is ever re-synced from the fork.
 * -------------------------------------------------------------------------
 */

#define DT_DRV_COMPAT alif_alif_pdm

#include <string.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/pdm/pdm_alif.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/init.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/util.h>
#include "alif_pdm_reg.h"
#include "alif_pdm_burst_plan.h"
#include "alif_pdm_chanmap.h"
#include "alif_pdm_trigger_state.h"

/* Upper bound on how many slab blocks a single IRQ burst can be split
 * across (#1122). A burst is at most MAX_DATA_ITEMS * MAX_NUM_CHANNELS *
 * sizeof(uint16_t) = 128 bytes; this comfortably covers every block_size
 * a real audio slab configures (down to single-digit bytes) while keeping
 * the ISR's chunk-plan array small and stack-bounded. Configurations
 * needing more chunks than this are rejected at runtime (burst dropped,
 * logged) rather than risk an unbounded ISR stack allocation.
 */
#define MAX_PDM_BURST_CHUNKS 20

LOG_MODULE_REGISTER(alif_pdm, LOG_LEVEL_INF);

#define DEV_DATA(dev) ((struct pdm_data *)((dev)->data))
#define DEV_CFG(dev)  ((const struct pdm_config *)((dev)->config))

struct pdm_data {
	DEVICE_MMIO_RAM;
	struct k_mem_slab *mem_slab;
	uint32_t block_size;
	struct k_msgq buf_queue;
	uint8_t channel_map;
	uint32_t num_channels;
	uint8_t *data_buffer;
	uint32_t buf_index;
	uint32_t slab_missed;
	/* Set when the ISR ever dropped audio data (a slab-alloc failure --
	 * see get_slab() -- or the delivery queue was full and the OLDEST
	 * undelivered block was evicted below) so dmic_alif_pdm_read() can
	 * report the gap instead of silently splicing the next arriving
	 * block onto it (issue #2133 round 4b; precedent: dmic_mcux.c's
	 * DMIC_STATE_ERROR, checked in dmic_mcux_read() before every
	 * k_msgq_get(), ZEPHYR_BASE zephyr/drivers/audio/dmic_mcux.c).
	 * Sticky like that precedent -- cleared only by DMIC_TRIGGER_STOP
	 * (issue #2133 round 4c: originally cleared by START itself, which
	 * skipped freeing the in-progress block/draining the queue and let a
	 * restart-without-stopping silently erase the report), so a caller
	 * that ignores one -EIO still cannot get a later read to silently
	 * succeed past the gap.
	 */
	bool overrun;
	uint32_t record_data;
	uint32_t bytes_got;
	uint8_t bypass_iir_filter;
	/* PDM_MODE resolved by dmic_alif_pdm_configure() but only WRITTEN by
	 * DMIC_TRIGGER_START/STOP (issue #2133 round 2 divergence (4)) -- see
	 * the file header for why configure() itself must not start the
	 * clock. */
	uint8_t clk_mode;
	void *queue_data[MAX_QUEUE_LEN];
	uint16_t data[MAX_NUM_CHANNELS * MAX_DATA_ITEMS];
};

struct pdm_config {
	DEVICE_MMIO_ROM;
	void (*irq_config)(void);
	uint32_t fifo_watermark;
	/*
	 * Whether this instance has its OWN error / audio-detect interrupt
	 * lines.  PDM does; LPPDM folds both into the warning interrupt, so the
	 * warning ISR has to service them itself.  These come from
	 * DT_INST_IRQ_HAS_NAME() at instantiation -- the warning ISR used to ask
	 * DT_NODE_HAS_PROP(DT_NODELABEL(dev), ...) with `dev` the C function
	 * parameter, which expands to an undefined node token and is therefore
	 * always 0, so both branches ran on every instance (#1826).
	 */
	bool                             has_error_irq;
	bool                             has_audio_det_irq;
	const struct pinctrl_dev_config *pcfg;
	const struct device *clk_dev;
	clock_control_subsys_t clkid;
	/* Board-level mic PDM clock range (issue #2133 round 3), DT property
	 * names matching Zephyr's own pdm-dmic.yaml (round 4a) -- e.g. the
	 * E1M-EVK's MP34DT05TR-A mics need 1.2-3.25 MHz. 0 / UINT32_MAX
	 * (unset in DT) mean "no board constraint here"; configure() only
	 * enforces the caller's dmic_cfg.io window in that case.
	 */
	uint32_t clk_frequency_min;
	uint32_t clk_frequency_max;
	/* PDM_CH_GAIN applied to every enabled channel (issue #2143) -- see
	 * alif,alif-pdm.yaml's channel-gain for the provenance/default.
	 */
	uint32_t channel_gain;
};

/* pcm_rate -> PDM clock-mode table (issue #2133). Each entry's PDM bit-clock
 * frequency is HWRM Table 15-118 (mode -> clock divisor -> decimation ->
 * Fs); the Alif DFP's ARM_PDM_MODE_* names (Driver_PDM.h) agree: mode 1 is
 * *_8K_DECM_64, mode 4 is *_16K_DECM_64. Do not add a rate here without a
 * citable HWRM/DFP source -- see securing-the-alp-sdk-position. HWRM is
 * NDA'd: cite section/table numbers only, never a filesystem path, never
 * copy its tables wholesale into this repo.
 *
 * pdm_clock_mode_for_rate() also returns pdm_clk_hz so configure() can
 * reject a mode whose PDM bit clock falls outside the caller's declared
 * mic-clock window (dmic_cfg.io) -- previously read by nothing.
 */
struct pdm_clock_mode_entry {
	uint32_t pcm_rate_hz;
	uint8_t mode;
	uint32_t pdm_clk_hz;
};

static const struct pdm_clock_mode_entry pdm_clock_modes[] = {
	/* Mode 1 (STANDARD_VOICE_512): 512 kHz clk, decimation 64 -> 8 kHz Fs
	 * (HWRM Table 15-118). PDM_CONFIG_REGISTER read back 0x00010033 on
	 * e1m-aen-evk-03 and the FIFO count moved (issue #2133 round 2), but
	 * 512 kHz is BELOW the EVK's MP34DT05TR-A mics' 1.2 MHz minimum
	 * (ST's in-tree mpxxdtyy.h: MPXXDTYY_MIN_PDM_FREQ) -- round 3: that
	 * reading is an under-clocked mic, not proof of correct capture, and
	 * the EVK's board overlay now declares a DT mic-clock range that
	 * rejects this mode with -EINVAL. Left in the table because it is
	 * valid for a mic that DOES accept 512 kHz -- only the EVK's DT
	 * range rejects it, not this table.
	 */
	{ 8000U, PDM_MODE_STANDARD_VOICE_512_CLK_FRQ, 512000U },
	/* Mode 4 (HIGH_QUALITY_1024): 1024 kHz clk, decimation 64 -- the SAME
	 * ratio as mode 1 -- -> 16 kHz Fs (HWRM Table 15-118). Also below the
	 * EVK mics' 1.2 MHz minimum (see mode 1's note); same "valid
	 * elsewhere, rejected here by DT" status, not yet bench-verified on
	 * any mic. The FIR operates on the DECIMATED stream, so the same
	 * decimation ratio (64) as mode 1 means pdm_default_fir_voice512 is
	 * EXPECTED to apply unchanged -- and Alif's own driver test applies
	 * the identical per-channel FIR tables across every mode 1-9 with no
	 * FIR switch (sdk-alif tests/drivers/pdm/src/alif_test_pdm.c, FIR
	 * tables + mode select). That makes the reuse VENDOR-SOURCED, but
	 * this entry is still a HYPOTHESIS.
	 */
	{ 16000U, PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ, 1024000U },
	/* Mode 5 (WIDE_BANDWIDTH_AUDIO_1536): 1536 kHz clk, decimation 48 ->
	 * 32 kHz Fs (HWRM Table 15-118) -- IN SPEC for the EVK's MP34DT05TR-A
	 * mics (1.2-3.25 MHz). Decimation (48) differs from the proven modes
	 * 1/4 (64), so the FIR-reuse argument is LESS direct than mode 7's --
	 * still sourced from the same sdk-alif driver test applying one FIR
	 * set across modes 1-9, but not itself bench-verified (issue #2133
	 * round 3).
	 */
	{ 32000U, PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ, 1536000U },
	/* Mode 7 (FULL_BANDWIDTH_AUDIO_3071): 3072 kHz clk, decimation 64 --
	 * the SAME ratio as modes 1/4 -- -> 48 kHz Fs (HWRM Table 15-118).
	 * IN SPEC for the EVK's MP34DT05TR-A mics (1.2-3.25 MHz) and the
	 * mode this example now defaults to. Same direct FIR-reuse argument
	 * as mode 4 (sdk-alif tests/drivers/pdm/src/alif_test_pdm.c:34-100
	 * FIR tables, :245-282 mode select, no FIR switch across modes 1-9).
	 * SILICON RESULT (issue #2133 round 4a, e1m-aen-evk-03): the mode IS
	 * programmed and HELD correctly -- PDM_CONFIG_REGISTER read back
	 * 0x00070033 throughout capture, 38400-byte blocks (4800 frames x 4
	 * ch x 2 B) arrived as configured -- but the app's `measured_rate_hz`
	 * was ~32 kHz (two runs), not 48 kHz.
	 *
	 * ROUND 4C: that example's rate measurement counted frames delivered
	 * per wall-clock time in ITS OWN read loop, so it measured how fast
	 * the CONSUMER pulled blocks, not the PDM sample clock -- round 4b
	 * found the example's own per-sample stats loop was slow enough
	 * (soft-float double-precision Welford, ~150 ms/block against a
	 * 100 ms block period, no CONFIG_FPU) to pace that measurement on
	 * its own. The example was fixed to integer-only stats.
	 *
	 * ROUND 4D SILICON RESULT (commit 68a169977, e1m-aen-evk-03, fixed
	 * consumer): a fresh run measured `measured_rate_hz=48000` exactly,
	 * with `slab_missed=0`, `overrun=0`, and no -EIO for the full run --
	 * mode 7 delivers 48 kHz PCM with no dropped blocks, CONFIRMED. A
	 * separate 30 s capture (same driver, patched read loop) recorded
	 * bursts on all 4 channels during a supposed "clap test" -- ROUND 4E
	 * CORRECTION: nobody actually clapped during that capture, so those
	 * bursts (peak p2p ch0=34 ch1=46 ch2=48 ch3=48, at the old gain
	 * 0x0000000D) are unidentified room sound or interference, NOT proof
	 * of live acoustic capture on their own. A mid-capture register
	 * readback during that same session found CH0 GAIN=0x0000000D -- see
	 * alif,alif-pdm.yaml's channel-gain property for the full gain
	 * provenance and round 4e's silicon-driven default change
	 * (issue #2143, divergence (9)/(10)).
	 *
	 * ROUND 4F: acoustic capture at 48 kHz IS now verified on mic ch0/ch1
	 * (PDM controller 0) only, by a different and actually controlled
	 * method -- a speaker-to-mic loopback (TAS2563 speakers playing known
	 * tones into the PDM mics, gain 0x200 readback-confirmed). The D2 pair
	 * (HW 4/5) is register-level only, never acoustically tested. See this
	 * file's top-of-file STATUS comment and divergence (11) for the
	 * numbers.
	 */
	{ 48000U, PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ, 3072000U },
};

static int pdm_clock_mode_for_rate(uint32_t pcm_rate_hz, uint8_t *mode_out,
				    uint32_t *pdm_clk_hz_out)
{
	for (size_t i = 0; i < ARRAY_SIZE(pdm_clock_modes); i++) {
		if (pdm_clock_modes[i].pcm_rate_hz == pcm_rate_hz) {
			*mode_out = pdm_clock_modes[i].mode;
			*pdm_clk_hz_out = pdm_clock_modes[i].pdm_clk_hz;
			return 0;
		}
	}
	return -EINVAL;
}

/* HWRM 15.7.5.3.1: avoid consecutive PDM_CTL0 (this driver's
 * PDM_CONFIG_REGISTER) writes without at least ~4 APB transactions between
 * them (issue #2133 round 2 divergence (4)). Every PDM_CONFIG_REGISTER
 * writer calls this immediately before its sys_write32(). Reading the
 * (read-only) FIFO status register is a harmless APB transaction that
 * cannot itself disturb PDM_CTL0 state.
 */
static void pdm_ctl0_write_spacer(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	for (int i = 0; i < 4; i++) {
		(void)sys_read32(reg_base + PDM_FIFO_STATUS_REGISTER);
	}
}

/* Force PDM_CONFIG_REGISTER's channel-enable AND clock-mode fields to their
 * inert state (no channels enabled, MICROPHONE_SLEEP) -- issue #2133 round
 * 4a. Silicon observation on e1m-aen-evk-03: PDM_CONFIG_REGISTER read back
 * 0x00010033 (mode 1, channels 0/1/4/5) left over from a PREVIOUS image,
 * survived the `loadbin` reset, and was STILL set after a refused
 * dmic_configure() in the NEXT image -- pdm_initialize() only ever wrote
 * PDM_CTL_REGISTER/PDM_THRESHOLD_REGISTER, never touching
 * PDM_CONFIG_REGISTER, so a fresh image silently inherited whatever clock
 * mode + channels the last one left running and kept clocking the mics for
 * the life of the image even though the app never started a capture.
 * Called from pdm_initialize() before anything else touches the block, and
 * from every -EINVAL return path in dmic_alif_pdm_configure().
 */
static void pdm_force_sleep(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t reg_val;

	pdm_ctl0_write_spacer(dev);
	reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);
	reg_val &= ~(uint32_t)PDM_CHANNEL_ENABLE;
	reg_val &= ~PDM_CLK_MODE_MASK;
	sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);
}

/* Per-channel FIR decimation-filter coefficients for STANDARD_VOICE_512 --
 * see the pdm_clock_modes provenance note above. */
static const uint32_t pdm_default_fir_voice512[PDM_MAX_FIR_COEFFICIENT] = {
	0x00000001, 0x00000003, 0x00000003, 0x000007F4, 0x00000004, 0x000007ED,
	0x000007F5, 0x000007F4, 0x000007D3, 0x000007FE, 0x000007BC, 0x000007E5,
	0x000007D9, 0x00000793, 0x00000029, 0x0000072C, 0x00000072, 0x000002FD,
};
#define PDM_DEFAULT_CH_PHASE          0x0000001FUL
#define PDM_DEFAULT_CH_PEAK_DETECT_TH 0x00060002UL
#define PDM_DEFAULT_CH_PEAK_DETECT_IT 0x0004002DUL
#define PDM_DEFAULT_CH_IIR_COEF       0x00000004UL /* DC-block IIR, bypassed on every
                                                      * current overlay (issue #2133 round
                                                      * 4e) -- if ever un-bypassed, a
                                                      * coefficient of 0x4 gives a ~480 Hz
                                                      * corner at 48 kHz (the reset value,
                                                      * 0x9, gives ~15 Hz). */

/* Prime one hardware channel's FIR/IIR/gain/phase/peak-detect state for
 * STANDARD_VOICE_512 -- same calls, same values, as
 * examples/aen/aen-pdm-mic-alif used to do by hand before every configure().
 * FIR/phase match Alif's own audio-capture examples (issue #2133); the IIR
 * coefficient (PDM_DEFAULT_CH_IIR_COEF, 0x4) does too, but whether it
 * actually filters depends on the board's `bypass_iir_filter` DT property
 * -- every current overlay bypasses it, same as Alif's examples. The gain
 * comes from the DT-configurable cfg->channel_gain, NOT a hardcoded
 * default -- see divergence (9)/(10) (issue #2143).
 */
static void pdm_apply_channel_defaults(const struct device *dev, uint8_t hw_ch)
{
	const struct pdm_config *cfg = DEV_CFG(dev);
	struct pdm_ch_config     cc = { 0 };

	pdm_set_ch_phase(dev, hw_ch, PDM_DEFAULT_CH_PHASE);
	pdm_set_ch_gain(dev, hw_ch, cfg->channel_gain);
	pdm_set_peak_detect_th(dev, hw_ch, PDM_DEFAULT_CH_PEAK_DETECT_TH);
	pdm_set_peak_detect_itv(dev, hw_ch, PDM_DEFAULT_CH_PEAK_DETECT_IT);

	cc.ch_num = hw_ch;
	memcpy(cc.ch_fir_coef, pdm_default_fir_voice512, sizeof(cc.ch_fir_coef));
	cc.ch_iir_coef = PDM_DEFAULT_CH_IIR_COEF;
	pdm_channel_config(dev, &cc);
}

/**
 * @fn		int dmic_alif_pdm_configure(const struct device *dev,
 *						struct dmic_cfg *config)
 * @brief	Configures requested number of channels, block size and
 *			enable the PDM  channels etc.
 * @param[in]   dev	: pointer to Runtime device structure
 * @param[in]   config  : Pointer to the dmic_cfg structure which contains
 *						  the input configuration.
 * @return	  Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_configure(const struct device *dev, struct dmic_cfg *config)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	const struct pdm_config *cfg = DEV_CFG(dev);
	uintptr_t reg_base;
	uint32_t reg_val;
	uint8_t hw_chan_mask;
	uint8_t clk_mode;
	uint32_t pdm_clk_hz;
	int rc;

	/* Mirror dmic_mcux_configure()'s DMIC_STATE_ACTIVE refusal (issue
	 * #2133 round 4d) -- see pdm_configure_allowed()'s header comment
	 * for the two failure sequences this closes. Checked BEFORE touching
	 * any hardware state, so a refused reconfigure during an active
	 * session leaves the running capture completely undisturbed.
	 */
	if (!pdm_configure_allowed(pdata->record_data != 0)) {
		LOG_ERR("Cannot configure device while it is active");
		return -EBUSY;
	}

	reg_base = DEVICE_MMIO_GET(dev);
	reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	if (config->channel.req_num_chan == 0 || config->channel.req_num_chan > MAX_NUM_CHANNELS) {
		LOG_DBG("config invalid: number of channels not valid\n");
		rc = -EINVAL;
		goto invalid;
	}

	/* A zero block_size would make the IRQ-burst rollover split loop
	 * divide progress by zero (#1122) -- reject before capture can ever
	 * start.
	 */
	if (config->streams[0].block_size == 0) {
		LOG_DBG("config invalid: block size must be non-zero\n");
		rc = -EINVAL;
		goto invalid;
	}

	/* Decode the STANDARD Zephyr channel-map encoding (issue #2133)
	 * instead of reading req_chan_map_lo's low byte as if it were
	 * already the hardware bitmask -- see alif_pdm_chanmap.h for the
	 * pdm-controller -> HW-channel grounding, and for why a duplicate/
	 * out-of-order map is also rejected here.
	 */
	rc = alif_pdm_chanmap_translate(config->channel.req_chan_map_lo,
					 config->channel.req_chan_map_hi,
					 config->channel.req_num_chan, &hw_chan_mask);
	if (rc != 0) {
		LOG_DBG("config invalid: channel map not expressible in hardware\n");
		goto invalid;
	}

	/* Reject a pcm_rate with no bench/vendor-grounded clock mode rather
	 * than leave the block silently asleep (issue #2133).
	 */
	rc = pdm_clock_mode_for_rate(config->streams[0].pcm_rate, &clk_mode, &pdm_clk_hz);
	if (rc != 0) {
		LOG_DBG("config invalid: no PDM clock mode for pcm_rate=%u\n",
			config->streams[0].pcm_rate);
		goto invalid;
	}

	/* Reject a mode whose PDM bit clock falls outside the INTERSECTION of
	 * the caller's declared io window (config->io, previously read by
	 * nothing -- issue #2133 round 2) and the board's mic clock range,
	 * if the devicetree node declares one (clk-frequency-min/
	 * clk-frequency-max, alif,alif-pdm.yaml -- round 3, renamed to
	 * Zephyr's own pdm-dmic.yaml property names in round 4a). A mic's
	 * clock spec is a BOARD fact, not something the caller or this
	 * driver should know -- e.g. the E1M-EVK's MP34DT05TR-A needs
	 * 1.2-3.25 MHz (mpxxdtyy.h MPXXDTYY_MIN/MAX_PDM_FREQ), well above the
	 * 512 kHz/1024 kHz modes this driver otherwise supports. 0 /
	 * UINT32_MAX mean "DT declares no board constraint" -- the
	 * intersection then collapses to config->io alone. An INVERTED DT
	 * range (min > max) is a build-time error -- see PDM_INIT's
	 * BUILD_ASSERT -- so eff_min <= eff_max is guaranteed here whenever
	 * both DT bounds are set.
	 */
	{
		uint32_t eff_min = MAX(cfg->clk_frequency_min, config->io.min_pdm_clk_freq);
		uint32_t eff_max = MIN(cfg->clk_frequency_max, config->io.max_pdm_clk_freq);

		if (pdm_clk_hz < eff_min) {
			LOG_DBG("config invalid: mode clock %u Hz below mic minimum %u Hz\n",
				pdm_clk_hz, eff_min);
			rc = -EINVAL;
			goto invalid;
		}
		if (pdm_clk_hz > eff_max) {
			LOG_DBG("config invalid: mode clock %u Hz above mic maximum %u Hz\n",
				pdm_clk_hz, eff_max);
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (pdata) {
		pdata->mem_slab = config->streams[0].mem_slab;
		pdata->block_size = config->streams[0].block_size;
		pdata->channel_map = hw_chan_mask;
		/* Resolved but NOT written here -- DMIC_TRIGGER_START writes
		 * it (issue #2133 round 2 divergence (4)): a configure() that
		 * starts the clock itself leaves the mics clocked from the
		 * moment the app calls dmic_configure(), long before
		 * dmic_trigger(START), and DMIC_TRIGGER_STOP had nothing to
		 * put back to sleep.
		 */
		pdata->clk_mode = clk_mode;

		pdm_ctl0_write_spacer(dev);

		/* Replace the channel-enable field rather than OR into
		 * whatever the register already held -- a previous
		 * configure() call's channels must not leak into this one
		 * (issue #2133).
		 */
		reg_val &= ~(uint32_t)PDM_CHANNEL_ENABLE;
		reg_val |= hw_chan_mask;

		/* Enable the PDM multiple channels */
		sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);

		pdata->num_channels = config->channel.req_num_chan;

		/* Standard dmic_configure() contract: prime every enabled
		 * channel's FIR/IIR/gain/phase/peak-detect defaults here so
		 * DMIC_TRIGGER_START's clock-mode write (which is what
		 * actually starts sampling) lands on an already-configured
		 * channel bank. Same values as
		 * examples/aen/aen-pdm-mic-alif used to set by hand.
		 * pdm_channel_config()/pdm_mode() stay public so an app can
		 * still override these afterward.
		 */
		for (uint8_t hw_ch = 0; hw_ch < MAX_NUM_CHANNELS; hw_ch++) {
			if (hw_chan_mask & (1U << hw_ch)) {
				pdm_apply_channel_defaults(dev, hw_ch);
			}
		}

		LOG_DBG("block size: %d\n", pdata->block_size);
	}

	LOG_DBG("DMIC configure okay\n");

	return 0;

invalid:
	/* A REFUSED configure() must not leave the block sampling under
	 * whatever channels/mode a PRIOR successful configure()+trigger()
	 * left running (issue #2133 round 4a) -- the app has no reason to
	 * expect an errored call left hardware state behind. */
	pdm_force_sleep(dev);
	if (pdata) {
		/* Also reset the SOFTWARE copies (issue #2133 round 4c): a
		 * PRIOR successful configure() could have left pdata->clk_mode/
		 * channel_map non-zero; without this, a refused reconfigure
		 * followed by DMIC_TRIGGER_START would restart the OLD clock
		 * mode over the hardware channel-enable field pdm_force_sleep()
		 * just cleared above -- reads then time out with zero channels
		 * actually enabled. PDM_MODE_MICROPHONE_SLEEP (0) also doubles
		 * as dmic_alif_pdm_trigger()'s "not configured" check.
		 */
		pdata->clk_mode = PDM_MODE_MICROPHONE_SLEEP;
		pdata->channel_map = 0;
	}
	return rc;
}

/**
 * @fn		void pdm_channel_config(const struct device *dev,
 *					struct pdm_ch_config *cnfg)
 * @brief	Sets FIR coefficient and IIR coefficient values.
 * @param[in]	dev  : Pointer to the runtime device structure.
 * @param[in]	cnfg : Pointer to the pdm_ch_config structure.
 * @return	    None
 */
void pdm_channel_config(const struct device *dev, struct pdm_ch_config *cnfg)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint8_t i;
	uintptr_t ch_n_fir_coef_0 = reg_base + PDM_CH_FIR_COEF + (cnfg->ch_num * PDM_CH_OFFSET);

	/* Store the FIR coefficient values. Each write goes through sys_write32()
	 * (a volatile MMIO accessor) rather than a plain pointer store: the
	 * compiler is otherwise free to reorder, combine or elide stores to a
	 * non-volatile object, which would silently corrupt the coefficient bank.
	 */
	for (i = 0; i < PDM_MAX_FIR_COEFFICIENT; i++) {
		sys_write32(cnfg->ch_fir_coef[i],
			    ch_n_fir_coef_0 + ((uintptr_t)i * sizeof(uint32_t)));
	}

	uintptr_t ch_n_iir_coef = (reg_base + PDM_CH_IIR_COEF_SEL + (cnfg->ch_num * PDM_CH_OFFSET));

	/* Store the IIR coefficient values */
	sys_write32(cnfg->ch_iir_coef, ch_n_iir_coef);
}

/**
 * @fn		void pdm_set_ch_phase(const struct device *dev,
 *					uint8_t ch_num,
 *					uint32_t ch_phase)
 * @brief	Sets the PDM channel phase control value
 * @param[in]	dev  : Pointer to the runtime device structure.
 * @param[in]	ch_num : PDM channel number.
 * @param[in]	ch_phase : PDM channel phase control value.
 * @return	    None
 */
void pdm_set_ch_phase(const struct device *dev, uint8_t ch_num, uint32_t ch_phase)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_phase = (reg_base + PDM_CH_PHASE + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_phase, ch_n_phase);
}

/**
 * @fn		void pdm_set_ch_gain(const struct device *dev,
 *					uint8_t ch_num,
 *					uint32_t ch_gain)
 * @brief	Sets the PDM channel gain control value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_gain	: PDM channel gain control value. Bits [11:0] only
 *			(issue #2133 round 4f) -- values above PDM_CH_GAIN_MAX
 *			are clamped, with a LOG_WRN, rather than written
 *			unclamped (which would overflow the field and mute
 *			the channel).
 * @return	    None
 */
void pdm_set_ch_gain(const struct device *dev, uint8_t ch_num, uint32_t ch_gain)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_gain = (reg_base + PDM_CH_GAIN + (ch_num * PDM_CH_OFFSET));

	/* PDM_CH_GAIN's GAIN field is only 12 bits (issue #2133 round 4f) --
	 * a value above PDM_CH_GAIN_MAX would truncate to bits [11:0]
	 * instead of clipping loud as a caller might expect (see
	 * PDM_CH_GAIN_MAX's own comment in alif_pdm_reg.h). This function
	 * returns void (public API, existing callers), so it cannot report
	 * -EINVAL -- clamp (pdm_ch_gain_clamp(), alif_pdm_reg.h -- host-
	 * tested, issue #2133 round 5) and warn instead of writing a value
	 * that truncates. The one in-tree caller
	 * (pdm_apply_channel_defaults(), via cfg->channel_gain) is already
	 * bounded at DT-build time by PDM_INIT's BUILD_ASSERT; this guards
	 * an app calling pdm_set_ch_gain() directly with a bad runtime
	 * value.
	 */
	if (ch_gain > PDM_CH_GAIN_MAX) {
		LOG_WRN("ch_gain 0x%x exceeds PDM_CH_GAIN's 12-bit field (max 0x%x) -- "
		        "clamping (an unclamped write would truncate to bits [11:0])",
		        ch_gain,
		        PDM_CH_GAIN_MAX);
	}
	ch_gain = pdm_ch_gain_clamp(ch_gain);

	sys_write32(ch_gain, ch_n_gain);
}

/**
 * @fn		void pdm_set_peak_detect_th(const struct device *dev,
 *						uint8_t ch_num,
 *						uint32_t ch_peak_detect_th)
 * @brief	Sets the PDM channel  Peak detector threshold value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_peak_detect_th : PDM channel  Peak detector
 *				threshold value.
 * @return		None
 */
void pdm_set_peak_detect_th(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_th)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_pkdet_th = (reg_base + PDM_CH_PKDET_TH + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_peak_detect_th, ch_n_pkdet_th);
}

/**
 * @fn		void pdm_set_peak_detect_itv(const struct device *dev,
 *						uint8_t ch_num,
 *						uint32_t ch_peak_detect_itv)
 * @brief	Sets the PDM channel  Peak detector interval value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_peak_detect_itv : PDM channel  Peak detector
 *				interval value.
 * @return		None
 */
void pdm_set_peak_detect_itv(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_itv)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_pkdet_itv = (reg_base + PDM_CH_PKDET_ITV + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_peak_detect_itv, ch_n_pkdet_itv);
}

/**
 * @fn		void pdm_mode(const struct device *dev, uint8_t mode)
 * @brief	Sets the PDM modes
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	mode	: pdm frequency modes
 * @return		None
 */
void pdm_mode(const struct device *dev, uint8_t mode)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/*
	 * HWRM 15.7.5.3.1 PDM_CTL0 bits 19-16 PDM_MODE select ONE of modes
	 * 0x0-0x9, each with its own clock divisor, decimation ratio and
	 * sampling rate (Table 15-118).  OR-ing without clearing first made
	 * pdm_mode(dev, 5) then pdm_mode(dev, 2) leave 5 | 2 = 7 -- a third rate
	 * neither caller asked for, and no error (#1826).
	 */
	reg_val &= ~PDM_CLK_MODE_MASK;
	reg_val |= ((uint32_t)mode << PDM_CLK_MODE) & PDM_CLK_MODE_MASK;

	/* HWRM 15.7.5.3.1: space this PDM_CTL0 write from whatever wrote it
	 * last (issue #2133 round 2 divergence (4)) -- e.g. the channel-enable
	 * write dmic_alif_pdm_configure() just made. */
	pdm_ctl0_write_spacer(dev);
	sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);
}

/**
 * @fn		void enable_interrupt(const struct device *dev)
 * @brief		Enable the IRQ
 * @param[in]	dev : Pointer to the runtime device structure.
 * @return	None
 */
static void enable_interrupt(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t irq_value = 0;
	uint32_t audio_ch = 0;

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/* Store user enabled channel */
	audio_ch = reg_val & PDM_CHANNEL_ENABLE;

	irq_value |= (audio_ch << 8 | PDM_FIFO_ALMOST_FULL_IRQ | PDM_FIFO_OVERFLOW_IRQ);

	/* Enable the Interrupt */
	sys_write32(irq_value, reg_base + PDM_INTERRUPT_REGISTER);
}

/**
 * @fn		void disable_interrupt(const struct device *dev)
 * @brief		Disable the IRQ
 * @param[in]	dev : Pointer to the runtime device structure.
 * @return	None
 */
static void disable_interrupt(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	/* Disable the Interrupt */
	sys_write32(0, reg_base + PDM_INTERRUPT_REGISTER);
}

/**
 * @fn		int dmic_alif_pdm_trigger(const struct device *dev,
 *					enum dmic_trigger cmd)
 * @brief	Send DMIC_TRIGGER_STOP or DMIC_TRIGGER_START to
 *			perform the specific operation.
 * @param[in]   dev	: pointer to Runtime device structure
 * @param[in]   cmd	: DMIC start or stop command
 * @return	  Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	struct pdm_data *pdata = DEV_DATA(dev);

	switch (cmd) {
	case DMIC_TRIGGER_STOP:
		disable_interrupt(dev);

		/* Return the clock to MICROPHONE_SLEEP (issue #2133 round 2
		 * divergence (4)) -- configure() no longer writes PDM_MODE, so
		 * without this PDM_C0/PDM_C2 would keep clocking the mics
		 * forever after alp_audio_in_stop()/close().
		 */
		pdm_mode(dev, PDM_MODE_MICROPHONE_SLEEP);

		pdata->record_data = 0;

		/* Free in-progress buffer to prevent slab leak */
		if (pdata->data_buffer != NULL) {
			k_mem_slab_free(pdata->mem_slab, pdata->data_buffer);
			pdata->data_buffer = NULL;
		}

		/* Drain queued buffers that the app hasn't read */
		void *buf;

		while (k_msgq_get(&pdata->buf_queue, &buf, K_NO_WAIT) == 0) {
			k_mem_slab_free(pdata->mem_slab, buf);
		}

		/* Clear the drop report HERE, not in START (issue #2133 round
		 * 4c): STOP is the point that actually frees the in-progress
		 * block and drains the queue above, so it is the only point
		 * where the session's dropped state is genuinely resolved.
		 * Clearing it in START let an app skip STOP and restart
		 * straight over a leaked in-progress block plus a queue still
		 * full of stale blocks from the errored session, with no
		 * -EIO ever reported for either.
		 */
		pdata->overrun = false;
		break;

	case DMIC_TRIGGER_START: {
		/* Pure refuse/no-op/proceed decision extracted to
		 * alif_pdm_trigger_state.h (issue #2133 round 4c) -- see that
		 * header for the full dmic_mcux precedent citation and
		 * tests/unit/alif_pdm_trigger_state for the host-testable
		 * proof.
		 */
		enum pdm_start_decision decision = pdm_decide_start(
		    pdata->clk_mode != PDM_MODE_MICROPHONE_SLEEP, pdata->overrun,
		    pdata->record_data != 0);

		if (decision == PDM_START_REFUSE) {
			/* Two distinct causes fold into the same decision (issue
			 * #2133 round 4d) -- log which one so a dead-capture
			 * report doesn't get misdiagnosed as the other.
			 */
			if (pdata->clk_mode == PDM_MODE_MICROPHONE_SLEEP) {
				LOG_ERR("Device is not configured");
			} else {
				LOG_ERR("Dropped PDM data pending -- call DMIC_TRIGGER_STOP "
					"before START");
			}
			return -EIO;
		}
		if (decision == PDM_START_NOOP) {
			break;
		}

		LOG_DBG("trigger start\n");
		pdata->record_data = 1;
		pdata->bytes_got = 0;
		pdata->buf_index = 0;
		pdata->data_buffer = NULL;
		pdata->slab_missed = 0;

		/* Order is load-bearing (issue #2133 round 2 divergence (4)):
		 * the mode write is what actually starts the block sampling,
		 * so clearing sticky status/FIFO before it would clear
		 * nothing useful; enabling interrupts before clearing risks
		 * enable_interrupt()'s own write latching an
		 * already-serviced overflow. Mode -> clear -> enable.
		 */
		pdm_mode(dev, pdata->clk_mode);

		{
			uintptr_t reg_base = DEVICE_MMIO_GET(dev);
			uint32_t reg_val;

			/* HWRM 15.7.5.3.5: PDM_ERROR_IRQ (0x10) and
			 * PDM_WARN_IRQ (0x14) are edge-triggered, sticky,
			 * clear-on-read. The 4-bit FIFO count field overflows
			 * in ~1.9 ms at 8 kHz, comfortably inside the
			 * mode-write-to-here window, so read-clear both
			 * before the first enable_interrupt() of this
			 * session -- otherwise that write would latch an
			 * already-serviced overflow and pdm_error_handler()
			 * would silently clear the overflow-enable bit for
			 * the rest of the session (a REAL later overflow
			 * then goes unreported).
			 */
			(void)sys_read32(reg_base + PDM_ERROR_IRQ);
			(void)sys_read32(reg_base + PDM_WARN_IRQ);

			/* Pulse FIFO_CLR (PDM_CONFIG_REGISTER bit 31) to drop
			 * whatever arrived in that same window, so block 0
			 * doesn't start with stale pre-clear samples followed
			 * by a gap. Read-modify-write so the channel-enable/
			 * mode bits this same register holds are preserved;
			 * write the un-set value back afterward regardless of
			 * whether FIFO_CLR is self-clearing in hardware.
			 */
			reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);
			pdm_ctl0_write_spacer(dev);
			sys_write32(reg_val | PDM_FIFO_CLEAR, reg_base + PDM_CONFIG_REGISTER);
			pdm_ctl0_write_spacer(dev);
			sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);
		}

		enable_interrupt(dev);
		break;
	}

	default:
		LOG_ERR("Invalid command: %d", cmd);
		return -EINVAL;
	}
	return 0;
}

/**
 * @fn		int dmic_alif_pdm_read(const struct device *dev,
 *					uint8_t stream,
 *					void **buffer, size_t *size,
 *					int32_t timeout)
 * @brief	Read the stored allocated block address in msg queue
 *			get the pcm samples.
 * @param[in]	dev	: pointer to Runtime device structure
 * @param[in]	stream	: stream configuration
 * @param[in]	buffer	: A pointer to the buffer where the
 *			  retrieved message will be copied.
 * @param[in]	size	: Size of the allocated block
 * @param[in]	timeout	: Maximum time to wait for a message
 * @return		Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_read(const struct device *dev, uint8_t stream, void **buffer, size_t *size,
			      int32_t timeout)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	int rc;

	/* A dropped burst (slab exhausted, the delivery queue evicted its
	 * oldest block, an unplannable burst, or a hardware FIFO overflow --
	 * see get_slab() / the ISR's queue-full path / pdm_error_handler())
	 * must fail every future read rather than let this call silently
	 * splice the next arriving block onto a gap the caller never sees
	 * (issue #2133 round 4b). Precedent: dmic_mcux.c's DMIC_STATE_ERROR
	 * check, first thing in dmic_mcux_read(), before its k_msgq_get()
	 * (ZEPHYR_BASE zephyr/drivers/audio/dmic_mcux.c). Sticky like that
	 * precedent: only DMIC_TRIGGER_STOP clears it (issue #2133 round 4c
	 * -- START used to clear it itself, defeating the report), so the
	 * caller must stop, then start again, rather than have a later read
	 * silently succeed past the gap.
	 */
	if (pdata->overrun) {
		LOG_DBG("read: data was dropped this session (slab exhausted, queue full, or "
			"hardware FIFO overflow); stop then start the capture to resume\n");
		return -EIO;
	}

	rc = k_msgq_get(&pdata->buf_queue, buffer, SYS_TIMEOUT_MS(timeout));

	if (rc != 0) {
		LOG_DBG("No audio data to be read\n");
	} else {
		*size = pdata->block_size;
	}
	return rc;
}

static inline void pdm_error_handler(const struct device *dev)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t error_status;

	sys_clear_bits(reg_base + PDM_INTERRUPT_REGISTER, PDM_FIFO_OVERFLOW_IRQ);

	/* PDM_ERROR_IRQ (register offset 0x10) is edge-triggered, sticky,
	 * clear-on-read -- this IS the read that clears it, so check its
	 * status bit before discarding the value. A hardware FIFO overflow
	 * is a THIRD silent drop alongside the slab-alloc-miss and
	 * queue-full paths (issue #2133 round 4c) -- it used to go
	 * unreported the same way they did before round 4b's `overrun` flag
	 * existed. This function runs from both the HP PDM's dedicated
	 * error_intr ISR (pdm_error_detect_irq_handler()) and, inline, from
	 * alif_pdm_warning_isr() for any instance without a named error_intr
	 * line (LPPDM folds error+audio-detect into the warning IRQ -- see
	 * the `!cfg->has_error_irq` call below), so this covers both paths.
	 *
	 * BIT POSITION (issue #2133 round 4d): PDM_ERROR_IRQ's
	 * FIFO_OVERFLOW_IRQ status bit is bit 0 -- PDM_ERROR_IRQ_FIFO_
	 * OVERFLOW_STAT, alif_pdm_reg.h, cited from the Alif SVD
	 * (AE822FA0E5597BS0_CM55_HP_View.svd, PDM_ERROR_IRQ register) and
	 * confirmed by the Alif DFP's own equality check against this
	 * register (drivers/source/pdm.c + include/pdm.h,
	 * PDM_INTERRUPT_STATUS_VALUE == 0x1U). This is NOT the same bit as
	 * PDM_FIFO_OVERFLOW_IRQ (bit 1), which names the DIFFERENT
	 * PDM_INTERRUPT_REGISTER (enable) bit layout used two lines above --
	 * an earlier version of this function wrongly reused that enable-bit
	 * constant to test the status register and so never actually
	 * detected a real overflow.
	 */
	error_status = sys_read32(reg_base + PDM_ERROR_IRQ);
	if (error_status & PDM_ERROR_IRQ_FIFO_OVERFLOW_STAT) {
		pdata->overrun = true;
	}
}

static inline void pdm_audio_det_handler(const struct device *dev)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	if (pdata->slab_missed != 0) {
		sys_clear_bits(reg_base + PDM_INTERRUPT_REGISTER, PDM_AUDIO_DETECT_IRQ_STAT);
	}
	(void)sys_read32(reg_base + PDM_AUDIO_DETECT_IRQ);
}
/**
 * @fn		static void pdm_error_detect_irq_handler()
 * @brief	ISR to handle the error interrupt
 * @param[in]	None
 * @return	None.
 */
static __maybe_unused void pdm_error_detect_irq_handler(const struct device *dev)
{
	pdm_error_handler(dev);
}

/**
 * @fn		static void pdm_audio_detect_irq_handler(const struct device *dev)
 * @brief	ISR to handle PDM audio detect interrupts.
 * @param[in]	dev	: pointer to Runtime device structure
 * @return	None.
 */
static __maybe_unused void pdm_audio_detect_irq_handler(const struct device *dev)
{
	pdm_audio_det_handler(dev);
}

/**
 * @fn		void *get_slab(struct pdm_data *pdm_data)
 * @brief	Allocates a memory block from the slab for PCM data.
 * @param[in]	pdm_data Pointer to the PDM data structure
 *			containing the memory slab.
 * @return		Pointer to the allocated memory block on
 *			Zero on success, and a negative value on failure.
 */
static void *get_slab(struct pdm_data *pdm_data)
{
	int rc;
	void *buffer;

	rc = k_mem_slab_alloc(pdm_data->mem_slab, &buffer, K_NO_WAIT);

	if (rc == 0) {
		LOG_DBG("Memory block allocated : %p\n", buffer);
	} else {
		pdm_data->slab_missed++;
		/* The consumer fell behind and a burst is about to be
		 * dropped -- mark the session so dmic_alif_pdm_read() reports
		 * it instead of silently handing back the next block as if
		 * nothing were missing (issue #2133 round 4b).
		 */
		pdm_data->overrun = true;
		return NULL;
	}

	return buffer;
}

/**
 * @fn		static void alif_pdm_warning_isr()
 * @brief	ISR to handle PDM warning interrupts.
 *			Collects audio data from the PDM channels, stores it
 *			in the buffer, and handles memory allocation and queue
 *			management.
 * @param[in]	dev	: pointer to Runtime device structure
 * @return	None.
 */
static void alif_pdm_warning_isr(const struct device *dev)
{
	struct pdm_data *pdmdata = DEV_DATA(dev);
	const struct pdm_config *cfg     = DEV_CFG(dev);
	uint8_t k = 0;
	uint8_t audio_ch;
	uint8_t intstatus;
	uintptr_t reg_base;
	uint32_t num_items;
	uint32_t data_bytes;
	uint32_t block_size;
	uint32_t bytes_available;
	uint32_t i;
	uint32_t audio_ch_0_1;
	uint32_t audio_ch_2_3;
	uint32_t audio_ch_4_5;
	uint32_t audio_ch_6_7;

	block_size = pdmdata->block_size;

	reg_base = DEVICE_MMIO_GET(dev);

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/* User enabled channel */
	audio_ch = reg_val & PDM_CHANNEL_ENABLE;

	intstatus = sys_read32(reg_base + PDM_WARN_IRQ);
	/*
	 * HWRM 15.7.5.3.4 PDM_FIFO_STAT: bits 31-4 RESERVED, bits 3-0 CNT --
	 * "Count of sets of audio data entry in FIFO for each channel available
	 * to be read".  The whole 32-bit word used to become the loop count, and
	 * the loop below writes up to MAX_NUM_CHANNELS uint16_t per iteration
	 * into a MAX_NUM_CHANNELS * MAX_DATA_ITEMS array.  CNT legitimately
	 * reaches 15, so with all eight channels enabled anything above
	 * MAX_DATA_ITEMS runs past the end of data[] -- the LAST member of
	 * struct pdm_data, so the overrun lands in whatever follows it.  The
	 * fifo_watermark binding caps the TRIGGER level at 7; that is not a
	 * ceiling on CNT, because any ISR-latency stall lets the FIFO fill
	 * further before this runs.  Mask to the field, then clamp to what
	 * data[] can hold (#1826).
	 */
	num_items = sys_read32(reg_base + PDM_FIFO_STATUS_REGISTER) & PDM_FIFO_STAT_CNT_MASK;
	num_items = MIN(num_items, MAX_DATA_ITEMS);

	/* LPPDM doesn't have separate error and audio detect isr handlers */
	if (!cfg->has_error_irq) {
		pdm_error_handler(dev);
	}

	if (!cfg->has_audio_det_irq) {
		pdm_audio_det_handler(dev);
	}

	for (i = 0; i < num_items; i++) {
		audio_ch_0_1 = sys_read32(reg_base + PDM_CH0_CH1_AUDIO_OUT);
		audio_ch_2_3 = sys_read32(reg_base + PDM_CH2_CH3_AUDIO_OUT);
		audio_ch_4_5 = sys_read32(reg_base + PDM_CH4_CH5_AUDIO_OUT);
		audio_ch_6_7 = sys_read32(reg_base + PDM_CH6_CH7_AUDIO_OUT);

		if ((audio_ch & PDM_CHANNEL_0) == PDM_CHANNEL_0) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_0_1);
		}
		if ((audio_ch & PDM_CHANNEL_1) == PDM_CHANNEL_1) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_0_1 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_2) == PDM_CHANNEL_2) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_2_3);
		}
		if ((audio_ch & PDM_CHANNEL_3) == PDM_CHANNEL_3) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_2_3 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_4) == PDM_CHANNEL_4) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_4_5);
		}
		if ((audio_ch & PDM_CHANNEL_5) == PDM_CHANNEL_5) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_4_5 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_6) == PDM_CHANNEL_6) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_6_7);
		}
		if ((audio_ch & PDM_CHANNEL_7) == PDM_CHANNEL_7) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_6_7 >> 16);
		}
	}

	if (pdmdata->record_data == 0) {
		return;
	}

	data_bytes = num_items * pdmdata->num_channels * sizeof(unsigned short);

	pdmdata->bytes_got += data_bytes;

	if (pdmdata->data_buffer == NULL) {

		pdmdata->data_buffer = get_slab(pdmdata);
		if (pdmdata->data_buffer == NULL) {
			/*
			 * The consumer is behind and the slab is momentarily
			 * empty.  Drop THIS burst (get_slab() already counted it
			 * in slab_missed) and leave the interrupts armed so
			 * capture resumes as soon as a block is freed.
			 *
			 * Writing 0 to PDM_INTERRUPT_REGISTER here used to mask
			 * every PDM interrupt including FIFO_ALMOST_FULL_IRQ_EN,
			 * and nothing re-armed them while record_data stayed 1 --
			 * so one transient slab exhaustion stopped capture for the
			 * life of the image and dmic_alif_pdm_read() only ever
			 * returned -EAGAIN (#1826).  Re-arming costs nothing: the
			 * warning interrupt is paced by the audio clock, so this
			 * path cannot spin faster than the sample rate.
			 */
			return;
		}
		pdmdata->buf_index = 0;
	}

	bytes_available = block_size - pdmdata->buf_index;

	/*
	 * Plan the split BEFORE touching any slab/queue state: a burst can
	 * exceed not just bytes_available but the configured block_size
	 * itself, so never assume a single replacement block is enough
	 * (#1122). Each chunk after the first starts a fresh block and is
	 * never larger than block_size, so no single memcpy can overrun a
	 * block.
	 */
	{
		uint32_t chunks[MAX_PDM_BURST_CHUNKS];
		size_t nchunks;
		uint32_t copied = 0;
		size_t idx;

		nchunks = pdm_plan_burst_chunks(bytes_available, data_bytes, block_size, chunks,
						 ARRAY_SIZE(chunks));
		if (nchunks == 0) {
			/* Cannot safely split this burst into MAX_PDM_BURST_CHUNKS
			 * blocks (a pathologically small block_size) -- drop the
			 * burst rather than risk writing past a block boundary.
			 */
			LOG_ERR("PDM burst too large to plan safely (data_bytes=%u "
				"block_size=%u); dropping burst\n",
				data_bytes, block_size);
			pdmdata->buf_index = 0;
			/* Same silent-gap hazard as the other drop paths
			 * (issue #2133 round 4c). */
			pdmdata->overrun = true;
			return;
		}

		for (idx = 0; idx < nchunks; idx++) {
			uint32_t chunk = chunks[idx];

			if (chunk > 0) {
				memcpy(pdmdata->data_buffer + pdmdata->buf_index,
				       (uint8_t *)pdmdata->data + copied, chunk);
				pdmdata->buf_index += chunk;
				copied += chunk;
			}

			if (idx + 1 >= nchunks) {
				/* Last chunk: leave it in progress in the
				 * current block, same as the fast path above.
				 */
				break;
			}

			/* This block is now full; queue it and start a fresh
			 * one for the next chunk.
			 */
			if (k_msgq_put(&pdmdata->buf_queue, &pdmdata->data_buffer, K_NO_WAIT) !=
			    0) {
				/* Queue full: drop oldest block to make room.
				 * Same silent-gap hazard as a slab-alloc miss
				 * (issue #2133 round 4b) -- the caller never
				 * sees the evicted block, so mark overrun. */
				void *oldest = NULL;

				pdmdata->overrun = true;
				if (k_msgq_get(&pdmdata->buf_queue, &oldest, K_NO_WAIT) == 0) {
					k_mem_slab_free(pdmdata->mem_slab, oldest);
					k_msgq_put(&pdmdata->buf_queue, &pdmdata->data_buffer,
						   K_NO_WAIT);
				}
			}

			pdmdata->data_buffer = get_slab(pdmdata);
			pdmdata->buf_index = 0;
			if (pdmdata->data_buffer == NULL) {
				/* Allocation failed mid-burst: drop the
				 * remainder of this burst, same ownership/
				 * error behavior as before (#1122).
				 */
				return;
			}
		}
	}
}

/* Init function */
/* Shared register bring-up for a genuine cold boot (DEVICE_DT_INST_DEFINE's
 * init callback, pdm_initialize() below) AND a PM_DEVICE_ACTION_RESUME
 * (issue #2133 round 4c) -- EXCEPT pdm_force_sleep(), which only runs when
 * `cold_init` is true. A resume must not wipe the hardware channel-enable/
 * clock-mode fields a prior configure()+trigger(START) already programmed
 * into PDM_CONFIG_REGISTER: pdm_initialize() used to always force-sleep,
 * so waking from suspend without the app re-calling configure() left the
 * MODE field restorable (pdata->clk_mode is a plain struct field with no
 * suspend handler of its own) but the CHANNEL-ENABLE bits permanently
 * cleared -- a post-resume dmic_trigger(START) would then clock the block
 * with zero channels actually enabled. Only a cold boot has no prior
 * session to preserve, so only it forces sleep. NOTE: as of divergence
 * (11), PM_DEVICE_ACTION_SUSPEND itself now resets pdata->clk_mode to
 * PDM_MODE_MICROPHONE_SLEEP (a fail-safe against the hardware registers
 * NOT surviving suspend) -- that is a SUSPEND-side reset, not something
 * this cold_init-only function does; clk_mode no longer survives a
 * suspend/resume cycle the way this comment originally assumed.
 */
static int pdm_hw_bringup(const struct device *dev, bool cold_init)
{
	const struct pdm_config *cfg = DEV_CFG(dev);
	struct pdm_data *pdata = DEV_DATA(dev);
	int32_t ret = 0;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	if (cfg->pcfg != NULL) {
		pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	}

	/* check device availability */
	if (!device_is_ready(cfg->clk_dev)) {
		LOG_ERR("clock controller device not ready");
		return -ENODEV;
	}

	/* Configure PDM clock sources.
	 * alp-sdk patch (Tier-2): the UPSTREAM alif clockctrl implements
	 * clock_control_configure() as a no-op returning -ENOSYS (unlike the fork
	 * clockctrl this driver was written against) -- the clock source field is
	 * carried in the clkid cell and applied by clock_control_on(), and the LPPDM
	 * source resets to the 76.8 MHz default (src=0) anyway. Tolerate -ENOSYS/
	 * -ENOTSUP so init does not abort (same fix as spi_dw_alif on this SoC). */
	ret = clock_control_configure(cfg->clk_dev, cfg->clkid, NULL);
	if (ret != 0 && ret != -ENOSYS && ret != -ENOTSUP) {
		LOG_ERR("Unable to configure clock: err:%d", ret);
		return ret;
	}

	/* Enable PDM clock from clock manager */
	ret = clock_control_on(cfg->clk_dev, cfg->clkid);
	if (ret != 0) {
		LOG_ERR("Unable to turn on clock: err:%d", ret);
		return ret;
	}

	if (cold_init) {
		/* Force the block to MICROPHONE_SLEEP with no channels
		 * enabled BEFORE anything else here touches
		 * PDM_CONFIG_REGISTER (issue #2133 round 4a) -- see
		 * pdm_force_sleep()'s comment for the silicon observation
		 * this fixes: a fresh image otherwise inherits whatever
		 * clock mode + channels the PREVIOUS image left running,
		 * surviving even a `loadbin` reset. Cold-boot only (round
		 * 4c) -- see this function's header comment for why RESUME
		 * must not do this.
		 */
		pdm_force_sleep(dev);
	}

	cfg->irq_config();

	k_msgq_init(&pdata->buf_queue, (char *)pdata->queue_data, sizeof(void *), MAX_QUEUE_LEN);

	/* Enable the Bypass IIR Filter */
	sys_write32(pdata->bypass_iir_filter << PDM_BYPASS_IIR, reg_base + PDM_CTL_REGISTER);

	sys_write32(cfg->fifo_watermark, reg_base + PDM_THRESHOLD_REGISTER);

	LOG_DBG("alif pdm driver init okay");

	return 0;
}

static int pdm_initialize(const struct device *dev)
{
	return pdm_hw_bringup(dev, true);
}

static const struct _dmic_ops dmic_alif_pdm_api = {
	.configure = dmic_alif_pdm_configure,
	.trigger = dmic_alif_pdm_trigger,
	.read = dmic_alif_pdm_read,
};

#if defined(CONFIG_PM_DEVICE)

/**
 * @brief PDM PM device action handler
 *
 * Handles power management state transitions for the PDM device.
 * Coordinates with power domain via PM framework.
 *
 * @param dev device struct
 * @param action PM device action
 *
 * @return 0 if successful, negative errno otherwise
 */
static int pdm_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		/* Device is powered - restore state. NOT pdm_initialize()
		 * (issue #2133 round 4c): that force-sleeps as a cold-boot
		 * step, which would wipe a prior configure()'s hardware
		 * channel-enable field on every resume -- see
		 * pdm_hw_bringup()'s header comment. */
		return pdm_hw_bringup(dev, false);

	case PM_DEVICE_ACTION_SUSPEND: {
		struct pdm_data *pdata = DEV_DATA(dev);

		/* Round 4e ordering fix: the round 4d version freed
		 * data_buffer and drained buf_queue while interrupts were
		 * STILL ENABLED and record_data was STILL 1 -- the ISR could
		 * run concurrently with this free/drain (a use-after-free on
		 * data_buffer, or a double-free racing the drain loop's own
		 * k_mem_slab_free()). DMIC_TRIGGER_STOP never had this bug:
		 * it disables interrupts and, ordering-wise, establishes
		 * "no longer recording" before it frees anything. Mirror
		 * that exact ordering here: interrupts off and record_data=0
		 * FIRST, so the ISR cannot touch data_buffer/buf_queue by
		 * the time this function frees/drains them. Latent today --
		 * CONFIG_PM_DEVICE is off on every board this driver ships
		 * on -- fixed anyway.
		 */
		disable_interrupt(dev);
		pdata->record_data = 0;

		/* Force the clock back to MICROPHONE_SLEEP (issue #2133
		 * round 2 divergence (4)) -- a suspend while still actively
		 * sampling (app skipped DMIC_TRIGGER_STOP) must not leave
		 * PDM_C0/PDM_C2 clocking the mics through a power-down.
		 */
		pdm_mode(dev, PDM_MODE_MICROPHONE_SLEEP);

		/* Round 4d fix: this used to leave record_data/overrun/the
		 * in-progress block/the queue exactly as an active session
		 * left them. Two bugs resulted: (1) record_data stayed 1
		 * across resume, so a post-resume DMIC_TRIGGER_START silently
		 * NOOPed (pdm_decide_start()) over a block nothing was
		 * sampling into anymore -- dead capture with no error; (2)
		 * pdm_hw_bringup()'s k_msgq_init() on resume re-initializes
		 * buf_queue, orphaning (leaking) whatever blocks were still
		 * queued at suspend time. Mirror DMIC_TRIGGER_STOP's cleanup
		 * so a suspend/resume cycle matches a STOP/START one from the
		 * app's side for the software state this function owns --
		 * this does NOT claim the HARDWARE registers behave the same
		 * way (issue #2133 round 4f: see the clk_mode reset below). */
		if (pdata->data_buffer != NULL) {
			k_mem_slab_free(pdata->mem_slab, pdata->data_buffer);
			pdata->data_buffer = NULL;
		}

		void *buf;

		while (k_msgq_get(&pdata->buf_queue, &buf, K_NO_WAIT) == 0) {
			k_mem_slab_free(pdata->mem_slab, buf);
		}

		pdata->overrun = false;

		/* issue #2133 round 4f: pdm_hw_bringup(dev, false) (resume)
		 * deliberately does NOT re-apply channel-enable/FIR/GAIN --
		 * see its header comment -- on the assumption those hardware
		 * registers survive the low-power transition, matching
		 * pdata->clk_mode surviving in software. If the power domain
		 * actually powers down, that assumption is false: the
		 * registers reset but clk_mode does not, so
		 * pdm_decide_start() would see "configured" and a post-resume
		 * DMIC_TRIGGER_START would proceed over dead register state
		 * instead of refusing. Reset clk_mode here too so START
		 * refuses as "not configured" regardless of whether the
		 * domain actually lost power -- fails safe either way, at the
		 * cost of requiring a fresh configure() after every resume
		 * even when retention did hold. */
		pdata->clk_mode = PDM_MODE_MICROPHONE_SLEEP;

		return 0;
	}

	case PM_DEVICE_ACTION_TURN_OFF:
	case PM_DEVICE_ACTION_TURN_ON:
		/* Power domain handling is automatic via PM framework */
		return 0;

	default:
		break;
	}

	return -ENOTSUP;
}
#endif /* CONFIG_PM_DEVICE */

/********** Device Definition per instance Macros **********/

#define PDM_INIT(n) \
	PINCTRL_DT_INST_DEFINE(n); \
	/* A DT overlay that sets BOTH clk-frequency-min and clk-frequency-max \
	 * with an INVERTED range (min > max) would make dmic_alif_pdm_configure() \
	 * reject every mode with an opaque per-attempt "below mic minimum"/ \
	 * "above mic maximum" message and never explain why -- fail loudly at \
	 * BUILD time instead (issue #2133 round 4a). Skipped when either \
	 * property is absent: an absent bound isn't a user-declared range to \
	 * invert (DT_INST_PROP_OR below still defaults it to 0/UINT32_MAX -- \
	 * issue #2133 round 4c: this comment used to also credit \
	 * pdm_force_sleep() for that default, which is wrong -- it only \
	 * clears hardware register bits, it has no role in the DT default \
	 * value). \
	 */ \
	BUILD_ASSERT(!(DT_INST_NODE_HAS_PROP(n, clk_frequency_min) && \
	               DT_INST_NODE_HAS_PROP(n, clk_frequency_max)) || \
	                 (DT_INST_PROP_OR(n, clk_frequency_min, 0) <= \
	                  DT_INST_PROP_OR(n, clk_frequency_max, UINT32_MAX)), \
	             "alif,alif-pdm: clk-frequency-min must not exceed " \
	             "clk-frequency-max (the DT range is inverted)"); \
	/* PDM_CH_GAIN's GAIN field is bits [11:0] (unsigned 8.4 fixed-point, \
	 * issue #2133 round 4e -- Alif SVD AE822FA0E5597BS0_CM55_HP_View.svd, \
	 * PDM_CH_GAIN register, GAIN field, lines ~19081-19092; matches the \
	 * Alif DFP's PDM_MAX_GAIN_CTRL 0xFFFU, drivers/include/pdm.h). The \
	 * bound is therefore the register's own hardware range: PDM_CH_GAIN_MAX \
	 * (0xFFF, 4095) is the largest value the 12-bit field can hold -- a \
	 * value written above it truncates to bits [11:0]; 0x1000 exactly \
	 * truncates to 0, which MUTES the channel, not "sets max gain" as a \
	 * name like PDM_MAX_GAIN_CTRL might suggest. The lower bound is 0x001: \
	 * 0x000 is 0.0x, a silently-muted channel indistinct from a dead one, \
	 * and never a plausible DT value. \
	 */ \
	BUILD_ASSERT(DT_INST_PROP_OR(n, channel_gain, 0x200) >= 0x001 && \
	                 DT_INST_PROP_OR(n, channel_gain, 0x200) <= PDM_CH_GAIN_MAX, \
	             "alif,alif-pdm: channel-gain out of PDM_CH_GAIN's 12-bit " \
	             "0x001..0xFFF range (0x1000 and above truncate the field and " \
	             "can mute the channel)"); \
	static void            pdm_irq_config_##n(void); \
	static struct pdm_data dmic_alif_pdm_data_##n = { \
		.bypass_iir_filter = DT_INST_PROP(n, bypass_iir_filter), \
	}; \
	static const struct pdm_config dmic_alif_pdm_cfg_##n = { \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)), \
		.fifo_watermark    = DT_INST_PROP(n, fifo_watermark), \
		.has_error_irq     = DT_INST_IRQ_HAS_NAME(n, error_intr), \
		.has_audio_det_irq = DT_INST_IRQ_HAS_NAME(n, audio_det_intr), \
		.irq_config        = pdm_irq_config_##n, \
		.pcfg              = PINCTRL_DT_INST_DEV_CONFIG_GET(n), \
		.clk_dev           = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)), \
		.clkid             = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, clkid), \
		.clk_frequency_min = DT_INST_PROP_OR(n, clk_frequency_min, 0), \
		.clk_frequency_max = DT_INST_PROP_OR(n, clk_frequency_max, UINT32_MAX), \
		.channel_gain      = DT_INST_PROP_OR(n, channel_gain, 0x200), \
	}; \
	static void pdm_irq_config_##n(void) \
	{ \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, warning_intr, irq), \
		            DT_INST_IRQ_BY_NAME(n, warning_intr, priority), \
		            alif_pdm_warning_isr, \
		            DEVICE_DT_INST_GET(n), \
		            0); \
		irq_enable(DT_INST_IRQ_BY_NAME(n, warning_intr, irq)); \
		IF_ENABLED(DT_INST_IRQ_HAS_NAME(n, error_intr), \
		           (IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, error_intr, irq), \
		                        DT_INST_IRQ_BY_NAME(n, error_intr, priority), \
		                        pdm_error_detect_irq_handler, \
		                        DEVICE_DT_INST_GET(n), \
		                        0); \
		            irq_enable(DT_INST_IRQ_BY_NAME(n, error_intr, irq));)) \
		IF_ENABLED(DT_INST_IRQ_HAS_NAME(n, audio_det_intr), \
		           (IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, audio_det_intr, irq), \
		                        DT_INST_IRQ_BY_NAME(n, audio_det_intr, priority), \
		                        pdm_audio_detect_irq_handler, \
		                        DEVICE_DT_INST_GET(n), \
		                        0); \
		            irq_enable(DT_INST_IRQ_BY_NAME(n, audio_det_intr, irq));)) \
	} \
	PM_DEVICE_DT_INST_DEFINE(n, pdm_pm_action); \
	DEVICE_DT_INST_DEFINE(n, \
	                      pdm_initialize, \
	                      PM_DEVICE_DT_INST_GET(n), \
	                      &dmic_alif_pdm_data_##n, \
	                      &dmic_alif_pdm_cfg_##n, \
	                      POST_KERNEL, \
	                      CONFIG_AUDIO_DMIC_INIT_PRIORITY, \
	                      &dmic_alif_pdm_api);

DT_INST_FOREACH_STATUS_OKAY(PDM_INIT)

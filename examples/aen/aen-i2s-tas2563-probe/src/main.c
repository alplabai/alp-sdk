/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i2s-tas2563-probe -- proves the I2S0 path through the REWORKED U46 mux
 * reaches both TAS2563 amps on the E1M-AEN801 (Alif Ensemble E8, M55-HE).
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
 * THE OBJECTIVE CHECK -- NEEDS NO EARS, AND DOES NOT TRUST THE SOFTWARE-
 * SHUTDOWN BASELINE. TAS2563's four latched-interrupt registers
 * (INT_LTCH0..4, read as one packed word by tas2563_read_faults(),
 * include/alp/chips/tas2563.h) carry @ref TAS2563_FAULT_TDM_CLOCK (0x24[2],
 * "TDM clock error", SLASET3D SS7.5.36 Table 7-136): the amp's own hardware
 * report of whether it currently sees a valid TDM/I2S clock on its RX pins.
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
 *   DURING  -- both amps ACTIVE, the tone genuinely playing: clear the latch,
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
 * ONLY as supplementary printed context, per the coordinator's safety
 * review -- see faults_before below, which no longer drives the verdict.
 *
 * TAS2563_FAULT_ASI2_CLOCK (0x27[3], a SECOND serial-interface clock-error
 * bit) is NOT used here -- it names ASI2, a different serial port from the
 * TDM/I2S receiver this board wires, so it says nothing about this signal
 * path. The "DSP Mode & TDM_DET" register (0x11, SLASET3D SS7.5.19,
 * FS_RATIO/FS_RATE clock-detect readback) is NOT exposed by this driver --
 * no macro exists for it in include/alp/chips/tas2563.h -- so it is TBD, not
 * used, and not invented here.
 *
 * HARDWARE SAFETY -- both TAS2563s can drive ~10 W peak into 4 ohm
 * (SLASET3D Table 7-105); tas2563.h's own @warning calls the power-on
 * AMP_LEVEL "near the top of the part's range". This app never exceeds:
 *   - TAS2563_AMP_LEVEL_MIN (8.5 dBV, the quietest listed analog gain code) --
 *     set on BOTH amps over I2C while they are still in software shutdown,
 *     and CONFIRMED by a register readback (tas2563_read_amp_level(), added
 *     to the driver for exactly this) before either amp is ever told
 *     ACTIVE. A write whose readback does not come back MIN keeps BOTH amps
 *     in software shutdown for the rest of this run, full stop -- see
 *     level_confirmed[]/i2s_confirmed[] below. tas2563_set_amp_level() is
 *     itself a read-modify-write against a register shared with other
 *     fields (DIS_DC_BLOCKER, reserved bits), so its own ALP_OK return is
 *     not, by itself, proof the level this app asked for is the level now
 *     on the part -- the readback is what actually proves it.
 *   - A FLAT digital volume of SOUND_VOL (4/255, ~1.6% of unity) via
 *     alp_audio_out_set_volume() -- opened and STARTED before ACTIVE, so
 *     the amp comes out of shutdown already receiving a near-silent stream,
 *     and never raised for the rest of this run (this is a diagnostic
 *     probe, not a demo -- there is no reason to ever go louder than the
 *     level that already answers the clock-detect question).
 *   - A short tone (~128 ms before the DURING read, then I2S3 is
 *     deliberately stopped for the STOPPED read -- see above), and teardown
 *     always mutes (tas2563_set_mode(SHUTDOWN)) BOTH amps before anything
 *     else tears down. This is true on EVERY exit path, not just the
 *     bottom of main(): every early return past the point AMP_ENABLE is
 *     first released (`--- 2.` below) drives AMP_ENABLE back to 0, mutes
 *     any amp that ever answered I2C, deinitialises it, releases the mux
 *     `/E`, and -- once the I2C bus is open -- closes it.
 *   Both levers mirror examples/aen/aen-evk-demo's phase 11, which this app
 *   was mined from -- see that phase's file header for the fuller "why two
 *   independent levers" reasoning.
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
 * `alp_i2s_open()` TX slab and this app's own `static int16_t tone_buf[]`
 * both live in whatever RAM the linker puts BSS/heap in (this board's
 * default -- ITCM/DTCM per the bench RAM-run `chosen` retarget), and that is
 * fine precisely because nothing but the CPU ever reads them.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/kernel.h>

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

#define SOUND_SAMPLE_RATE_HZ            16000u
#define SOUND_FRAMES_PER_BLOCK          256u
#define SOUND_TONE_HZ                   1000u
#define SOUND_TONE_AMPLITUDE            20000 /* int16, headroom below INT16_MAX. */
#define SOUND_TONE_BLOCKS_BEFORE_ACTIVE 2u    /* 2*16ms=32ms: quiet stream running before ACTIVE. */
#define SOUND_TONE_BLOCKS_AFTER_ACTIVE  6u /* 6*16ms=96ms: genuinely playing at the DURING read. */
#define SOUND_VOL                       4u /* ~1.6% of unity (255) -- see file header SAFETY. */

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

/* The named outcomes this probe can report -- see the file header's
 * "THE OBJECTIVE CHECK" section for what DURING/STOPPED mean.
 *
 * AMP_CONTROL_GPIO_FAILED is split out from TAS2563_NOT_RESPONDING (an
 * earlier version folded gpio5/AMP_ENABLE/AMP_FAULT failures into the I2C
 * bucket): a gpio5 device or pinctrl fault is a DIFFERENT layer than an I2C
 * NACK, and printing "TAS2563 I2C not responding" for a GPIO fault the app
 * never got far enough to even attempt I2C on would misname the cause.
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
 * header's "THE OBJECTIVE CHECK" section and the matrix in main(). */
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
 * Returns false on any write failure -- the caller latches that into its
 * own write_failed and stops calling this. */
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
	 * run never opens I2S3 or sets ANY amp ACTIVE at all: with only one
	 * amp (or neither) trustworthy, the DURING/STOPPED comparison below
	 * cannot produce a meaningful two-amp verdict either, so there is
	 * nothing this run can still safely learn by proceeding. */
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

	/* --- 6. Supplementary-only baseline: still in software shutdown ----- */
	/* Printed for context; per the file header, this does NOT drive the
	 * verdict -- whether the TDM-clock latch is even live in software
	 * shutdown is undocumented, so a CLEAR read here proves nothing either
	 * way. The DURING/STOPPED pair below (both inside the ACTIVE window)
	 * is what decides the outcome. */
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

	/* --- 7. I2S3 TX: open at the quiet volume, THEN start ---------------- */
	alp_audio_out_t *spk = alp_audio_out_open(&(alp_audio_config_t){
	    .peripheral_id    = 0,
	    .sample_rate_hz   = SOUND_SAMPLE_RATE_HZ,
	    .channels         = 2,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = SOUND_FRAMES_PER_BLOCK,
	});
	alp_status_t     spk_rc =
	    (spk != NULL) ? alp_audio_out_set_volume(spk, SOUND_VOL) : alp_last_error();
	if (spk_rc == ALP_OK) spk_rc = alp_audio_out_start(spk);
	printf("[probe] alp_audio_out_open+set_volume(%u)+start(I2S3) -> %d\n", SOUND_VOL, (int)spk_rc);

	static int16_t tone_buf[SOUND_FRAMES_PER_BLOCK * 2u]; /* static: off main()'s stack. */
	uint32_t       phase_acc         = 0;
	const uint32_t samples_per_cycle = SOUND_SAMPLE_RATE_HZ / SOUND_TONE_HZ;
	bool           write_failed      = false;

	/* --- 8. First few blocks -- a quiet stream is already running BEFORE
	 * either amp goes ACTIVE (lever 2, digital volume, already engaged). - */
	for (unsigned b = 0; b < SOUND_TONE_BLOCKS_BEFORE_ACTIVE && spk_rc == ALP_OK && !write_failed;
	     b++) {
		if (!write_one_tone_block(spk, tone_buf, &phase_acc, samples_per_cycle)) {
			write_failed = true;
		}
	}

	/* --- 9. ONLY NOW: both (confirmed) amps ACTIVE -- the stream is
	 * already quiet and running, matching examples/aen/aen-evk-demo's
	 * phase 11 ordering. Both amps ARE confirmed (step 5's hard gate), so
	 * no per-amp skip is needed here. ------------------------------------ */
	bool active_set = false;
	if (spk_rc == ALP_OK && !write_failed) {
		active_set = true;
		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
			if (arc != ALP_OK) active_set = false;
			printf("[probe] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
		}
	}

	/* --- 10. More tone blocks WHILE ACTIVE, so the DURING read below
	 * lands while the tone is genuinely, freshly playing -- not just
	 * relying on the controller free-running after the last write. ------ */
	for (unsigned b = 0; b < SOUND_TONE_BLOCKS_AFTER_ACTIVE && active_set && !write_failed; b++) {
		if (!write_one_tone_block(spk, tone_buf, &phase_acc, samples_per_cycle)) {
			write_failed = true;
		}
	}

	/* --- 11. OBJECTIVE CHECK, DURING: amps ACTIVE, tone genuinely playing */
	uint32_t faults_during[AMP_COUNT] = { 0 };
	bool     during_valid             = active_set && spk_rc == ALP_OK && !write_failed;
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

	/* --- 12. STOP-CONTROL: stop I2S3 while BOTH amps stay ACTIVE --------- */
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

	/* --- 13. OBJECTIVE CHECK, STOPPED: amps still ACTIVE, I2S3 halted ---- */
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

	/* --- 14. Teardown, mute FIRST -- confirmed SHUTDOWN before anything
	 * else stops, on this (the only remaining) exit path. ----------------- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
	}
	if (spk != NULL) {
		if (!stream_stop_attempted) alp_audio_out_stop(spk); /* not yet stopped above. */
		alp_audio_out_close(spk);
	}
	for (size_t i = 0; i < AMP_COUNT; i++) {
		tas2563_deinit(&amps[i]);
	}
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	alp_i2c_close(bus);

	/* --- 15. Verdict, per amp then combined ------------------------------ */
	if (!during_valid) {
		printf("[probe] I2S3/ACTIVE never reached a clean playing state (open/set_volume/"
		       "start/write/set_mode rc above) -- cannot run the DURING/STOPPED check\n");
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_CLOCKS_NOT_REACHING_AMP));
		printf("[probe] done\n");
		return 0;
	}
	if (!stopped_valid) {
		printf("[probe] could not stop the I2S3 stream for the stop-control read (rc=%d)\n",
		       (int)stop_rc);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_INCONCLUSIVE));
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
	printf("[probe] VERDICT: %s\n", verdict_str(v));
	printf("[probe] done\n");
	return 0;
}

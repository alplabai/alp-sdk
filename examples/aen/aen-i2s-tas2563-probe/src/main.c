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
 * THE OBJECTIVE CHECK -- NEEDS NO EARS. TAS2563's four latched-interrupt
 * registers (INT_LTCH0..4, read as one packed word by tas2563_read_faults(),
 * include/alp/chips/tas2563.h) carry @ref TAS2563_FAULT_TDM_CLOCK (0x24[2],
 * "TDM clock error", SLASET3D SS7.5.36 Table 7-136): the amp's own hardware
 * report of whether it currently sees a valid TDM/I2S clock on its RX pins.
 * This app clears + reads that bit TWICE on both amps:
 *   BASELINE, before alp_audio_out_open()/start() ever runs (no I2S3 traffic
 *   of any kind has happened yet) -- expected: the bit re-latches SET, since
 *   nothing is clocking the amp's SDIN/SCLK/WS pins at all.
 *   DURING, after the tone has been playing for several blocks -- if the
 *   bit reads CLEAR here, that is the evidence: SCLK/WS genuinely reached
 *   the amp through the reworked U46, not just that this app's own I2C
 *   writes succeeded.
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
 *     before either is ever told ACTIVE.
 *   - A FLAT digital volume of SOUND_VOL (4/255, ~1.6% of unity) via
 *     alp_audio_out_set_volume() -- opened and STARTED before ACTIVE, so
 *     the amp comes out of shutdown already receiving a near-silent stream,
 *     and never raised for the rest of this run (this is a diagnostic
 *     probe, not a demo -- there is no reason to ever go louder than the
 *     level that already answers the clock-detect question).
 *   - A short tone: SOUND_TONE_BLOCKS_TOTAL * (SOUND_FRAMES_PER_BLOCK /
 *     SOUND_SAMPLE_RATE_HZ) = 16 * 16 ms = 256 ms total, teardown always
 *     mutes (tas2563_set_mode(SHUTDOWN)) before anything else stops.
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

#define SOUND_SAMPLE_RATE_HZ         16000u
#define SOUND_FRAMES_PER_BLOCK       256u
#define SOUND_TONE_HZ                1000u
#define SOUND_TONE_AMPLITUDE         20000 /* int16, headroom below INT16_MAX. */
#define SOUND_TONE_BLOCKS_BEFORE_MID 8u    /* 8*16ms=128ms before the mid-stream clock check. */
#define SOUND_TONE_BLOCKS_AFTER_MID  8u    /* 8*16ms=128ms after it. */
#define SOUND_VOL                    4u    /* ~1.6% of unity (255) -- see file header SAFETY. */

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

/* The four named outcomes this probe can report -- see the file header's
 * "THE OBJECTIVE CHECK" section for what each layer means. AMP_ENABLE (the
 * SD_N hardware-shutdown release on gpio5) is folded into TAS2563_NOT_
 * RESPONDING rather than getting a fifth bucket: if SD_N cannot be driven,
 * both amps stay in hardware shutdown and tas2563_init() will not get an
 * ACK either, so the two failures are indistinguishable from this app's
 * evidence and the task only asks for four named layers. */
typedef enum {
	VERDICT_BRIDGE_MUX_FAILED,
	VERDICT_TAS2563_NOT_RESPONDING,
	VERDICT_CLOCKS_NOT_REACHING_AMP,
	VERDICT_CLOCKS_REACH_AMP,
} verdict_t;

static const char *verdict_str(verdict_t v)
{
	switch (v) {
	case VERDICT_BRIDGE_MUX_FAILED:
		return "bridge/mux-enable failed";
	case VERDICT_TAS2563_NOT_RESPONDING:
		return "TAS2563 I2C not responding";
	case VERDICT_CLOCKS_NOT_REACHING_AMP:
		return "I2S clocks not reaching the amp";
	case VERDICT_CLOCKS_REACH_AMP:
		return "clocks reach the amp";
	default:
		return "unknown";
	}
}

/* mux_sel/mux_en/gpio5 teardown shared by every exit path.  alp_gpio_close()
 * on NULL is a documented no-op, so this is safe to call from a path that
 * never got as far as opening either handle. */
static void mux_disable(alp_gpio_t *mux_sel, alp_gpio_t *mux_en)
{
	if (mux_en != NULL) {
		(void)alp_gpio_write(mux_en, true); /* /E high = mux disabled/Hi-Z. */
	}
	alp_gpio_close(mux_sel);
	alp_gpio_close(mux_en);
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
		mux_disable(mux_sel, mux_en);
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
		mux_disable(mux_sel, mux_en);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_BRIDGE_MUX_FAILED));
		return 0;
	}
	k_msleep(MUX_SETTLE_MS);

	/* --- 2. AMP_ENABLE (SD_N) hardware reset + AMP_FAULT as input -------- */
	const struct device *gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	if (!device_is_ready(gpio5)) {
		printf("[probe] gpio5 not ready -- AMP_ENABLE/AMP_FAULT (P5_2/P5_0) unreachable\n");
		mux_disable(mux_sel, mux_en);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_TAS2563_NOT_RESPONDING));
		return 0;
	}
	int grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(gpio5, AMP_ENABLE_PIN, 1);
	printf("[probe] AMP_ENABLE (SD_N, P5_2) hardware reset + release -> %d\n", grc);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	if (grc != 0) {
		printf("[probe] AMP_ENABLE/AMP_FAULT not drivable -- neither amp can leave "
		       "hardware shutdown\n");
		mux_disable(mux_sel, mux_en);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_TAS2563_NOT_RESPONDING));
		return 0;
	}
	k_usleep(TAS2563_RESET_SETTLE_US); /* SDZ just went high -- settle before any I2C. */

	/* --- 3. Carrier I2C bus + tas2563_init() on both amps ---------------- */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	tas2563_t  amps[AMP_COUNT];
	bool       amp_up[AMP_COUNT] = { false, false };
	int        ok_amps           = 0;
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
		(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
		mux_disable(mux_sel, mux_en);
		printf("[probe] VERDICT: %s\n", verdict_str(VERDICT_TAS2563_NOT_RESPONDING));
		return 0;
	}

	/* --- 4. Safety lever 1: quietest analog level, BEFORE anything else -- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t lrc = tas2563_set_amp_level(&amps[i], TAS2563_AMP_LEVEL_MIN);
		printf("[probe] tas2563_set_amp_level(0x%02x, MIN=8.5dBV) -> %d\n", amp_addrs[i], (int)lrc);
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
	for (size_t i = 0; i < AMP_COUNT; i++) {
		alp_status_t crc = tas2563_configure_i2s(&amps[i], &amp_i2s_cfg, amp_rx_channel[i]);
		printf("[probe] tas2563_configure_i2s(0x%02x) -> %d\n", amp_addrs[i], (int)crc);
	}

	/* --- 6. OBJECTIVE CHECK, baseline: no I2S3 traffic has happened yet -- */
	uint32_t faults_before[AMP_COUNT] = { 0 };
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_clear_faults(&amps[i]);
	}
	k_msleep(CLOCK_CHECK_SETTLE_MS);
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_read_faults(&amps[i], &faults_before[i]);
		printf("[probe] tas2563_read_faults(0x%02x) BASELINE -> 0x%08x (TDM_CLOCK %s)\n",
		       amp_addrs[i],
		       faults_before[i],
		       (faults_before[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET (no clock, expected)" : "clear");
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

	/* --- 8. First half of the tone -- a quiet stream is already running -- */
	for (unsigned b = 0; b < SOUND_TONE_BLOCKS_BEFORE_MID && spk_rc == ALP_OK; b++) {
		for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
			int16_t sample       = ((phase_acc % samples_per_cycle) < samples_per_cycle / 2u)
			                           ? SOUND_TONE_AMPLITUDE
			                           : -SOUND_TONE_AMPLITUDE;
			tone_buf[2u * f]     = sample;
			tone_buf[2u * f + 1] = sample;
			phase_acc++;
		}
		if (alp_audio_out_write(spk, tone_buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u) != ALP_OK) {
			write_failed = true;
			break;
		}
	}

	/* --- 9. ONLY NOW: both amps ACTIVE -- the stream is already quiet and
	 * running, matching examples/aen/aen-evk-demo's phase 11 ordering. ---- */
	if (spk_rc == ALP_OK && !write_failed) {
		for (size_t i = 0; i < AMP_COUNT; i++) {
			alp_status_t arc = tas2563_set_mode(&amps[i], TAS2563_MODE_ACTIVE);
			printf("[probe] tas2563_set_mode(0x%02x, ACTIVE) -> %d\n", amp_addrs[i], (int)arc);
		}
	}

	/* --- 10. Second half of the tone, THEN the mid-stream clock check ---- */
	for (unsigned b = 0; b < SOUND_TONE_BLOCKS_AFTER_MID && spk_rc == ALP_OK && !write_failed;
	     b++) {
		for (uint32_t f = 0; f < SOUND_FRAMES_PER_BLOCK; f++) {
			int16_t sample       = ((phase_acc % samples_per_cycle) < samples_per_cycle / 2u)
			                           ? SOUND_TONE_AMPLITUDE
			                           : -SOUND_TONE_AMPLITUDE;
			tone_buf[2u * f]     = sample;
			tone_buf[2u * f + 1] = sample;
			phase_acc++;
		}
		if (alp_audio_out_write(spk, tone_buf, SOUND_FRAMES_PER_BLOCK, NULL, 200u) != ALP_OK) {
			write_failed = true;
			break;
		}
	}

	/* --- 11. OBJECTIVE CHECK, during: ~128-256ms of real I2S3 traffic ---- */
	uint32_t faults_during[AMP_COUNT] = { 0 };
	bool     playing                  = (spk_rc == ALP_OK) && !write_failed;
	if (playing) {
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_clear_faults(&amps[i]);
		}
		k_msleep(CLOCK_CHECK_SETTLE_MS);
		for (size_t i = 0; i < AMP_COUNT; i++) {
			(void)tas2563_read_faults(&amps[i], &faults_during[i]);
			printf("[probe] tas2563_read_faults(0x%02x) DURING -> 0x%08x (TDM_CLOCK %s)\n",
			       amp_addrs[i],
			       faults_during[i],
			       (faults_during[i] & TAS2563_FAULT_TDM_CLOCK) ? "SET (still no clock)"
			                                                    : "clear (clock present)");
		}
	}

	/* --- 12. Teardown, mute BEFORE anything else stops (unconditional) --- */
	for (size_t i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_set_mode(&amps[i], TAS2563_MODE_SHUTDOWN);
	}
	if (spk != NULL) {
		alp_audio_out_stop(spk);
		alp_audio_out_close(spk);
	}
	for (size_t i = 0; i < AMP_COUNT; i++) {
		tas2563_deinit(&amps[i]);
	}
	(void)gpio_pin_set(gpio5, AMP_ENABLE_PIN, 0);
	mux_disable(mux_sel, mux_en);
	if (bus != NULL) alp_i2c_close(bus);

	/* --- 13. Verdict ------------------------------------------------------ */
	verdict_t v;
	if (!playing) {
		printf("[probe] I2S3 never started cleanly (open/set_volume/start/write rc "
		       "above) -- cannot evaluate the clock-detect bit\n");
		v = VERDICT_CLOCKS_NOT_REACHING_AMP;
	} else {
		bool clock_ok = true;
		for (size_t i = 0; i < AMP_COUNT; i++) {
			if ((faults_during[i] & TAS2563_FAULT_TDM_CLOCK) != 0u) clock_ok = false;
		}
		v = clock_ok ? VERDICT_CLOCKS_REACH_AMP : VERDICT_CLOCKS_NOT_REACHING_AMP;
	}
	printf("[probe] VERDICT: %s\n", verdict_str(v));
	printf("[probe] done\n");
	return 0;
}

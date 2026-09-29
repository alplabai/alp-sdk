/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * sound/src/main.c -- Trace Runner game sound on the E1M-EVK's two TAS2563
 * amps: src/audio's synth -> I2S3 (TR_AUDIO_RATE stereo S16) -> U27 (0x4D, left,
 * J14) + U28 (0x4E, right, J21).
 *
 * Bring-up is the sequence that made the amps audible on the reworked EVK
 * (E1M-AEN803 serial 2026W36-0002, 2026-09-15, aen-i2s-tas2563-probe) in the
 * order alp-sdk's aen-evk-demo phase 11 settled on:
 *   1 CC3501E bridge up (the mux controls are CC3501E GPIOs)
 *   2 I2S0 mux S = 0 (amps) -- S is NEVER driven 1: on the reworked carrier
 *     S=1 routes the M.2 card's 3.3 V I2S onto a 1.8 V pad
 *   3 I2S0 mux /E = 0 (enabled)
 *   4 SD_N (P5_2) low 24 ms then high: hardware reset of both amps
 *   5 carrier I2C bus 0 open
 *   6 tas2563_init() on 0x4D and 0x4E
 *   7 AMP_LEVEL MIN + configure_i2s (2 x 32-bit slots, U27 LEFT, U28 RIGHT)
 *   8 (TEST) PDM mics open + start, 48 kHz stereo
 *   9 I2S3 open, digital volume TR_SND_VOLUME, start
 *  10 one silent block: the bit clock actually runs before any amp wakes
 *  11 tas2563_resume() on both amps
 * then the stream never stops (an idle bit clock lets the TAS2563 shut itself
 * down within ~1 s, alp-sdk #2146): silence is written as zero blocks.
 *
 * TR_SND_TEST=0 (GAME, the M55-HP firmware): drain the HE's event ring
 * (src/ipc/tr_aring.h) into tr_audio_event() before every block. Status for
 * the bench lives in the ring's HP block (SRAM0, readable from any AP):
 * hp_state, hp_heartbeat (+1 per block), hp_underruns, hp_fault_step.
 *
 * TR_SND_TEST=1: the standalone 2026W36-0002 test -- see run_test().
 *
 * The DesignWare I2S driver (i2s_dw.c) is FIFO/interrupt driven, not DMA:
 * the "double buffer" is the audio_out slab, which queues the block being
 * clocked out while the next one renders here.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include "alp/audio.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/tas2563.h"
#include "alp/i2s.h"
#include "alp/peripheral.h"

#include "audio/tr_audio.h"
#include "cc3501e_bridge.h"
#include "snd_verdict.h"
#include "ipc/tr_aring.h"

/* Digital volume, alp_audio_out_set_volume() 0..255 (255 = unity). 128 is the
 * level heard "louder and clean" on the 2026W36-0002 speakers (2026-09-15, amp
 * analog level MIN); the speakers' power rating is unknown, so it is also
 * the cap. */
#ifndef TR_SND_VOLUME
#define TR_SND_VOLUME 128u
#endif
BUILD_ASSERT(TR_SND_VOLUME <= 128u, "above the bench-heard level: ask before raising (speaker rating unknown)");

#define RATE      TR_AUDIO_RATE
#define BLOCK     (RATE * 16u / 1000u) /* frames per 16 ms block: 256 at 16 kHz, 768 at 48 kHz */
#define WRITE_TMO 100u

#define AMP_ENABLE_PIN           2   /* P5_2 = SD_N, active high (R138 pull-up to +VIO) */
#define AMP_FAULT_PIN            0   /* P5_0 = IRQZ, open drain, active low */
#define AMP_FAULT_PAD_REN        (1U << 16)
#define AMP_ENABLE_RESET_HOLD_MS 24u /* >= 23.8 ms max SDZ_TIMEOUT (SLASET3D Table 7-7) */
#define MUX_SETTLE_MS            10u
/* Step 10's bit-clock proof: 8 silent 16 ms blocks. The audio_out slab
 * buffers a couple, so the writes return after ~5-6 blocks of real time
 * (80-96 ms); a dead clock times the third write out instead. */
#define CLOCK_CHECK_BLOCKS 8u
#define CLOCK_CHECK_MIN_MS 60u
#define CLOCK_CHECK_MAX_MS 250u
#define AMP_COUNT                2u

static const pinctrl_soc_pin_t amp_enable_mux[] = { PIN_P5_2__GPIO };
static const pinctrl_soc_pin_t amp_fault_mux[]  = { PIN_P5_0__GPIO | AMP_FAULT_PAD_REN };
static const uint8_t amp_addrs[AMP_COUNT]      = { TAS2563_I2C_ADDR_GND_PULL, TAS2563_I2C_ADDR_VDD_PULL };
/* explicit LEFT/RIGHT, not FROM_ADDR: the DW I2S3 frame is 2 x 32-bit slots */
static const tas2563_rx_channel_t amp_rx[AMP_COUNT] = { TAS2563_RX_LEFT, TAS2563_RX_RIGHT };

static struct {
	cc3501e_t            fw;
	alp_gpio_t          *mux_sel, *mux_en;
	const struct device *gpio5;
	alp_i2c_t           *bus;
	tas2563_t            amps[AMP_COUNT];
	alp_audio_out_t     *spk;
	alp_audio_in_t      *mic;
} s;

static uint32_t s_amp_overrides __maybe_unused; /* TEST reports it in the capture header */
#ifdef TR_SND_AMP_REGS_FILE
#include TR_SND_AMP_REGS_FILE /* defines TR_SND_AMP_REGS: tas2563_tuning_reg_t initializers */
static const tas2563_tuning_reg_t k_amp_regs[] = TR_SND_AMP_REGS;
#endif

static int16_t s_mono[BLOCK];
static int16_t s_stereo[BLOCK * 2u];

/* Write one mono block to both channels. */
static alp_status_t out_block(const int16_t *mono)
{
	for (unsigned i = 0; i < BLOCK; i++) {
		s_stereo[2u * i]      = mono[i]; /* L -> U27 */
		s_stereo[2u * i + 1u] = mono[i]; /* R -> U28 */
	}
	return alp_audio_out_write(s.spk, s_stereo, BLOCK, NULL, WRITE_TMO);
}

/* Steps 1..11 above; returns 0, or the failing step number. */
static int bringup(bool with_mic)
{
	alp_status_t rc = cc3501e_bridge_bringup(&s.fw);
	printk("[snd] 1 cc3501e_bridge_bringup -> %d\n", (int)rc);
	if (rc != ALP_OK) return 1;

	s.mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	s.mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
	if (s.mux_sel == NULL || s.mux_en == NULL) {
		printk("[snd] mux open failed (IO8 needs a CRC-valid manifest with hw_rev %s) err=%d\n",
		       CONFIG_ALP_SDK_SOM_HW_REV, (int)alp_last_error());
		return 2;
	}
	rc = alp_gpio_configure(s.mux_sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (rc == ALP_OK) rc = alp_gpio_write(s.mux_sel, false);
	printk("[snd] 2 I2S_SELECT = 0 (amps) -> %d\n", (int)rc);
	if (rc != ALP_OK) return 2;
	rc = alp_gpio_configure(s.mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (rc == ALP_OK) rc = alp_gpio_write(s.mux_en, false);
	printk("[snd] 3 I2S_EN = 0 (enabled) -> %d\n", (int)rc);
	if (rc != ALP_OK) return 3;
	k_msleep(MUX_SETTLE_MS);

	s.gpio5 = DEVICE_DT_GET(DT_NODELABEL(gpio5));
	int grc = device_is_ready(s.gpio5) ? 0 : -ENODEV;
	if (grc == 0) grc = pinctrl_configure_pins(amp_enable_mux, ARRAY_SIZE(amp_enable_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(s.gpio5, AMP_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	if (grc == 0) k_msleep(AMP_ENABLE_RESET_HOLD_MS);
	if (grc == 0) grc = gpio_pin_set(s.gpio5, AMP_ENABLE_PIN, 1);
	if (grc == 0) grc = pinctrl_configure_pins(amp_fault_mux, ARRAY_SIZE(amp_fault_mux), 0U);
	if (grc == 0) grc = gpio_pin_configure(s.gpio5, AMP_FAULT_PIN, GPIO_INPUT);
	printk("[snd] 4 SD_N reset + release -> %d\n", grc);
	if (grc != 0) return 4;
	k_usleep(TAS2563_RESET_SETTLE_US);

	s.bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = EVK_I2C_BUS_SENSORS, .bitrate_hz = 100000u });
	printk("[snd] 5 alp_i2c_open(bus %d) -> %s\n", (int)EVK_I2C_BUS_SENSORS, s.bus ? "ok" : "NULL");
	if (s.bus == NULL) return 5;

	for (unsigned i = 0; i < AMP_COUNT; i++) {
		rc = tas2563_init(&s.amps[i], s.bus, amp_addrs[i], NULL);
		printk("[snd] 6 tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 6;
	}
	const alp_i2s_config_t i2s = {
		.bus_id = 0, .direction = ALP_I2S_DIR_TX, .sample_rate_hz = RATE, .channels = 2,
		.word_bits = 16, .format = ALP_I2S_FMT_I2S, .block_frames = BLOCK,
	};
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		rc = tas2563_set_amp_level(&s.amps[i], TAS2563_AMP_LEVEL_MIN);
		if (rc == ALP_OK) rc = tas2563_configure_i2s(&s.amps[i], &i2s, amp_rx[i]);
		printk("[snd] 7 amp 0x%02x level MIN + configure_i2s -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 7;
#ifdef TR_SND_AMP_REGS
		/* Bench A/B: extra register writes from -DTR_SND_AMP_REGS_FILE
		 * (sound/amp_regs_example.h), after the driver's own config. */
		size_t bad = 0;
		rc         = tas2563_load_tuning(&s.amps[i], k_amp_regs, ARRAY_SIZE(k_amp_regs), &bad);
		printk("[snd] 7b amp 0x%02x %u override writes -> %d (record %u)\n", amp_addrs[i],
		       (unsigned)ARRAY_SIZE(k_amp_regs), (int)rc, (unsigned)bad);
		if (rc != ALP_OK) return 7;
		s_amp_overrides = ARRAY_SIZE(k_amp_regs);
#endif
	}

	if (with_mic) {
		/* Opened here, STARTED only right before the first read: the
		 * backend's slab holds 4 blocks (64 ms) and an overrun is sticky. */
		s.mic = alp_audio_in_open(&(alp_audio_config_t){ .peripheral_id = 0, .sample_rate_hz = 48000u,
		                                                 .channels = 2, .format = ALP_AUDIO_FMT_S16_LE,
		                                                 .frames_per_block = 768u });
		rc = s.mic ? ALP_OK : alp_last_error();
		printk("[snd] 8 PDM open (48 kHz stereo) -> %d\n", (int)rc);
		if (rc != ALP_OK) return 8;
	}

	s.spk = alp_audio_out_open(&(alp_audio_config_t){ .peripheral_id = 0, .sample_rate_hz = RATE, .channels = 2,
	                                                  .format = ALP_AUDIO_FMT_S16_LE, .frames_per_block = BLOCK });
	rc = s.spk ? alp_audio_out_set_volume(s.spk, TR_SND_VOLUME) : alp_last_error();
	if (rc == ALP_OK) rc = alp_audio_out_start(s.spk);
	printk("[snd] 9 I2S3 open + volume %u + start -> %d\n", TR_SND_VOLUME, (int)rc);
	if (rc != ALP_OK) return 9;

	/* A queued write does NOT prove the bit clock runs: the first bench
	 * run (2026-09-23) queued its priming block fine with no 76.8 MHz audio
	 * clock at all, then every later write timed out. So stream several
	 * silent blocks and require the driver to consume them at about real
	 * time before any amp wakes. */
	memset(s_mono, 0, sizeof(s_mono));
	uint32_t t_clk = k_uptime_get_32();
	rc = ALP_OK;
	for (unsigned b = 0; b < CLOCK_CHECK_BLOCKS && rc == ALP_OK; b++) {
		rc = out_block(s_mono);
	}
	uint32_t clk_ms = k_uptime_get_32() - t_clk;
	printk("[snd] 10 %u silent blocks -> %d in %u ms (expect %u..%u)\n", CLOCK_CHECK_BLOCKS, (int)rc,
	       (unsigned)clk_ms, CLOCK_CHECK_MIN_MS, CLOCK_CHECK_MAX_MS);
	if (rc != ALP_OK || clk_ms < CLOCK_CHECK_MIN_MS || clk_ms > CLOCK_CHECK_MAX_MS) {
		printk("[snd] I2S3 is not consuming blocks at %u Hz: no bit clock. Is ZEPHYR_BASE's "
		       "clock_control_alif.c patched (alp-sdk zephyr/patches/zephyr/0001)?\n",
		       RATE);
		return 10; /* never wake the amps without a running clock */
	}

	for (unsigned i = 0; i < AMP_COUNT; i++) {
		rc = tas2563_resume(&s.amps[i]);
		printk("[snd] 11 tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 11;
	}
	return 0;
}

/* Mute first, then stop the clock, then release SD_N (input: R138 pulls it
 * up -- driving it low would hold both amps in hardware shutdown after exit
 * so they vanish from I2C, alp-sdk #2164), then disable the mux. S stays 0. */
static void teardown(void)
{
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		(void)tas2563_set_mode(&s.amps[i], TAS2563_MODE_SHUTDOWN);
	}
	if (s.spk) {
		(void)alp_audio_out_stop(s.spk);
		alp_audio_out_close(s.spk);
	}
	if (s.mic) {
		(void)alp_audio_in_stop(s.mic);
		alp_audio_in_close(s.mic);
	}
	if (s.gpio5 && device_is_ready(s.gpio5)) {
		(void)gpio_pin_configure(s.gpio5, AMP_ENABLE_PIN, GPIO_INPUT);
	}
	if (s.mux_en) {
		(void)alp_gpio_write(s.mux_en, true); /* /E high = mux off */
	}
}

#if TR_SND_TEST
/* ---- TEST: per-effect loopback ----------------------------------------------
 *
 * Plays sound/src/snd_script.h window by window (silence, three reference
 * tones, music, then every effect alone) while mic U19 (PDM ch0) records all
 * of it to TR_SND_CAPTURE_ADDR, mono S16 at 48 kHz. After each window both
 * amps' latched faults (limiter, brown-out, clock, ...) are read and cleared,
 * so a window that tripped the amp's protection names itself.
 * tools/audio_spectro.py --compare renders the same script on the PC and
 * ranks the effects by how far the captured sound departs from it.
 *
 * Capture header, 512 B, u32 words (the stream follows):
 *   0 'TRC2' (written last)  1 mic rate  2 channels (1)  3 frames  4 windows
 *   5 I2S write failures  6 mic read failures  7 mic restarts  8 played ms
 *   9 real-time ms  10 synth rate  11 0x100 | synth version (1..3; round-2
 *   images wrote 0/1 = V1/V2)  12 digital volume
 *   13 amp register overrides applied  16.. per window, 5 words:
 *   type | kind << 8 | param << 16 | tone Hz / 10 << 24, frames, faults 0x4D, faults 0x4E,
 *   worst synth render cycles in the window. */
#include "snd_script.h"

#define TR_SND_CAPTURE_ADDR 0x02000000u
#define CAP_MAGIC           0x32435254u /* 'TRC2' */
#define CAP_HDR_BYTES       512u
#define CAP_LIMIT           0x0237F000u /* the event ring's page: stop below it */
#define MIC_BLOCK           768u        /* 16 ms at 48 kHz */
#define PREROLL_BLOCKS      4u          /* mic settling, not captured */

static volatile uint32_t *const s_cap_hdr = (volatile uint32_t *)TR_SND_CAPTURE_ADDR;
static int16_t *const           s_cap     = (int16_t *)(TR_SND_CAPTURE_ADDR + CAP_HDR_BYTES);
static uint32_t                 s_cap_frames, s_mic_errors, s_mic_restarts;

/* One 16 ms mic block; ch0 kept. A read error (the backend's sticky
 * overrun, ALP_ERR_IO) restarts the stream instead of failing forever --
 * the first TEST image started the PDM ~130 ms before its first read, the
 * 4-block slab overran, and every read after that returned the sticky
 * error. */
static void mic_block(bool keep)
{
	if (!TR_SND_MIC) {
		return;
	}
	static int16_t buf[MIC_BLOCK * 2u];
	size_t         got = 0;
	alp_status_t   rc  = alp_audio_in_read(s.mic, buf, MIC_BLOCK, &got, 100u);

	if (rc != ALP_OK || got != MIC_BLOCK) {
		if (s_mic_errors++ < 4u) {
			printk("[snd] mic read -> %d (got %u)\n", (int)rc, (unsigned)got);
		}
		if (rc == ALP_ERR_IO) {
			(void)alp_audio_in_stop(s.mic);
			(void)alp_audio_in_start(s.mic);
			s_mic_restarts++;
		}
		memset(buf, 0, sizeof(buf));
	}
	if (keep && TR_SND_CAPTURE_ADDR + CAP_HDR_BYTES + (s_cap_frames + MIC_BLOCK) * 2u <= CAP_LIMIT) {
		for (unsigned i = 0; i < MIC_BLOCK; i++) {
			s_cap[s_cap_frames + i] = buf[2u * i];
		}
		s_cap_frames += MIC_BLOCK;
		s_cap_hdr[3] = s_cap_frames;
	}
}

/* AC RMS and normalised Goertzel power at `hz` over one captured window. */
static void win_stats(uint32_t base, uint32_t n, uint32_t hz, float *rms, float *bin)
{
	double sum = 0, sq = 0;
	float  cf = 2.0f * cosf(2.0f * 3.14159265f * (float)hz / 48000.0f), s1 = 0, s2 = 0;

	for (uint32_t i = 0; i < n; i++) {
		float v = s_cap[base + i];
		sum += (double)v;
		sq += (double)v * (double)v;
		float s0 = v + cf * s1 - s2;
		s2       = s1;
		s1       = s0;
	}
	double m = n ? sum / n : 0.0;
	*rms     = n ? (float)sqrt(sq / n - m * m) : 0.0f;
	*bin     = n ? (s1 * s1 + s2 * s2 - cf * s1 * s2) / ((float)n * (float)n) : 0.0f;
}

static uint32_t amp_faults(unsigned i)
{
	uint32_t f = 0;
	(void)tas2563_read_faults(&s.amps[i], &f);
	(void)tas2563_clear_faults(&s.amps[i]);
	return f;
}

static int run_test(void)
{
	int step = bringup(TR_SND_MIC);
	if (step != 0) {
		printk("[snd] RESULT FAIL: bring-up step %d\n", step);
		teardown();
		return 0;
	}
	for (unsigned i = 0; i < CAP_HDR_BYTES / 4u; i++) {
		s_cap_hdr[i] = 0u;
	}
	s_cap_hdr[1]  = 48000u;
	s_cap_hdr[2]  = 1u;
	s_cap_hdr[4]  = SND_WINDOWS;
	s_cap_hdr[10] = TR_AUDIO_RATE;
	s_cap_hdr[11] = 0x100u | (TR_AUDIO_V3 ? 3u : TR_AUDIO_V2 ? 2u : 1u); /* 0x100: round-3 encoding */
	s_cap_hdr[12] = TR_SND_VOLUME;
	s_cap_hdr[13] = s_amp_overrides;

	uint32_t want_blocks = PREROLL_BLOCKS;
	for (unsigned w = 0; w < SND_WINDOWS; w++) {
		want_blocks += snd_windows[w].blocks;
	}
	const uint32_t want_ms = want_blocks * SND_BLOCK_MS;

	memset(s_mono, 0, sizeof(s_mono));
	if (TR_SND_MIC) {
		alp_status_t rc = alp_audio_in_start(s.mic); /* right before the first read */
		printk("[snd] PDM start -> %d\n", (int)rc);
	}
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		(void)amp_faults(i); /* drop whatever bring-up latched */
	}

	uint32_t writes_failed = 0, phase = 0, t0 = k_uptime_get_32();
	for (unsigned b = 0; b < PREROLL_BLOCKS; b++) {
		writes_failed += out_block(s_mono) != ALP_OK;
		mic_block(false);
	}
	uint32_t win_base[SND_WINDOWS];
	for (unsigned w = 0; w < SND_WINDOWS; w++) {
		const snd_win_t *sw  = &snd_windows[w];
		uint32_t         max = 0;
		win_base[w]          = s_cap_frames;
		for (unsigned b = 0; b < sw->blocks; b++) {
			uint32_t c0 = k_cycle_get_32();
			snd_script_block(sw, b, s_mono, &phase);
			uint32_t dc = k_cycle_get_32() - c0;
			max         = dc > max ? dc : max;
			writes_failed += out_block(s_mono) != ALP_OK;
			mic_block(true);
		}
		volatile uint32_t *rec = &s_cap_hdr[16u + 5u * w];
		rec[0] = sw->type | (uint32_t)sw->kind << 8 | (uint32_t)sw->param << 16 | (uint32_t)(sw->hz / 10u) << 24;
		rec[1] = s_cap_frames - win_base[w];
		rec[2] = amp_faults(0);
		rec[3] = amp_faults(1);
		rec[4] = max;
	}
	uint32_t ms = k_uptime_get_32() - t0;
	teardown();
	s_cap_hdr[5] = writes_failed;
	s_cap_hdr[6] = s_mic_errors;
	s_cap_hdr[7] = s_mic_restarts;
	s_cap_hdr[8] = ms;
	s_cap_hdr[9] = want_ms;
	if (TR_SND_MIC) {
		s_cap_hdr[0] = CAP_MAGIC;
	}

	printk("[snd] played %u ms of %u, %u write failures, %u mic read failures (%u restarts), clipped %u, "
	       "limited %u\n",
	       (unsigned)ms, (unsigned)want_ms, (unsigned)writes_failed, (unsigned)s_mic_errors,
	       (unsigned)s_mic_restarts, (unsigned)tr_audio_clipped(), (unsigned)tr_audio_limited());
	printk("[snd] synth %u Hz V%u, budget %u cycles per 16 ms block\n", TR_AUDIO_RATE,
	       TR_AUDIO_V3 ? 3u : TR_AUDIO_V2 ? 2u : 1u,
	       (unsigned)((uint64_t)sys_clock_hw_cycles_per_sec() * SND_BLOCK_MS / 1000u));
	for (unsigned w = 0; w < SND_WINDOWS; w++) {
		float rms = 0, bin = 0;
		if (TR_SND_MIC) {
			win_stats(win_base[w], s_cap_hdr[16u + 5u * w + 1u],
			          snd_windows[w].type == SND_W_TONE ? snd_windows[w].hz : 1000u, &rms, &bin);
		}
		printk("[snd] %-15s mic rms %8.1f bin %.3e  faults 0x4d %08x 0x4e %08x  synth max %u cyc\n",
		       snd_windows[w].name, (double)rms, (double)bin, (unsigned)s_cap_hdr[16u + 5u * w + 2u],
		       (unsigned)s_cap_hdr[16u + 5u * w + 3u], (unsigned)s_cap_hdr[16u + 5u * w + 4u]);
	}

	if (!TR_SND_MIC) {
		bool ok = writes_failed <= SND_MAX_FAILS && ms <= want_ms + want_ms / 10u;
		printk("[snd] RESULT %s (play-only): %s\n", ok ? "PASS" : "FAIL",
		       ok ? "I2S3 streamed in real time; listen for each effect" : "I2S3 did not stream in real time");
		return 0;
	}
	/* snd_verdict.h's three windows: silence, the -12 dBFS 1 kHz tone, music
	 * (one mic, so channel 1 stays dead and is skipped). */
	snd_stats_t st = {
		.frames = s_cap_frames, .want_frames = (want_blocks - PREROLL_BLOCKS) * MIC_BLOCK,
		.write_fail = writes_failed, .mic_fail = s_mic_errors, .played_ms = ms, .want_ms = want_ms,
	};
	const unsigned k_vw[3] = { snd_window_of(SND_W_SILENCE), snd_window_of(SND_W_TONE),
	                           snd_window_of(SND_W_MUSIC) }; /* silence, tone1k_-12dBFS, music */
	for (unsigned k = 0; k < 3u; k++) {
		win_stats(win_base[k_vw[k]], s_cap_hdr[16u + 5u * k_vw[k] + 1u], 1000u, &st.rms[k][0], &st.bin[k][0]);
	}
	const char *why = snd_verdict(&st);
	if (why == NULL) {
		printk("[snd] RESULT PASS: the mic heard the tone and the music; per-effect verdict: "
		       "tools/audio_spectro.py --compare\n");
	} else {
		printk("[snd] RESULT FAIL: %s\n", why);
	}
	return 0;
}
#else
/* ---- GAME: the M55-HP firmware --------------------------------------------- */
#define TR_SND_GAME_SEED      0x5EEDu
#define UNDERRUN_FAULT_BLOCKS 8u  /* 8 failed writes in a row (>= 0.8 s at the 100 ms timeout) */
#define UNDERRUN_FAULT_STEP   12u /* hp_fault_step: streaming underrun, not a bring-up step */
static volatile tr_aring_t *const s_ring = (volatile tr_aring_t *)TR_ARING_ADDR;

static void dsb(void)
{
	barrier_dsync_fence_full();
}

static int run_game(void)
{
	int step = bringup(false);
	if (step != 0) {
		printk("[snd] bring-up failed at step %d -- no game sound\n", step);
		teardown();
		for (;;) { /* keep reporting: the HE may re-init the ring after us */
			s_ring->hp_state      = TR_ARING_HP_FAULT;
			s_ring->hp_fault_step = (uint32_t)step;
			k_msleep(100);
		}
	}
	/* Fixed seed: the HP's output for a given event sequence is the host
	 * render's, bit for bit (tools/audio_preview.c, same synth build). */
	tr_audio_init(TR_SND_GAME_SEED);
	printk("[snd] game sound running at %u Hz: ring at 0x%08x\n", RATE, TR_ARING_ADDR);
	uint32_t fails = 0; /* consecutive failed block writes */
	for (;;) {
		tr_aev_t e;
		while (tr_aring_pop(s_ring, &e, dsb)) {
			tr_audio_event(e.kind, e.param);
			s_ring->hp_events = s_ring->hp_events + 1u;
		}
		tr_audio_render(s_mono, BLOCK);
		if (out_block(s_mono) != ALP_OK) {
			s_ring->hp_underruns = s_ring->hp_underruns + 1u;
			fails++;
		} else {
			fails = 0u;
		}
		/* A write storm (bit clock gone, amp path broken) reads FAULT on the
		 * HUD, not "audio"; one good block clears it. */
		if (fails >= UNDERRUN_FAULT_BLOCKS) {
			s_ring->hp_fault_step = UNDERRUN_FAULT_STEP;
			s_ring->hp_state      = TR_ARING_HP_FAULT;
		} else {
			s_ring->hp_state = TR_ARING_HP_RUNNING;
		}
		s_ring->hp_heartbeat = s_ring->hp_heartbeat + 1u;
	}
	return 0;
}
#endif

int main(void)
{
	(void)alp_init();
	printk("[snd] Trace Runner sound (%s), %u Hz, volume %u\n", TR_SND_TEST ? "TEST" : "GAME", RATE,
	       TR_SND_VOLUME);
#if TR_SND_TEST
	return run_test();
#else
	return run_game();
#endif
}

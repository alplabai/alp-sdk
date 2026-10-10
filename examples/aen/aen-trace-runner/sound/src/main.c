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
 *   5 carrier I2C bus 0 open -- after a 2 ms settle (TR_SND_AMP_SETTLE_US, not the SDK's 200 us)
 *  5b each amp polled for its address ACK (1 ms steps, <= 50 ms; src/audio/tr_amp_ready.h)
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
 * TR_SND_EMBED=1 (GAME only): this file is linked INTO the HP vision image (hp_vision/ with
 * -DTR_HP_SOUND=ON, docs/2026-09-23-sound.md "Sound with the vision HP") -- no main() here;
 * run_game() runs as its own cooperative thread, above the vision (main) thread. Same bring-up,
 * same amp values, same stream. Additions, each inside `#if TR_SND_EMBED` (or SND_BUS() /
 * SND_RELEASE(), empty without it): lease I2C2 + GPIO5 from the HE (src/ipc/tr_bus2.h) ONLY
 * around the stretches that use them -- the proxy attach's EEPROM read in step 1, steps 4-7 and
 * step 11 -- and give the bus BACK to the HE across the rest (the CC3501E reset and steps 2-3,
 * the I2S3 bring-up of steps 8-10; the HUD power line needs the bus); every lease re-arms the
 * DT-deferred I2C2 (IC_ENABLE = 0, INTR_MASK = 0, pending IRQ cleared, driver semaphore reset,
 * bus-clear if stuck: src/platform/tr_i2c2_rearm.h); this core's I2C2 IRQ is armed only inside
 * a Dekker-checked bus step (tr_snd_bus_enter) and off the moment it ends; a take-back seen at an
 * entry aborts with no further bus access and waits again; the lease beats hp_beat at every
 * enter / leave / poll (a step between two entries is <= 50 ms); the I2S3 underrun indications
 * into the lease record; a paced retry after a failed write.
 * With TR_SND_EMBED=0 none of this is compiled; the amp settle and ACK poll (steps 5 / 5b) apply
 * to the standalone GAME and TEST images and the embedded one alike.
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

#include "audio/tr_amp_ready.h"
#include "audio/tr_audio.h"
#include "cc3501e_bridge.h"
#include "snd_verdict.h"
#include "ipc/tr_aring.h"

#ifndef TR_SND_EMBED
#define TR_SND_EMBED 0
#endif
#if TR_SND_EMBED
#include "ipc/tr_vol.h" /* the HE owns a volume word only in the combined HP image */
#endif
#if TR_SND_EMBED
#if TR_SND_TEST
#error "TR_SND_EMBED is the GAME firmware inside hp_vision; the TEST image stays standalone"
#endif
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>

#include "ipc/tr_bus2.h"
#include "platform/tr_i2c2_rearm.h"

#ifndef TR_SND_UNDERRUN_TEST
#define TR_SND_UNDERRUN_TEST 0 /* DEV bench control; a release refuses an image that has it */
#endif

/* Entering a bring-up step that touches I2C2 or GPIO5 (tr_bus2.h): with no lease held, wait for
 * the HE's offer and lease the bus; then hp_state = BUS, fence, THEN the HE's offer must still
 * stand -- else stop HERE, before the access. This core's I2C2 IRQ is armed only after the entry
 * passed. SND_RELEASE() ends a stretch: IRQ off, the lease handed BACK (the HE owns the bus for
 * the CC3501E reset and the I2S3 bring-up). tr_snd_bus_enter() / _release() are also called by
 * cc3501e_bridge.c around the proxy attach (its identity-EEPROM read @0x50). */
#define SND_ABORTED 100
bool tr_snd_bus_enter(void);
void tr_snd_bus_leave(void);
void tr_snd_bus_release(void);
static bool
    s_aborted; /* a bus entry saw the HE take the bus back (read after the CC3501E bridge) */
/* What the lease wait does instead of sleeping (step 11 keeps the bit clock fed). */
static void (*s_idle)(void);
#define SND_BUS() \
	do { \
		if (!tr_snd_bus_enter()) { \
			return SND_ABORTED; \
		} \
	} while (0)
#define SND_RELEASE()   tr_snd_bus_release()
#define SND_BEAT()      (((volatile tr_bus2_t *)TR_MEM_BUS2)->hp_beat++)
#define SND_IDLE_SET(f) (s_idle = (f))
static bool s_bridge_up; /* step 1 done: a retried bring-up keeps the bridge and its handles */
#else
#define SND_BUS()       (void)0
#define SND_RELEASE()   (void)0
#define SND_BEAT()      (void)0
#define SND_IDLE_SET(f) (void)0
#endif

/* Digital volume, alp_audio_out_set_volume() 0..255 (255 = unity). 128 is the
 * level heard "louder and clean" on the 2026W36-0002 speakers (2026-09-15, amp
 * analog level MIN); the speakers' power rating is unknown, so it is also
 * the cap. */
#ifndef TR_SND_VOLUME
#define TR_SND_VOLUME 128u
#endif
BUILD_ASSERT(TR_SND_VOLUME <= 128u,
             "above the bench-heard level: ask before raising (speaker rating unknown)");

#define RATE      TR_AUDIO_RATE
#define BLOCK     (RATE * 16u / 1000u) /* frames per 16 ms block: 256 at 16 kHz, 768 at 48 kHz */
#define WRITE_TMO 100u

#define AMP_ENABLE_PIN           2 /* P5_2 = SD_N, active high (R138 pull-up to +VIO) */
#define AMP_FAULT_PIN            0 /* P5_0 = IRQZ, open drain, active low */
#define AMP_FAULT_PAD_REN        (1U << 16)
#define AMP_ENABLE_RESET_HOLD_MS 24u /* >= 23.8 ms max SDZ_TIMEOUT (SLASET3D Table 7-7) */
#define MUX_SETTLE_MS            10u
/* Step 10's bit-clock proof: 8 silent 16 ms blocks. The audio_out slab
 * buffers a couple, so the writes return after ~5-6 blocks of real time
 * (80-96 ms); a dead clock times the third write out instead. */
#define CLOCK_CHECK_BLOCKS 8u
#define CLOCK_CHECK_MIN_MS 60u
#define CLOCK_CHECK_MAX_MS 250u
#define AMP_COUNT          2u

static const pinctrl_soc_pin_t amp_enable_mux[]     = { PIN_P5_2__GPIO };
static const pinctrl_soc_pin_t amp_fault_mux[]      = { PIN_P5_0__GPIO | AMP_FAULT_PAD_REN };
static const uint8_t           amp_addrs[AMP_COUNT] = { TAS2563_I2C_ADDR_GND_PULL,
	                                                    TAS2563_I2C_ADDR_VDD_PULL };
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

/* Step 5b's I/O (tr_amp_ready.h): a 1-byte read on the carrier bus. */
static bool amp_ack(void *ctx, uint8_t addr)
{
	uint8_t b;

	(void)ctx;
	return alp_i2c_read(s.bus, addr, &b, 1u) == ALP_OK;
}

static void amp_sleep_us(uint32_t us)
{
	k_usleep((int32_t)us);
}

static uint64_t amp_now_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

#if TR_SND_EMBED
/* One silent block: feeds the running bit clock while a lease is awaited (out_block paces it). */
static void keepalive_block(void)
{
	memset(s_mono, 0, sizeof(s_mono));
	(void)out_block(s_mono);
}
#endif

/* Steps 1..11 above; returns 0, or the failing step number. */
static int bringup(bool with_mic)
{
	alp_status_t rc = ALP_OK;
#if TR_SND_EMBED
	if (!s_bridge_up) {
		s_aborted = false;
		rc =
		    cc3501e_bridge_bringup(&s.fw); /* bus entry around its EEPROM read (cc3501e_bridge.c) */
		if (s_aborted) {
			return SND_ABORTED;
		}
		printk("[snd] 1 cc3501e_bridge_bringup -> %d\n", (int)rc);
		if (rc != ALP_OK) return 1;
		s_bridge_up = true;
	}
	if (s.mux_sel == NULL) s.mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	if (s.mux_en == NULL) s.mux_en = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
#else
	rc = cc3501e_bridge_bringup(&s.fw);
	printk("[snd] 1 cc3501e_bridge_bringup -> %d\n", (int)rc);
	if (rc != ALP_OK) return 1;

	s.mux_sel = alp_gpio_open(EVK_PIN_I2S_MUX_SEL);
	s.mux_en  = alp_gpio_open(EVK_PIN_I2S_MUX_EN);
#endif
	if (s.mux_sel == NULL || s.mux_en == NULL) {
		printk("[snd] mux open failed (IO8 needs a CRC-valid manifest with hw_rev %s) err=%d\n",
		       CONFIG_ALP_SDK_SOM_HW_REV,
		       (int)alp_last_error());
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

	SND_BUS(); /* steps 4-7: GPIO5, then I2C2 -- one uninterrupted bus stretch */
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
	/* Not the SDK's TAS2563_RESET_SETTLE_US (200 us): on the bench carrier U27 (0x4D) NACKs its
	 * address 313 us after SD_N and first ACKs at 1142 us (2026-09-30) -- settle
	 * TR_SND_AMP_SETTLE_US (2 ms), then poll each amp for its ACK below (step 5b). */
	k_usleep(TR_SND_AMP_SETTLE_US);

	SND_BUS();
#if TR_SND_EMBED
	if (s.bus == NULL)
#endif
		s.bus = alp_i2c_open(
		    &(alp_i2c_config_t){ .bus_id = EVK_I2C_BUS_SENSORS, .bitrate_hz = 100000u });
	printk("[snd] 5 alp_i2c_open(bus %d) -> %s\n", (int)EVK_I2C_BUS_SENSORS, s.bus ? "ok" : "NULL");
	if (s.bus == NULL) return 5;

	/* 5b: each amp answers its address before tas2563_init (tr_amp_ready.h): a 1-byte read every
	 * 1 ms, at most 50 ms. A timeout is a bring-up failure at step 6 (teardown, no sound). */
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		static const tr_amp_io_t amp_io = { amp_ack, amp_sleep_us, amp_now_us, NULL };
		uint32_t                 tries, waited;

		SND_BUS();
		if (!tr_amp_wait_ack(&amp_io,
		                     amp_addrs[i],
		                     TR_SND_AMP_POLL_US,
		                     TR_SND_AMP_POLL_MAX_US,
		                     &tries,
		                     &waited)) {
			printk("[snd] RESULT FAIL: amp 0x%02x did not ACK within %u ms (%u reads)\n",
			       amp_addrs[i],
			       (unsigned)(TR_SND_AMP_POLL_MAX_US / 1000u),
			       (unsigned)tries);
			return 6;
		}
		printk("[snd] 5b amp 0x%02x ACK: %u read(s), %u us after the %u us settle\n",
		       amp_addrs[i],
		       (unsigned)tries,
		       (unsigned)waited,
		       (unsigned)TR_SND_AMP_SETTLE_US);
	}

	for (unsigned i = 0; i < AMP_COUNT; i++) {
		SND_BUS();
		rc = tas2563_init(&s.amps[i], s.bus, amp_addrs[i], NULL);
		printk("[snd] 6 tas2563_init(0x%02x) -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 6;
	}
	const alp_i2s_config_t i2s = {
		.bus_id         = 0,
		.direction      = ALP_I2S_DIR_TX,
		.sample_rate_hz = RATE,
		.channels       = 2,
		.word_bits      = 16,
		.format         = ALP_I2S_FMT_I2S,
		.block_frames   = BLOCK,
	};
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		SND_BUS();
		rc = tas2563_set_amp_level(&s.amps[i], TAS2563_AMP_LEVEL_MIN);
		if (rc == ALP_OK) rc = tas2563_configure_i2s(&s.amps[i], &i2s, amp_rx[i]);
		printk("[snd] 7 amp 0x%02x level MIN + configure_i2s -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 7;
#ifdef TR_SND_AMP_REGS
		/* Bench A/B: extra register writes from -DTR_SND_AMP_REGS_FILE
		 * (sound/amp_regs_example.h), after the driver's own config. */
		size_t bad = 0;

		rc = ALP_OK;
		for (size_t k = 0; k < ARRAY_SIZE(k_amp_regs) && rc == ALP_OK; k++) {
			size_t one_bad = 0;

			SND_BEAT(); /* a long override list must not outlast the HE's 2 s heartbeat limit */
			rc  = tas2563_load_tuning(&s.amps[i], &k_amp_regs[k], 1u, &one_bad);
			bad = k + one_bad;
		}
		printk("[snd] 7b amp 0x%02x %u override writes -> %d (record %u)\n",
		       amp_addrs[i],
		       (unsigned)ARRAY_SIZE(k_amp_regs),
		       (int)rc,
		       (unsigned)bad);
		if (rc != ALP_OK) return 7;
		s_amp_overrides = ARRAY_SIZE(k_amp_regs);
#endif
	}

	if (with_mic) {
		/* Opened here, STARTED only right before the first read: the
		 * backend's slab holds 4 blocks (64 ms) and an overrun is sticky. */
		s.mic = alp_audio_in_open(&(alp_audio_config_t){ .peripheral_id    = 0,
		                                                 .sample_rate_hz   = 48000u,
		                                                 .channels         = 2,
		                                                 .format           = ALP_AUDIO_FMT_S16_LE,
		                                                 .frames_per_block = 768u });
		rc    = s.mic ? ALP_OK : alp_last_error();
		printk("[snd] 8 PDM open (48 kHz stereo) -> %d\n", (int)rc);
		if (rc != ALP_OK) return 8;
	}

	SND_RELEASE(); /* steps 8-10: PDM / I2S3 only -- the bus goes back to the HE */
	s.spk = alp_audio_out_open(&(alp_audio_config_t){ .peripheral_id    = 0,
	                                                  .sample_rate_hz   = RATE,
	                                                  .channels         = 2,
	                                                  .format           = ALP_AUDIO_FMT_S16_LE,
	                                                  .frames_per_block = BLOCK });
	rc    = s.spk ? alp_audio_out_set_volume(s.spk, TR_SND_VOLUME) : alp_last_error();
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
	rc             = ALP_OK;
	for (unsigned b = 0; b < CLOCK_CHECK_BLOCKS && rc == ALP_OK; b++) {
		rc = out_block(s_mono);
	}
	uint32_t clk_ms = k_uptime_get_32() - t_clk;
	printk("[snd] 10 %u silent blocks -> %d in %u ms (expect %u..%u)\n",
	       CLOCK_CHECK_BLOCKS,
	       (int)rc,
	       (unsigned)clk_ms,
	       CLOCK_CHECK_MIN_MS,
	       CLOCK_CHECK_MAX_MS);
	if (rc != ALP_OK || clk_ms < CLOCK_CHECK_MIN_MS || clk_ms > CLOCK_CHECK_MAX_MS) {
		printk("[snd] I2S3 is not consuming blocks at %u Hz: no bit clock. Is ZEPHYR_BASE's "
		       "clock_control_alif.c patched (alp-sdk zephyr/patches/zephyr/0001)?\n",
		       RATE);
		return 10; /* never wake the amps without a running clock */
	}

	/* The lease for step 11 is waited for with the bit clock RUNNING: silence is written while the
	 * HE's offer is pending (a stopped clock would let the amp sleep before it is resumed). */
	SND_IDLE_SET(keepalive_block);
	for (unsigned i = 0; i < AMP_COUNT; i++) {
		SND_BUS();
		SND_IDLE_SET(NULL);
		rc = tas2563_resume(&s.amps[i]);
		printk("[snd] 11 tas2563_resume(0x%02x) -> %d\n", amp_addrs[i], (int)rc);
		if (rc != ALP_OK) return 11;
	}
	SND_RELEASE();
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
	if (keep &&
	    TR_SND_CAPTURE_ADDR + CAP_HDR_BYTES + (s_cap_frames + MIC_BLOCK) * 2u <= CAP_LIMIT) {
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
	s_cap_hdr[11] = 0x100u | (TR_AUDIO_V3   ? 3u
	                          : TR_AUDIO_V2 ? 2u
	                                        : 1u); /* 0x100: round-3 encoding */
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
		rec[0]                 = sw->type | (uint32_t)sw->kind << 8 | (uint32_t)sw->param << 16 |
		                         (uint32_t)(sw->hz / 10u) << 24;
		rec[1]                 = s_cap_frames - win_base[w];
		rec[2]                 = amp_faults(0);
		rec[3]                 = amp_faults(1);
		rec[4]                 = max;
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

	printk("[snd] played %u ms of %u, %u write failures, %u mic read failures (%u restarts), "
	       "clipped %u, "
	       "limited %u\n",
	       (unsigned)ms,
	       (unsigned)want_ms,
	       (unsigned)writes_failed,
	       (unsigned)s_mic_errors,
	       (unsigned)s_mic_restarts,
	       (unsigned)tr_audio_clipped(),
	       (unsigned)tr_audio_limited());
	printk("[snd] synth %u Hz V%u, budget %u cycles per 16 ms block\n",
	       TR_AUDIO_RATE,
	       TR_AUDIO_V3   ? 3u
	       : TR_AUDIO_V2 ? 2u
	                     : 1u,
	       (unsigned)((uint64_t)sys_clock_hw_cycles_per_sec() * SND_BLOCK_MS / 1000u));
	for (unsigned w = 0; w < SND_WINDOWS; w++) {
		float rms = 0, bin = 0;
		if (TR_SND_MIC) {
			win_stats(win_base[w],
			          s_cap_hdr[16u + 5u * w + 1u],
			          snd_windows[w].type == SND_W_TONE ? snd_windows[w].hz : 1000u,
			          &rms,
			          &bin);
		}
		printk("[snd] %-15s mic rms %8.1f bin %.3e  faults 0x4d %08x 0x4e %08x  synth max %u cyc\n",
		       snd_windows[w].name,
		       (double)rms,
		       (double)bin,
		       (unsigned)s_cap_hdr[16u + 5u * w + 2u],
		       (unsigned)s_cap_hdr[16u + 5u * w + 3u],
		       (unsigned)s_cap_hdr[16u + 5u * w + 4u]);
	}

	if (!TR_SND_MIC) {
		bool ok = writes_failed <= SND_MAX_FAILS && ms <= want_ms + want_ms / 10u;
		printk("[snd] RESULT %s (play-only): %s\n",
		       ok ? "PASS" : "FAIL",
		       ok ? "I2S3 streamed in real time; listen for each effect"
		          : "I2S3 did not stream in real time");
		return 0;
	}
	/* snd_verdict.h's three windows: silence, the -12 dBFS 1 kHz tone, music
	 * (one mic, so channel 1 stays dead and is skipped). */
	snd_stats_t st = {
		.frames      = s_cap_frames,
		.want_frames = (want_blocks - PREROLL_BLOCKS) * MIC_BLOCK,
		.write_fail  = writes_failed,
		.mic_fail    = s_mic_errors,
		.played_ms   = ms,
		.want_ms     = want_ms,
	};
	const unsigned k_vw[3] = { snd_window_of(SND_W_SILENCE),
		                       snd_window_of(SND_W_TONE),
		                       snd_window_of(SND_W_MUSIC) }; /* silence, tone1k_-12dBFS, music */
	for (unsigned k = 0; k < 3u; k++) {
		win_stats(win_base[k_vw[k]],
		          s_cap_hdr[16u + 5u * k_vw[k] + 1u],
		          1000u,
		          &st.rms[k][0],
		          &st.bin[k][0]);
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

#if TR_SND_EMBED
/* ---- the I2C2 + GPIO5 lease (src/ipc/tr_bus2.h) ----------------------------------------------
 * This core's I2C2 (carrier bus 0, 0x49012000, IRQ 134) is DT `zephyr,deferred-init`
 * (hp_vision/sound_hp.overlay): no register write, no ISR until the HE has leased the bus. The
 * lease is taken lazily by the first bus entry of a stretch (tr_snd_bus_enter) and handed back by
 * SND_RELEASE; this core's IRQ 134 is on only between an entry that passed and the release, so
 * the HE alone resetting while this core holds or has returned the lease never meets an armed
 * i2c_dw_isr. Any later runtime write to the amps (volume, resume) is bracketed the same way:
 * SND_BUS(); ...; SND_RELEASE(). */
#define SND_I2C2_NODE   TR_I2C2_NODE
BUILD_ASSERT(DT_PROP(SND_I2C2_NODE, zephyr_deferred_init),
             "hp_vision/sound_hp.overlay must defer the HP's I2C2 init");
BUILD_ASSERT(DT_PROP(SND_I2C2_NODE, clock_frequency) == 100000,
             "the HP's I2C2 is standard mode, as the HE's");

static volatile tr_bus2_t *const s_b2 = (volatile tr_bus2_t *)TR_MEM_BUS2;
static tr_bus2_hp_t              s_lease;
static bool                      s_leased;    /* a lease is held: the HE stays off the bus */
static bool                      s_i2c2_init; /* device_init() done once; a re-lease re-arms it */
/* s_in_bus: this core has been inside ONE uninterrupted bus step since an entry that saw the
 * offer standing -- a take-back seen now was made while the HE could read hp_state == BUS, so the
 * HE is parked in its (<= 500 ms) boot wait and a GPIO5 write in the abort is safe. After a
 * no-bus step it may not be: no GPIO5 then. */
static bool s_in_bus, s_abort_gpio_ok;

/* PRE_KERNEL_1, before any driver: forget what a previous session left. */
static int snd_bus2_boot(void)
{
	tr_bus2_hp_boot(s_b2, dsb);
	return 0;
}
SYS_INIT(snd_bus2_boot, PRE_KERNEL_1, 0);

static void snd_irq_on(void)
{
	NVIC_ClearPendingIRQ(TR_I2C2_IRQN);
	irq_enable(TR_I2C2_IRQN);
}

static void snd_irq_off(void)
{
	irq_disable(TR_I2C2_IRQN);
}

/* Wait for a live offer from the HE (it offers once its display is up and it sees this core
 * waiting), enter the first bus step of the lease, and re-arm this core's I2C2 for it. A missing or
 * dead HE: wait forever -- the vision thread is unaffected, there is only no sound. A core whose
 * D-cache is on (a warm RAM-run inherits CCR.DC=1) never claims: the lease record is not coherent.
 * The IRQ stays off here; the caller arms it. */
static void lease_acquire(void)
{
	bool told = false, dc_told = false;

	for (;;) {
		while ((SCB->CCR & SCB_CCR_DC_Msk) != 0u) {
			if (!dc_told) {
				printk("[snd] D-cache is ON (SCB->CCR.DC): not claiming the I2C2 lease\n");
				dc_told = true;
			}
			k_msleep(100);
		}
		tr_bus2_hp_want(&s_lease, s_b2, dsb);
		while (!tr_bus2_hp_poll(&s_lease, s_b2)) {
			if (!told) {
				printk("[snd] waiting for the HE's I2C2 + GPIO5 offer (0x%08x) -- no bus access "
				       "before it\n",
				       (unsigned)TR_MEM_BUS2);
				told = true;
			}
			if (s_idle != NULL) {
				s_idle();
			} else {
				k_msleep(TR_BUS2_HP_POLL_MS);
			}
		}
		if (tr_bus2_hp_enter(&s_lease, s_b2, dsb)) {
			break;
		}
		tr_bus2_hp_abort(&s_lease, s_b2, dsb); /* withdrawn between the accept and the entry */
	}
	const struct device *i2c2 = DEVICE_DT_GET(SND_I2C2_NODE);
	int                  rc;
	bool                 sda = tr_i2c2_quiesce(i2c2, s_i2c2_init);

	if (!s_i2c2_init) {
		rc = device_init(i2c2);
		snd_irq_off(); /* the driver armed it: only an accepted entry may */
		s_i2c2_init = true;
	} else {
		/* the HE's own i2c_dw reprogrammed the controller in between */
		rc = i2c_configure(i2c2, I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER);
	}
	printk("[snd] HE offered I2C2 + GPIO5: controller re-armed -> %d%s\n",
	       rc,
	       sda ? "" : " (SDA still low after the bus-clear)");
	s_leased = true;
	s_in_bus = true;
}

bool tr_snd_bus_enter(void)
{
	if (!s_leased) {
		lease_acquire(); /* returns inside the first bus step of the lease */
		snd_irq_on();
		return true;
	}
	bool was = s_in_bus;

	if (!tr_bus2_hp_enter(&s_lease, s_b2, dsb)) {
		printk(
		    "[snd] the HE took I2C2 + GPIO5 back -- bring-up aborted before its next bus access\n");
		s_abort_gpio_ok = was;
		s_in_bus        = false;
		s_aborted       = true;
		s_leased        = false;
		return false;
	}
	s_in_bus = true;
	snd_irq_on(); /* only after the entry passed */
	return true;
}

/* A step on SPI1 / LP-GPIO / I2S3 only, the lease kept: IRQ off first (an HE-only reset in this
 * window must not meet a live i2c_dw_isr), then HELD. The bring-up returns the lease instead
 * (tr_snd_bus_release), except where it must keep it. */
void tr_snd_bus_leave(void)
{
	snd_irq_off();
	s_in_bus = false;
	tr_bus2_hp_leave(&s_lease, s_b2, dsb);
}

/* End of a bus stretch (pass or fail): IRQ off, then the bus goes BACK to the HE (the HUD power
 * line, the HE's own transfers). */
void tr_snd_bus_release(void)
{
	if (!s_leased) {
		return;
	}
	tr_snd_bus_leave();
	tr_bus2_hp_return(&s_lease, s_b2, dsb);
	s_leased = false;
	printk("[snd] I2C2 + GPIO5 given back to the HE\n");
}

/* The HE took the bus back in the middle of the bring-up: stop WITHOUT another I2C2 transfer.
 * I2S3 stopped; the mux disabled (/E high, a CC3501E GPIO over SPI1 -- not a shared bus: the
 * amps see no I2S); S stays 0. SD_N low (both amps in hardware shutdown) only when the take-back
 * came inside an uninterrupted bus step (s_abort_gpio_ok: the HE is parked in its boot wait).
 * Then back to waiting for a NEW live offer. */
static void abort_bringup(void)
{
	if (s_abort_gpio_ok && s.gpio5 != NULL && device_is_ready(s.gpio5)) {
		(void)gpio_pin_set(s.gpio5, AMP_ENABLE_PIN, 0);
	}
	if (s.spk != NULL) {
		(void)alp_audio_out_stop(s.spk);
		alp_audio_out_close(s.spk);
		s.spk = NULL;
	}
	if (s.mux_en != NULL) {
		(void)alp_gpio_write(s.mux_en, true);
	}
	snd_irq_off();
	s_leased        = false;
	s_in_bus        = false;
	s_abort_gpio_ok = false;
	SND_IDLE_SET(NULL);
	tr_bus2_hp_abort(&s_lease, s_b2, dsb);
}

/* The bring-up, retried after every abort. */
static int bringup_handoff(void)
{
	for (;;) {
		int step = bringup(false);

		if (step != SND_ABORTED) {
			return step;
		}
		abort_bringup();
	}
}

/* I2S3 (0x49017000) status, read-only except TOR (reading it clears TXFO + TXFU on the E8;
 * i2s_dw treats TXFO as "should not happen" and clears it the same way). */
#define SND_I2S3_ITER   0x49017008u
#define SND_I2S3_ISR    0x49017038u
#define SND_I2S3_TOR    0x49017044u
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(i2s3)) == 0x49017000, "I2S3 status registers");
/* The 500 us FIFO refill deadline (hp_vision/sound_hp.overlay): I2S3 above the camera, CSI, U55
 * and I2C1. */
#define SND_IRQ_PRIO(l) DT_IRQ(DT_NODELABEL(l), priority)
BUILD_ASSERT(SND_IRQ_PRIO(i2s3) < SND_IRQ_PRIO(ethosu55) &&
                 SND_IRQ_PRIO(i2s3) < SND_IRQ_PRIO(cam) && SND_IRQ_PRIO(i2s3) < SND_IRQ_PRIO(csi) &&
                 SND_IRQ_PRIO(i2s3) < SND_IRQ_PRIO(i2c1),
             "I2S3 must be the highest-priority IRQ of I2S3 / camera / CSI / U55 / I2C1 on the HP");
#define SND_I2S_TXFU    (1u << 6) /* ISR0.TXFU: TX FIFO underrun (E8), sticky until TOR is read */
#define SND_I2S_TXEN (1u << 0) /* ITER.TXEN: 0 = i2s_dw parked the TX block (its underrun exit) */

static void i2s_status(void)
{
	if (*(volatile const uint32_t *)SND_I2S3_ISR & SND_I2S_TXFU) {
		s_b2->hp_i2s_fu = s_b2->hp_i2s_fu + 1u;
		(void)*(volatile const uint32_t *)SND_I2S3_TOR;
	}
	if ((*(volatile const uint32_t *)SND_I2S3_ITER & SND_I2S_TXEN) == 0u) {
		s_b2->hp_i2s_err =
		    s_b2->hp_i2s_err + 1u; /* the write below recovers it (PREPARE + START) */
	}
}
#endif

static int run_game(void)
{
#if TR_SND_EMBED
	int step = bringup_handoff(); /* a success has already given the bus back */

	if (step != 0) {
		if (tr_snd_bus_enter()) {
			teardown(); /* uses I2C2 + GPIO5 (amp shutdown) -- inside a bus step */
			SND_RELEASE();
		} else {
			abort_bringup(); /* taken back at the same moment: no bus access */
		}
	}
#else
	int step = bringup(false);
#endif
	if (step != 0) {
		printk("[snd] bring-up failed at step %d -- no game sound\n", step);
#if !TR_SND_EMBED
		teardown();
#endif
		for (;;) { /* keep reporting: the HE may re-init the ring after us */
			s_ring->hp_state      = TR_ARING_HP_FAULT;
			s_ring->hp_fault_step = (uint32_t)step;
			k_msleep(100);
		}
	}
	/* Fixed seed: the HP's output for a given event sequence is the host
	 * render's, bit for bit (tools/audio_preview.c, same synth build). */
	tr_audio_init(TR_SND_GAME_SEED);
#if TR_SND_EMBED
	/* The volume word (src/ipc/tr_vol.h): the HE owns it, this core scales every block by it.
	 * A software gain, so a change never touches I2C2 (leased) or SD_N; 0 is silence with the amps
	 * running. Starts at the level already published: no ramp up from unity on the first block.
	 * Only the combined HP image has an HE that publishes it; the standalone sound image plays at
	 * unity (TR_SND_VOLUME). */
	static tr_vol_ramp_t           ramp;
	volatile const tr_vol_t *const vol = (volatile const tr_vol_t *)TR_MEM_VOL;

	tr_vol_ramp_init(&ramp, tr_vol_read(vol->vol));
#endif
	printk("[snd] game sound running at %u Hz: ring at 0x%08x\n", RATE, TR_ARING_ADDR);
	uint32_t fails = 0; /* consecutive failed block writes */
#if TR_SND_EMBED
	(void)*(volatile const uint32_t *)SND_I2S3_TOR; /* drop the bring-up's start-up TXFU */
#if TR_SND_UNDERRUN_TEST
	uint32_t blocks = 0;
#endif
#endif
	for (;;) {
		tr_aev_t e;
		while (tr_aring_pop(s_ring, &e, dsb)) {
			tr_audio_event(e.kind, e.param);
			s_ring->hp_events = s_ring->hp_events + 1u;
		}
		tr_audio_render(s_mono, BLOCK);
#if TR_SND_EMBED
		tr_vol_apply(&ramp, s_mono, BLOCK, tr_vol_read(vol->vol));
#endif
#if TR_SND_EMBED
#if TR_SND_UNDERRUN_TEST
		/* DEV positive control (TR_SND_UNDERRUN_TEST, refused by a32/release/hp_vision_check.sh and
		 * snd_hp_check.sh): once, after ~3 s of stream, hold the I2S3 IRQ off 2 ms -- four times
		 * the 500 us FIFO refill deadline -- so the FIFO runs empty and hp_i2s_fu MUST count. The
		 * bench uses it to prove the counter can count at all (the HWRM describes TXFU only under
		 * TDM mode; the driver's 2-block queue never runs dry in 2 ms by itself): a 0 in
		 * hp_i2s_fu proves nothing until this has made it count. */
		if (++blocks == 200u) {
			printk("[snd] underrun control: I2S3 IRQ held off 2000 us (hp_i2s_fu must count)\n");
			irq_disable(DT_IRQN(DT_NODELABEL(i2s3)));
			k_busy_wait(2000);
			irq_enable(DT_IRQN(DT_NODELABEL(i2s3)));
		}
#endif
		i2s_status(); /* before the write: a parked TX block is still parked here */
#endif
		if (out_block(s_mono) != ALP_OK) {
			s_ring->hp_underruns = s_ring->hp_underruns + 1u;
			fails++;
#if TR_SND_EMBED
			/* A failed write can return at once (the driver in its error state): a
			 * cooperative thread that loops without blocking would starve the vision
			 * thread for good. Pace the retry at the block rate. */
			k_msleep(BLOCK * 1000u / RATE);
#endif
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

#if TR_SND_EMBED
/* The sound thread of the combined HP image. Cooperative, above the vision (main) thread: it
 * runs ~0.5 ms per 16 ms block and must never wait behind the vision's CPU stages; the vision
 * thread sleeps through every NPU invoke. */
#ifndef TR_SND_EMBED_PRIO
#define TR_SND_EMBED_PRIO \
	K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 2) /* -2: above the system workqueue (-1) */
#endif
#define TR_SND_EMBED_STACK 8192 /* the standalone image's CONFIG_MAIN_STACK_SIZE */
BUILD_ASSERT(TR_SND_EMBED_PRIO < CONFIG_MAIN_THREAD_PRIORITY && TR_SND_EMBED_PRIO < 0,
             "the sound thread must be cooperative and above the vision (main) thread");
BUILD_ASSERT(IS_ENABLED(CONFIG_FPU_SHARING),
             "the vision thread uses the FPU; the synth's MVE code may too");

static void snd_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	(void)alp_init();
	printk("[snd] Trace Runner game sound inside hp_vision (GAME, embedded), %u Hz, volume %u, "
	       "prio %d\n",
	       RATE,
	       TR_SND_VOLUME,
	       TR_SND_EMBED_PRIO);
	(void)run_game();
}
K_THREAD_DEFINE(tr_snd_thread,
                TR_SND_EMBED_STACK,
                snd_thread,
                NULL,
                NULL,
                NULL,
                TR_SND_EMBED_PRIO,
                0,
                0);
#else
int main(void)
{
	(void)alp_init();
	printk("[snd] Trace Runner sound (%s), %u Hz, volume %u\n",
	       TR_SND_TEST ? "TEST" : "GAME",
	       RATE,
	       TR_SND_VOLUME);
#if TR_SND_TEST
	return run_test();
#else
	return run_game();
#endif
}
#endif

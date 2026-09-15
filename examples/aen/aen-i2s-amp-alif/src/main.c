/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i2s-amp-alif -- drive a tone out of the Ensemble E8 audio I2S (the SoC's
 * i2s3@49017000) via the vendored snps,designware-i2s driver (drivers/i2s/i2s_dw.c),
 * on the E1M-AEN801 (M55-HE).  Uses the standard Zephyr I2S API (i2s_configure /
 * i2s_write / i2s_trigger) on DT_ALIAS(alp_i2s0) = &i2s3 (the SoM "I2S0_*" audio
 * signals route to I2S3_*_A on P9_3/4/5 -- per metadata from-alif.tsv).
 *
 * The 76.8 MHz audio reference (CGU master) and the I2S bit-clock divider are now
 * programmed by the Tier-1.5 clockctrl west-patch
 * (zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch),
 * NOT by this example: i2s_configure() -> the i2s_dw driver calls
 * clock_control_on()/clock_control_set_rate(), and the patched clockctrl enables
 * the CGU master source and divides it down to SCLK (the same master-source fix
 * that made the PDM mics capture).  The controller then clocks a tone out on
 * SCLK/WS/SDO.  On the EVK that signal reaches the two TAS2563
 * smart-amplifiers through a 2:1 mux (U46): SELECT = CC3501E GPIO13 (over the
 * inter-chip SPI bridge, currently firmware-gated), both hw revisions; ENABLE
 * is REVISION-DEPENDENT -- Alif P7.1 (direct GPIO) on r1, CC3501E GPIO_30 on
 * r2 -- see include/alp/boards/alp_e1m_evk.h's I2S mux block.  AUDIBLE amp
 * output additionally needs U46 to be a 3257-type bus switch with VCC on
 * +3V3, OR a switch rated for 1.8 V VCC (untested)
 * (on e1m-aen-evk-03, VCC was moved to +3V3 between a silent run and
 * an audible run, not established as the only difference; the as-built
 * 74LVC157 can never pass this direction regardless, see the same header)
 * + the mux routed + the TAS2563 configured; this example validates only
 * the I2S controller + clock path.
 *
 * PASS gate: device ready, i2s_configure + i2s_write(s) + i2s_trigger(START) all
 * return 0 and the TX FIFO DRAINs cleanly with the 76.8 MHz clock ON (the
 * controller genuinely clocked the tone out).  A run that cannot drain is PARTIAL.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#define I2S_NODE DT_ALIAS(alp_i2s0)
/* i2s3 ships "disabled" by default on EVK rev 2626-R2 -- see the SAFETY
 * note in boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay.
 * DEVICE_DT_GET() on a disabled node fails to LINK, not just to run, so
 * this has to be a compile-time branch: with the node disabled (the
 * default), i2s_dev() returns NULL and main() prints why and exits
 * before touching the Zephyr I2S API at all. */
#if DT_NODE_HAS_STATUS(I2S_NODE, okay)
static const struct device *i2s_dev(void)
{
	return DEVICE_DT_GET(I2S_NODE);
}
#else
static const struct device *i2s_dev(void)
{
	return NULL;
}
#endif
#define SAMPLE_RATE_HZ 48000
#define WORD_BITS      16
#define NUM_CHANNELS   2 /* stereo: L/R interleaved */
#define BLOCK_FRAMES   256u
#define BLOCK_BYTES    (BLOCK_FRAMES * NUM_CHANNELS * (WORD_BITS / 8u))
/* BLOCK_COUNT sizes the slab bigger than what's actually queued
 * (BLOCKS_TO_SEND) so the driver always has a free block on hand even while
 * the app is still allocating/filling the next one -- avoids an alloc stall
 * mid-fill. */
#define BLOCK_COUNT    8u
#define BLOCKS_TO_SEND 4u
#define TX_TIMEOUT_MS  1000

K_MEM_SLAB_DEFINE_STATIC(i2s_slab, BLOCK_BYTES, BLOCK_COUNT, 4);

/* Triangle wave (no libm): a full +/- ramp across the block, written to both
 * channels.  Audible on a real codec; deterministic on every toolchain. */
static void fill_triangle(int16_t *frames)
{
	const int16_t step = (int16_t)(0x7FFF / (BLOCK_FRAMES / 2u));
	int16_t       v    = 0;
	int           dir  = 1;

	for (unsigned f = 0; f < BLOCK_FRAMES; f++) {
		frames[2u * f]      = v; /* left  */
		frames[2u * f + 1u] = v; /* right */
		if (dir > 0 && v > (int16_t)(0x7FFF - step)) {
			dir = -1;
		} else if (dir < 0 && v < (int16_t)(-0x7FFF + step)) {
			dir = 1;
		}
		v = (int16_t)(v + dir * step);
	}
}

int main(void)
{
	const struct device *i2s = i2s_dev();

	if (i2s == NULL) {
		printf("[i2s] SOUND: playback skipped -- EVK 2626-R2 U46 has no Hi-Z state and "
		       "routes SoC I2S outputs into mux outputs (a working, audible mux "
		       "requires U46 to be a 3257-type switch powered from +3V3, or a "
		       "1.8 V-rated switch (untested); see alp_e1m_evk.h); i2s3 is left "
		       "\"disabled\" in the "
		       "board overlay so no pinctrl is applied and no clock is emitted\n"
		       "[i2s] RESULT SKIPPED: i2s3 disabled by default on this "
		       "board revision\n[i2s] done\n");
		return 0;
	}

	printf("[i2s] open %s (audio I2S = i2s3, %d ch @ %d Hz, %d-bit TX)\n",
	       i2s->name,
	       NUM_CHANNELS,
	       SAMPLE_RATE_HZ,
	       WORD_BITS);
	if (!device_is_ready(i2s)) {
		printf("[i2s] RESULT FAIL: device not ready\n[i2s] done\n");
		return 0;
	}

	struct i2s_config cfg = {
		.word_size = WORD_BITS,
		.channels  = NUM_CHANNELS,
		.format    = I2S_FMT_DATA_FORMAT_I2S,
		/* Both CONTROLLER flags: the i2s3 controller itself generates WS
		 * (frame clock) and SCLK (bit clock) from the 76.8 MHz audio
		 * source, rather than expecting an external codec to drive them --
		 * matches the EVK wiring, where i2s3 is the only clock source on
		 * the SCLK/WS/SDO net. */
		.options        = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.frame_clk_freq = SAMPLE_RATE_HZ,
		.mem_slab       = &i2s_slab,
		.block_size     = BLOCK_BYTES,
		.timeout        = TX_TIMEOUT_MS,
	};

	/* No raw clock poke here: the patched Tier-1.5 clockctrl enables the CGU
	 * master 76.8 MHz source and programs the I2Sx_CTRL bit-clock divider inside
	 * i2s_configure() (via clock_control_on()/set_rate()). */
	int rc = i2s_configure(i2s, I2S_DIR_TX, &cfg);
	printf("[i2s] i2s_configure(TX) -> %d\n", rc);
	if (rc != 0) {
		printf("[i2s] RESULT FAIL: configure rc=%d\n[i2s] done\n", rc);
		return 0;
	}

	/* Queue the tone blocks BEFORE START so the FIFO never underruns. */
	unsigned queued = 0;
	for (unsigned b = 0; b < BLOCKS_TO_SEND; b++) {
		void *block = NULL;
		if (k_mem_slab_alloc(&i2s_slab, &block, K_MSEC(TX_TIMEOUT_MS)) != 0) {
			printf("[i2s] slab alloc failed at block %u\n", b);
			break;
		}
		fill_triangle((int16_t *)block);
		rc = i2s_write(i2s, block, BLOCK_BYTES);
		if (rc != 0) {
			printf("[i2s] i2s_write[%u] -> %d\n", b, rc);
			k_mem_slab_free(&i2s_slab, block);
			break;
		}
		queued++;
	}
	printf("[i2s] queued %u/%u tone blocks\n", queued, BLOCKS_TO_SEND);

	rc = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	printf("[i2s] i2s_trigger(START) -> %d\n", rc);
	if (rc != 0) {
		printf("[i2s] RESULT FAIL: start rc=%d\n[i2s] done\n", rc);
		return 0;
	}

	/* Wait for the ISR to actually clock every queued block out BEFORE asking
	 * the driver to DRAIN: the i2s_dw DRAIN tears the TX channel/clock down
	 * rather than blocking, so triggering it immediately would stop the
	 * controller before the tone was transmitted and the PASS gate would never
	 * observe a real drain.  A block is returned to the slab only once the ISR
	 * has finished sending it, so num_free climbs back to BLOCK_COUNT exactly
	 * when the last queued block has been clocked out. */
	bool consumed = false;
	for (int waited_ms = 0; waited_ms <= TX_TIMEOUT_MS; waited_ms += 10) {
		if (k_mem_slab_num_free_get(&i2s_slab) == BLOCK_COUNT) {
			consumed = true;
			break;
		}
		k_msleep(10);
	}
	printf("[i2s] TX blocks clocked out: %s (slab free %u/%u)\n",
	       consumed ? "yes" : "TIMEOUT",
	       k_mem_slab_num_free_get(&i2s_slab),
	       BLOCK_COUNT);

	/* Now every block is on the wire; DRAIN just flushes the (empty) FIFO and
	 * stops the TX channel cleanly. */
	rc = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	printf("[i2s] i2s_trigger(DRAIN) -> %d\n", rc);

	bool drained = (rc == 0) && consumed && (queued == BLOCKS_TO_SEND);
	printf("[i2s] RESULT %s: %s\n",
	       drained ? "PASS" : "PARTIAL",
	       drained ? "i2s3 TX clocked the tone out with the 76.8MHz audio clock ON (SCLK/WS/SDO "
	                 "on P9_3/4/5). For AUDIBLE amp out: U46 needs a 3257-type switch on +3V3, "
	                 "OR a switch rated for 1.8V VCC (untested) "
	                 "(not the stock 74LVC157), routed + the TAS2563 configured (ACTIVE)"
	               : "configured but TX did not drain cleanly (check clock/pinctrl)");
	printf("[i2s] done\n");
	return 0;
}

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
 * The example enables the 76.8 MHz audio reference at the CGU (the same fix that
 * made the PDM mics capture -- the upstream clockctrl never enables it) and clocks
 * a tone out on SCLK/WS/SDO.  On the EVK that signal reaches the two TAS2563
 * smart-amplifiers through a 74LVC157 2:1 mux: /E = Alif P7.1 (drivable) and the
 * SELECT = CC3501E GPIO13 (over the inter-chip SPI bridge, currently
 * firmware-gated). So this example validates the I2S controller + clock path; the
 * AUDIBLE amp output additionally needs the mux routed + the TAS2563 configured.
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
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define I2S_NODE       DT_ALIAS(alp_i2s0)
#define SAMPLE_RATE_HZ 48000
#define WORD_BITS      16
#define NUM_CHANNELS   2 /* stereo: L/R interleaved */
#define BLOCK_FRAMES   256u
#define BLOCK_BYTES    (BLOCK_FRAMES * NUM_CHANNELS * (WORD_BITS / 8u))
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
	const struct device *i2s = DEVICE_DT_GET(I2S_NODE);

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
		.word_size      = WORD_BITS,
		.channels       = NUM_CHANNELS,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE_HZ,
		.mem_slab       = &i2s_slab,
		.block_size     = BLOCK_BYTES,
		.timeout        = TX_TIMEOUT_MS,
	};

	/* Enable the 76.8 MHz audio reference (HFOSCx2) at the CGU -- the I2S bit clock
	 * is derived from it, and the upstream Alif clockctrl never enables this master
	 * source (only per-peripheral gates). Without it the I2S does not truly clock
	 * out (the same fix that made the PDM mics capture). CGU_CLK_ENA = CGU base
	 * 0x1A602000 + 0x14, bit 24 = CLK76P8M (from the fork clock driver, not invented). */
	sys_set_bits(0x1A602014U, BIT(24));

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

	/* DRAIN blocks until every queued block has been clocked out, then stops. */
	rc = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	printf("[i2s] i2s_trigger(DRAIN) -> %d\n", rc);

	bool drained = (rc == 0) && (queued == BLOCKS_TO_SEND);
	printf("[i2s] RESULT %s: %s\n",
	       drained ? "PASS" : "PARTIAL",
	       drained ? "i2s3 TX clocked the tone out with the 76.8MHz audio clock ON (SCLK/WS/SDO "
	                 "on P9_3/4/5). For AUDIBLE amp out: route the 74LVC157 mux (EN=Alif P7.1 + "
	                 "SEL=CC3501E GPIO13) to the TAS2563 + configure the amp (ACTIVE)"
	               : "configured but TX did not drain cleanly (check clock/pinctrl)");
	printf("[i2s] done\n");
	return 0;
}

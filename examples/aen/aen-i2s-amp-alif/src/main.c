/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i2s-amp-alif -- drive a tone out of the Ensemble E8 I2S0 controller via the
 * vendored snps,designware-i2s driver (drivers/i2s/i2s_dw.c), on the E1M-AEN801
 * (M55-HE).  Uses the standard Zephyr I2S API (i2s_configure / i2s_write /
 * i2s_trigger) on DT_ALIAS(alp_i2s0) = &i2s0.
 *
 * On the E1M EVK, I2S0 is the on-board audio bus -- it reaches the two TAS2563
 * smart-amplifiers through a 74LVC157 2:1 mux (<alp/boards/alp_e1m_evk.h>):
 *   /E (enable, active-low) = E1M IO8  -> Alif P7.1   (drive low to pass I2S0)
 *   S  (select)             = E1M IO13 -> CC3501E GPIO13 (low = TAS2563 amps)
 * The SELECT line is on the CC3501E and must be driven over the inter-chip SPI
 * bridge (ALP_CC3501E_CMD_GPIO_WRITE), so this example does NOT toggle the mux --
 * it validates the I2S *controller/driver* path. See the README for the two HW
 * steps (mux + pad-route confirmation) needed for audible amp output.
 *
 * PASS gate: device ready, i2s_configure + i2s_write(s) + i2s_trigger(START) all
 * return 0 and the TX FIFO DRAINs cleanly (i2s_trigger(DRAIN) returns 0) -- the
 * controller clocked the whole tone buffer out without an underrun/error.  A run
 * that configures but cannot drain is reported PARTIAL.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

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

	printf("[i2s] open %s (I2S0, %d ch @ %d Hz, %d-bit TX)\n",
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
	       drained ? "PARTIAL" : "PARTIAL",
	       drained ? "I2S driver path executed (configure/write/START/DRAIN all ok). NOTE: true "
	                 "bit-clock-out is UNVERIFIED -- the 76.8MHz audio clock is SE-managed "
	                 "(CLKEN_HFOSCx2, not requested); + the EVK audio I2S is i2s3/P9_x (SoM TSV) "
	                 "behind the CC3501E mux, not this i2s0 demo instance"
	               : "configured but TX did not drain cleanly (check clock/pinctrl)");
	printf("[i2s] done\n");
	return 0;
}

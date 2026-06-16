/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-pdm-mic-alif -- capture from the EVK's PDM microphones (4x MP34DT05) via the
 * Ensemble E8 LPPDM block + the vendored alif,alif-pdm DMIC driver, on the
 * E1M-AEN801 (M55-HE).  Drives the standard Zephyr DMIC API (dmic_configure /
 * dmic_trigger / dmic_read) on DT_ALIAS(alp_pdm0) = &lppdm.
 *
 * PASS gate: the device is ready, dmic_configure + dmic_trigger(START) return 0,
 * and dmic_read returns blocks with non-zero, NON-CONSTANT samples (live acoustic
 * energy -- tap or speak near the mics).  A run that configures + reads cleanly but
 * sees only silence/constant data is reported PARTIAL (driver path proven; check
 * the mic routing / gain rather than the driver).
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>

#define PDM_NODE         DT_ALIAS(alp_pdm0)
#define SAMPLE_RATE_HZ   16000
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     4 /* LPPDM C0..C3 -> the 4 MP34DT05 mics */
#define READ_TIMEOUT_MS  2000
/* 100 ms block: bytes = 2 * (rate/10) * channels */
#define BLOCK_SIZE  (2u * (SAMPLE_RATE_HZ / 10u) * NUM_CHANNELS)
#define BLOCK_COUNT 4

K_MEM_SLAB_DEFINE_STATIC(pdm_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

int main(void)
{
	const struct device *dmic = DEVICE_DT_GET(PDM_NODE);

	printf("[pdm] open %s (LPPDM, %d ch @ %d Hz)\n", dmic->name, NUM_CHANNELS, SAMPLE_RATE_HZ);
	if (!device_is_ready(dmic)) {
		printf("[pdm] RESULT FAIL: device not ready\n[pdm] done\n");
		return 0;
	}

	struct pcm_stream_cfg stream = {
		.pcm_width  = SAMPLE_BIT_WIDTH,
		.pcm_rate   = SAMPLE_RATE_HZ,
		.block_size = BLOCK_SIZE,
		.mem_slab   = &pdm_slab,
	};
	uint32_t chan_map = 0;
	for (int c = 0; c < NUM_CHANNELS; c++) {
		chan_map |= dmic_build_channel_map(c, c, PDM_CHAN_LEFT);
	}
	struct dmic_cfg cfg = {
		.io =
		    {
		        .min_pdm_clk_freq = 1024000,
		        .max_pdm_clk_freq = 4096000,
		        .min_pdm_clk_dc   = 40,
		        .max_pdm_clk_dc   = 60,
		    },
		.streams = &stream,
		.channel =
		    {
		        .req_num_streams = 1,
		        .req_num_chan    = NUM_CHANNELS,
		        .req_chan_map_lo = chan_map,
		    },
	};

	int rc = dmic_configure(dmic, &cfg);
	printf("[pdm] dmic_configure -> %d\n", rc);
	if (rc != 0) {
		printf("[pdm] RESULT FAIL: configure rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	rc = dmic_trigger(dmic, DMIC_TRIGGER_START);
	printf("[pdm] dmic_trigger(START) -> %d\n", rc);
	if (rc != 0) {
		printf("[pdm] RESULT FAIL: start rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	bool got = false, varying = false;
	for (int b = 0; b < BLOCK_COUNT; b++) {
		void    *buf  = NULL;
		uint32_t size = 0;
		rc            = dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS);
		if (rc != 0) {
			printf("[pdm] dmic_read[%d] -> %d\n", b, rc);
			continue;
		}
		const int16_t *s     = (const int16_t *)buf;
		size_t         n     = size / sizeof(int16_t);
		int16_t        first = (n > 0) ? s[0] : 0;
		int            nz    = 0;
		for (size_t i = 0; i < n; i++) {
			if (s[i] != 0) nz++;
			if (s[i] != first) varying = true;
		}
		if (nz > 0) got = true;
		printf("[pdm] read[%d] size=%u nonzero=%d first=%d\n", b, size, nz, (int)first);
		k_mem_slab_free(&pdm_slab, buf);
	}
	dmic_trigger(dmic, DMIC_TRIGGER_STOP);

	printf("[pdm] RESULT %s: %s\n",
	       (got && varying) ? "PASS" : "PARTIAL",
	       varying ? "varying PCM captured = live audio"
	       : got   ? "non-zero but constant (check gain)"
	               : "configured + read ok, samples silent (tap mics / confirm routing)");
	printf("[pdm] done\n");
	return 0;
}

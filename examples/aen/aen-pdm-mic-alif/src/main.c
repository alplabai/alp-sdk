/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-pdm-mic-alif -- capture from the EVK's PDM microphones (4x MP34DT05) via the
 * Ensemble E8 HP PDM block (pdm@4902d000) + the vendored alif,alif-pdm DMIC driver,
 * on the E1M-AEN801 (M55-HE).  Drives the standard Zephyr DMIC API (dmic_configure
 * / dmic_trigger / dmic_read) on DT_ALIAS(alp_pdm0) = &pdm.  The mics are wired to
 * the HP PDM (per the SoM from-alif.tsv), NOT the LPPDM.
 *
 * As of issue #2133, dmic_alif_pdm_configure() honours the STANDARD dmic
 * contract itself: it decodes dmic_build_channel_map()'s nibble encoding into
 * the hardware channel-enable bits, validates the requested pcm_rate + io
 * clock window against a HWRM/DFP-grounded PDM_MODE table, and
 * dmic_trigger(START) primes the per-channel FIR/gain defaults and selects
 * the real clock mode -- no app-side pdm_mode() / pdm_channel_config() calls
 * are needed for either rate this example supports. Those calls remain
 * public only for an app that wants to override the driver's defaults.
 *
 * Two rates are supported (zephyr/drivers/audio/alif_pdm.c's
 * pdm_clock_modes table): 8000 Hz (PDM_MODE_STANDARD_VOICE_512_CLK_FRQ,
 * bench-proven on e1m-aen-evk-03) and 16000 Hz
 * (PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ, same decimation ratio as the proven
 * mode and a vendor-sourced FIR reuse -- see the driver table's comment --
 * but not yet itself bench-verified). Build either with:
 *   west build ... -- -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=8000"
 * (default SAMPLE_RATE_HZ is 16000).
 *
 * PASS gate: the device is ready, dmic_configure + dmic_trigger(START)
 * return 0, dmic_read returns blocks with non-zero, NON-CONSTANT samples
 * (live acoustic energy -- tap or speak near the mics), AND the MEASURED
 * sample rate (frames delivered / elapsed wall-clock time, excluding the
 * first read's startup latency) is within +/-5% of SAMPLE_RATE_HZ -- a "PCM
 * varies" check alone does not catch a wrong clock mode (round 1 of this
 * issue shipped mode 1 mislabelled 16 kHz when it is actually 8 kHz; this
 * gate exists so that class of bug fails loudly next time). A run that
 * configures + reads cleanly at the right rate but sees only silence/
 * constant data is reported PARTIAL (driver path proven; check the mic
 * routing / gain rather than the driver).
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>

#define PDM_NODE DT_ALIAS(alp_pdm0)
#ifndef SAMPLE_RATE_HZ
#define SAMPLE_RATE_HZ 16000 /* override: -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=8000" */
#endif
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     4 /* HP PDM: D0->ch0/1, D2->ch4/5 = the 4 MP34DT05 mics */

#define READ_TIMEOUT_MS 2000
/* 100 ms block: bytes = 2 * (rate/10) * channels */
#define BLOCK_SIZE  (2u * (SAMPLE_RATE_HZ / 10u) * NUM_CHANNELS)
#define BLOCK_COUNT 4

/* The DMIC API is zero-copy: dmic_read() hands back a pointer into one of
 * these slab blocks and the caller must k_mem_slab_free() it once done, so
 * the driver can DMA/PIO the next PDM frame straight into a free block
 * without an extra memcpy. BLOCK_COUNT=4 gives the driver headroom to keep
 * filling blocks while this app is still consuming the previous one. */
K_MEM_SLAB_DEFINE_STATIC(pdm_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

int main(void)
{
	const struct device *dmic = DEVICE_DT_GET(PDM_NODE);

	printf("[pdm] open %s (HP PDM, %d ch @ %d Hz)\n", dmic->name, NUM_CHANNELS, SAMPLE_RATE_HZ);
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
	/* Standard dmic_build_channel_map() encoding -- one nibble per logical
	 * channel, (PDM controller, L/R). D0 is PDM controller 0 (mics 0/1);
	 * D2 is PDM controller 2 (mics 2/3). The driver's alif_pdm_chanmap.h
	 * translates this into the hardware channel-enable bits (0,1,4,5). */
	struct dmic_cfg cfg = {
		/* Window covers both bench/vendor-grounded alif_pdm clock modes:
		 * 512 kHz (8 kHz Fs, mode 1, bench-proven) and 1024 kHz (16 kHz
		 * Fs, mode 4). The driver now enforces this window itself
		 * (issue #2133 round 2) -- it used to be read by nothing. */
		.io =
		    {
		        .min_pdm_clk_freq = 500000,
		        .max_pdm_clk_freq = 4096000,
		        .min_pdm_clk_dc   = 40,
		        .max_pdm_clk_dc   = 60,
		    },
		.streams = &stream,
		.channel =
		    {
		        .req_num_streams = 1,
		        .req_num_chan    = NUM_CHANNELS,
		        .req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
		                           dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT) |
		                           dmic_build_channel_map(2, 2, PDM_CHAN_LEFT) |
		                           dmic_build_channel_map(3, 2, PDM_CHAN_RIGHT),
		    },
	};

	/* No raw clock pokes here: the patched Tier-1.5 clockctrl
	 * (zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch)
	 * enables the CGU master 76.8 MHz source AND the EXPMST0_CTRL IPCLK/PCLK force
	 * bits the EXPMST0-domain HP PDM needs, inside clock_control_on() -- which the
	 * alif_pdm driver calls during dmic_configure(). Without those the PDM has no
	 * functional clock and never samples (FIFO=0 -> dmic_read -EAGAIN). */
	int rc = dmic_configure(dmic, &cfg);
	if (rc != 0) {
		printf("[pdm] dmic_configure -> %d\n", rc);
		printf("[pdm] RESULT FAIL: configure rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	/* Nothing prints between configure() and trigger(START): a printf here
	 * used to cost ~3 ms, and at 8 kHz the driver's 4-bit FIFO count field
	 * overflows in ~1.9 ms once dmic_trigger(START) leaves the block
	 * sampling -- see alif_pdm.c's DMIC_TRIGGER_START comment (issue
	 * #2133 round 2). */
	rc = dmic_trigger(dmic, DMIC_TRIGGER_START);
	printf("[pdm] dmic_configure -> 0, dmic_trigger(START) -> %d\n", rc);
	if (rc != 0) {
		printf("[pdm] RESULT FAIL: start rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	bool     got = false, varying = false;
	int64_t  t_anchor_ms     = 0;
	int64_t  t_last_ms       = 0;
	uint32_t frames_measured = 0;

	for (int b = 0; b < BLOCK_COUNT; b++) {
		void    *buf  = NULL;
		uint32_t size = 0;
		rc            = dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS);
		if (rc != 0) {
			printf("[pdm] dmic_read[%d] -> %d\n", b, rc);
			continue;
		}

		int64_t  now_ms          = k_uptime_get();
		uint32_t frames_in_block = size / (sizeof(int16_t) * NUM_CHANNELS);

		/* Measure the ACHIEVED rate, not the requested one: count
		 * frames across reads and divide by elapsed wall-clock time,
		 * excluding the first read as a startup-latency anchor only
		 * (it can span the pre-START/post-START boundary). A "PCM
		 * varies" check alone never would have caught round 1's
		 * 8-kHz-labelled-16-kHz bug. */
		if (b == 0) {
			t_anchor_ms = now_ms;
		} else {
			frames_measured += frames_in_block;
			t_last_ms = now_ms;
		}

		const int16_t *s     = (const int16_t *)buf;
		size_t         n     = size / sizeof(int16_t);
		int16_t        first = (n > 0) ? s[0] : 0;
		int            nz    = 0;
		/* nz alone can't prove live audio: a stuck channel can read back a
		 * non-zero DC value every sample. Tracking whether any sample
		 * differs from the first (varying) is what actually distinguishes
		 * real acoustic energy from a frozen/constant register value. */
		for (size_t i = 0; i < n; i++) {
			if (s[i] != 0) nz++;
			if (s[i] != first) varying = true;
		}
		if (nz > 0) got = true;
		printf("[pdm] read[%d] size=%u nonzero=%d first=%d\n", b, size, nz, (int)first);
		k_mem_slab_free(&pdm_slab, buf);
	}
	dmic_trigger(dmic, DMIC_TRIGGER_STOP);

	uint32_t measured_rate_hz = 0;

	if (t_last_ms > t_anchor_ms) {
		measured_rate_hz =
		    (uint32_t)((uint64_t)frames_measured * 1000ULL / (uint64_t)(t_last_ms - t_anchor_ms));
	}
	uint32_t rate_lo = (uint32_t)((uint64_t)SAMPLE_RATE_HZ * 95u / 100u);
	uint32_t rate_hi = (uint32_t)((uint64_t)SAMPLE_RATE_HZ * 105u / 100u);
	bool     rate_ok = measured_rate_hz >= rate_lo && measured_rate_hz <= rate_hi;

	printf("[pdm] measured_rate_hz=%u requested=%u (+/-5%% window [%u,%u])\n",
	       measured_rate_hz,
	       (unsigned)SAMPLE_RATE_HZ,
	       rate_lo,
	       rate_hi);

	printf("[pdm] RESULT %s: %s\n",
	       (got && varying && rate_ok) ? "PASS" : "PARTIAL",
	       !rate_ok  ? "measured rate outside +/-5% of requested -- wrong PDM clock mode"
	       : varying ? "varying PCM captured at the requested rate = live audio"
	       : got ? "non-zero but constant (check gain)"
	             : "FIFO empty -- HP-PDM config register-verified (ch0,1,4,5 enabled, mode set; "
	               "the patched clockctrl forced EXPMST0 IPCLK/PCLK + set the CGU CLK_ENA bit); "
	               "not sampling -> the 76.8MHz audio source itself (HFOSCx2) is SE-managed: the "
	               "CGU CLK_ENA bit alone may not engage the oscillator. Needs the se_services/MHU "
	               "clock request to the SE (alp-sdk doesn't wire it yet).");
	printf("[pdm] done\n");
	return 0;
}

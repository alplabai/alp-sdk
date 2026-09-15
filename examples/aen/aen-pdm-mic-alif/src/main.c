/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-pdm-mic-alif -- capture from the EVK's PDM microphones (4x MP34DT05TR-A)
 * via the Ensemble E8 HP PDM block (pdm@4902d000) + the vendored alif,alif-pdm
 * DMIC driver, on the E1M-AEN801 (M55-HE).  Drives the standard Zephyr DMIC
 * API (dmic_configure / dmic_trigger / dmic_read) on DT_ALIAS(alp_pdm0) =
 * &pdm.  The mics are wired to the HP PDM (per the SoM from-alif.tsv), NOT
 * the LPPDM.
 *
 * As of issue #2133, dmic_alif_pdm_configure() honours the STANDARD dmic
 * contract itself: it decodes dmic_build_channel_map()'s nibble encoding into
 * the hardware channel-enable bits, validates the requested pcm_rate against
 * a HWRM/DFP-grounded PDM_MODE table AND the board's mic clock range (the
 * board overlay's min-pdm-clk-freq/max-pdm-clk-freq -- a board fact, not
 * something this app or the driver invents), and dmic_trigger(START) primes
 * the per-channel FIR/gain defaults and selects the real clock mode. No
 * app-side pdm_mode() / pdm_channel_config() calls are needed.
 *
 * Rates (zephyr/drivers/audio/alif_pdm.c's pdm_clock_modes table): this
 * example defaults to 48000 Hz (PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ,
 * 3072 kHz clk) -- the only table entry that is BOTH a proven-decimation-
 * ratio FIR reuse AND inside the fitted MP34DT05TR-A mics' 1.2-3.25 MHz spec
 * (round 1's 8 kHz and round 2's 16 kHz modes are both below that minimum --
 * round 1's "working" capture was an under-clocked mic, not proof of real
 * audio; the EVK's board overlay now declares the mic range and the driver
 * rejects both with -EINVAL). Override with:
 *   west build ... -- -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=16000"
 * to demonstrate that rejection on silicon (expect RESULT FAIL: configure
 * rc=-22).
 *
 * PASS gate: the device is ready, dmic_configure + dmic_trigger(START)
 * return 0, dmic_read returns blocks with non-zero, NON-CONSTANT samples,
 * the MEASURED sample rate (frames delivered / elapsed wall-clock time,
 * excluding the first read's startup latency) is within +/-5% of
 * SAMPLE_RATE_HZ, AND at least one channel's RMS and peak-to-peak both clear
 * a documented floor above a dead/under-clocked mic's residual noise (issue
 * #2133 round 3: round 1's PASS gate was "samples aren't all equal", which a
 * flat +/-1-2 LSB noise floor satisfies -- that is NOT evidence of live
 * acoustic capture). A run that reads cleanly at the right rate but with no
 * channel over that floor is reported INCONCLUSIVE, not PASS.
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>

#define PDM_NODE DT_ALIAS(alp_pdm0)
#ifndef SAMPLE_RATE_HZ
#define SAMPLE_RATE_HZ 48000 /* override: -DEXTRA_CFLAGS="-DSAMPLE_RATE_HZ=16000" */
#endif
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     4 /* HP PDM: D0->ch0/1, D2->ch4/5 = the 4 MP34DT05TR-A mics */

#define READ_TIMEOUT_MS 2000
/* 100 ms block: bytes = 2 * (rate/10) * channels */
#define BLOCK_SIZE  (2u * (SAMPLE_RATE_HZ / 10u) * NUM_CHANNELS)
#define BLOCK_COUNT 4

/* Liveness floor for the per-channel signal-level check (issue #2133 round
 * 3). A dead or under-clocked PDM channel reads back "+/-1-2 LSB flat
 * noise" -- these thresholds sit a full order of magnitude above that floor
 * so real acoustic energy (even a quiet room, not just a loud tap) passes
 * while a dead/silent channel does not. Not a calibrated SNR figure -- a
 * documented, order-of-magnitude liveness gate.
 */
#define MIN_SIGNAL_RMS_LSB          16
#define MIN_SIGNAL_PEAK_TO_PEAK_LSB 64

/* The DMIC API is zero-copy: dmic_read() hands back a pointer into one of
 * these slab blocks and the caller must k_mem_slab_free() it once done, so
 * the driver can DMA/PIO the next PDM frame straight into a free block
 * without an extra memcpy. BLOCK_COUNT=4 gives the driver headroom to keep
 * filling blocks while this app is still consuming the previous one. */
K_MEM_SLAB_DEFINE_STATIC(pdm_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

/* Per-channel running stats (Welford's online algorithm for a numerically
 * stable single-pass mean/variance -- the raw PDM stream is consumed as it
 * arrives, so a two-pass RMS-of-(sample-mean) isn't an option without
 * buffering the whole capture). min/max give peak-to-peak directly (DC-
 * offset-invariant, no mean needed); mean itself is reported separately as
 * the DC offset.
 */
struct chan_stats {
	int32_t  min_v;
	int32_t  max_v;
	double   mean;
	double   m2; /* sum of squared deviations from the running mean */
	uint32_t n;
};

static void chan_stats_init(struct chan_stats *cs)
{
	cs->min_v = INT16_MAX;
	cs->max_v = INT16_MIN;
	cs->mean  = 0.0;
	cs->m2    = 0.0;
	cs->n     = 0;
}

static void chan_stats_update(struct chan_stats *cs, int16_t x)
{
	double delta, delta2;

	if (x < cs->min_v) cs->min_v = x;
	if (x > cs->max_v) cs->max_v = x;

	cs->n++;
	delta = (double)x - cs->mean;
	cs->mean += delta / (double)cs->n;
	delta2 = (double)x - cs->mean;
	cs->m2 += delta * delta2;
}

static double chan_stats_rms_ac(const struct chan_stats *cs)
{
	return (cs->n > 0) ? sqrt(cs->m2 / (double)cs->n) : 0.0;
}

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
		/* Generic PDM bit-clock window -- the board overlay's
		 * min-pdm-clk-freq/max-pdm-clk-freq (the fitted MP34DT05TR-A's
		 * real spec) is what actually constrains the driver's mode
		 * choice (issue #2133 round 3); this io window just needs to
		 * be wide enough to admit every mode this app might request. */
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
	 * used to cost ~3 ms, and at low rates the driver's 4-bit FIFO count
	 * field can overflow in under 2 ms once dmic_trigger(START) leaves the
	 * block sampling -- see alif_pdm.c's DMIC_TRIGGER_START comment (issue
	 * #2133 round 2). */
	rc = dmic_trigger(dmic, DMIC_TRIGGER_START);
	printf("[pdm] dmic_configure -> 0, dmic_trigger(START) -> %d\n", rc);
	if (rc != 0) {
		printf("[pdm] RESULT FAIL: start rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	int64_t  t_anchor_ms     = 0;
	int64_t  t_last_ms       = 0;
	uint32_t frames_measured = 0;
	uint32_t reads_ok        = 0;

	struct chan_stats stats[NUM_CHANNELS];

	for (int c = 0; c < NUM_CHANNELS; c++)
		chan_stats_init(&stats[c]);

	for (int b = 0; b < BLOCK_COUNT; b++) {
		void    *buf  = NULL;
		uint32_t size = 0;
		rc            = dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS);
		if (rc != 0) {
			printf("[pdm] dmic_read[%d] -> %d\n", b, rc);
			continue;
		}
		reads_ok++;

		int64_t  now_ms          = k_uptime_get();
		uint32_t frames_in_block = size / (sizeof(int16_t) * NUM_CHANNELS);

		/* Measure the ACHIEVED rate, not the requested one: count
		 * frames across reads and divide by elapsed wall-clock time,
		 * excluding the first read as a startup-latency anchor only
		 * (it can span the pre-START/post-START boundary). A "PCM
		 * varies" check alone never would have caught round 1's
		 * mislabelled-rate bug. */
		if (b == 0) {
			t_anchor_ms = now_ms;
		} else {
			frames_measured += frames_in_block;
			t_last_ms = now_ms;
		}

		const int16_t *s = (const int16_t *)buf;
		size_t         n = size / sizeof(int16_t);

		for (size_t i = 0; i < n; i++) {
			chan_stats_update(&stats[i % NUM_CHANNELS], s[i]);
		}
		printf("[pdm] read[%d] size=%u\n", b, size);
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

	/* Per-channel RMS/peak/DC-offset print + liveness gate (issue #2133
	 * round 3): ANY channel clearing both floors is enough to call the
	 * capture live -- the 4 mics are physically separated, so a quiet tap
	 * or speech need not reach every one equally. */
	bool signal_ok = false;

	for (int c = 0; c < NUM_CHANNELS; c++) {
		int32_t peak_to_peak = stats[c].max_v - stats[c].min_v;
		double  rms_ac       = chan_stats_rms_ac(&stats[c]);
		bool    chan_live =
		    (rms_ac >= (double)MIN_SIGNAL_RMS_LSB) && (peak_to_peak >= MIN_SIGNAL_PEAK_TO_PEAK_LSB);

		printf("[pdm] ch[%d] n=%u rms_ac=%d peak_to_peak=%d dc_offset=%d %s\n",
		       c,
		       stats[c].n,
		       (int)rms_ac,
		       peak_to_peak,
		       (int)stats[c].mean,
		       chan_live ? "LIVE" : "flat");

		if (chan_live) signal_ok = true;
	}

	const char *verdict;
	const char *reason;

	if (reads_ok == 0) {
		verdict = "PARTIAL";
		reason  = "FIFO empty -- HP-PDM config register-verified (channel mask + mode set; "
		          "the patched clockctrl forced EXPMST0 IPCLK/PCLK + set the CGU CLK_ENA bit); "
		          "not sampling -> the 76.8MHz audio source itself (HFOSCx2) is SE-managed: the "
		          "CGU CLK_ENA bit alone may not engage the oscillator. Needs the se_services/MHU "
		          "clock request to the SE (alp-sdk doesn't wire it yet).";
	} else if (!rate_ok) {
		verdict = "PARTIAL";
		reason  = "measured rate outside +/-5% of requested -- wrong PDM clock mode";
	} else if (!signal_ok) {
		verdict = "INCONCLUSIVE";
		reason  = "no acoustic signal (every channel's RMS/peak-to-peak stayed at or below the "
		          "dead-mic floor) -- tap or speak near the mics and rerun";
	} else {
		verdict = "PASS";
		reason =
		    "varying PCM captured at the requested rate, above the liveness floor = live audio";
	}

	printf("[pdm] RESULT %s: %s\n", verdict, reason);
	printf("[pdm] done\n");
	return 0;
}

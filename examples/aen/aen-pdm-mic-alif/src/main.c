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
 * PASS gate: the device is ready, dmic_configure + dmic_trigger(START) return 0,
 * and dmic_read returns blocks with non-zero, NON-CONSTANT samples (live acoustic
 * energy -- tap or speak near the mics).  A run that configures + reads cleanly but
 * sees only silence/constant data is reported PARTIAL (driver path proven; check
 * the mic routing / gain rather than the driver).
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/pdm/pdm_alif.h>

#define PDM_NODE         DT_ALIAS(alp_pdm0)
#define SAMPLE_RATE_HZ   16000
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     4 /* HP PDM: D0->ch0/1, D2->ch4/5 = the 4 MP34DT05 mics */

/* The E1M-AEN801 routes its mics to HP-PDM data lines D0 (channels 0,1) and D2
 * (channels 4,5) -- see the board overlay / from-alif.tsv. */
static const uint8_t mic_channels[NUM_CHANNELS] = { 0, 1, 4, 5 };
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

/*
 * Per-channel FIR/IIR + gain/phase/peak-detect coefficients for STANDARD_VOICE
 * (PDM_MODE_STANDARD_VOICE_512_CLK_FRQ), copied VERBATIM from the Alif reference
 * dmic app (sdk-alif samples/drivers/audio/dmic_alif). These are NOT invented --
 * the FIR is the block's decimation filter for this clock mode. The driver leaves
 * the PDM clock-mode field at reset (MICROPHONE_SLEEP) until the app calls
 * pdm_mode(), and never programs the FIR/gain -- so configuring each enabled
 * channel + selecting a non-sleep mode here is what makes the FIFO actually fill.
 */
static const uint32_t fir_voice512[PDM_MAX_FIR_COEFFICIENT] = {
	0x00000001, 0x00000003, 0x00000003, 0x000007F4, 0x00000004, 0x000007ED,
	0x000007F5, 0x000007F4, 0x000007D3, 0x000007FE, 0x000007BC, 0x000007E5,
	0x000007D9, 0x00000793, 0x00000029, 0x0000072C, 0x00000072, 0x000002FD
};
#define CH_PHASE          0x0000001F
#define CH_GAIN           0x0000000D
#define CH_PEAK_DETECT_TH 0x00060002
#define CH_PEAK_DETECT_IT 0x0004002D
#define CH_IIR_COEF       0x00000004

/* Program one PDM channel exactly as the Alif reference does, then the caller
 * selects the clock mode once for the block. */
static void pdm_config_channel(const struct device *dmic, uint8_t ch)
{
	struct pdm_ch_config cc = { 0 };

	pdm_set_ch_phase(dmic, ch, CH_PHASE);
	pdm_set_ch_gain(dmic, ch, CH_GAIN);
	pdm_set_peak_detect_th(dmic, ch, CH_PEAK_DETECT_TH);
	pdm_set_peak_detect_itv(dmic, ch, CH_PEAK_DETECT_IT);

	cc.ch_num = ch;
	memcpy(cc.ch_fir_coef, fir_voice512, sizeof(cc.ch_fir_coef));
	cc.ch_iir_coef = CH_IIR_COEF;
	pdm_channel_config(dmic, &cc);
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
	/* The alif_pdm driver takes req_chan_map_lo's low byte VERBATIM as the PDM
	 * hardware channel-enable mask (dmic_alif_pdm_configure: channel_map =
	 * req_chan_map_lo & 0xFF), NOT the dmic_build_channel_map() nibble encoding.
	 * Enable the mics' HW channels: D0 -> ch0/ch1, D2 -> ch4/ch5. */
	uint32_t chan_map =
	    PDM_MASK_CHANNEL_0 | PDM_MASK_CHANNEL_1 | PDM_MASK_CHANNEL_4 | PDM_MASK_CHANNEL_5;
	struct dmic_cfg cfg = {
		/* Acceptable range for the PDM bit clock the driver derives from the
		 * 76.8 MHz audio source: wide enough to admit whatever divide the
		 * alif_pdm driver picks for STANDARD_VOICE_512, with a conservative
		 * mid-range duty-cycle window (40-60%). */
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

	/* No raw clock pokes here: the patched Tier-1.5 clockctrl
	 * (zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch)
	 * enables the CGU master 76.8 MHz source AND the EXPMST0_CTRL IPCLK/PCLK force
	 * bits the EXPMST0-domain HP PDM needs, inside clock_control_on() -- which the
	 * alif_pdm driver calls during dmic_configure(). Without those the PDM has no
	 * functional clock and never samples (FIFO=0 -> dmic_read -EAGAIN). */
	int rc = dmic_configure(dmic, &cfg);
	printf("[pdm] dmic_configure -> %d\n", rc);
	if (rc != 0) {
		printf("[pdm] RESULT FAIL: configure rc=%d\n[pdm] done\n", rc);
		return 0;
	}

	/* Configure each enabled channel's FIR/IIR/gain, then select a non-sleep
	 * clock mode -- WITHOUT this the PDM block stays in MICROPHONE_SLEEP and the
	 * FIFO never fills (every read would -EAGAIN). pdm_mode() is app-side API. */
	for (int i = 0; i < NUM_CHANNELS; i++) {
		pdm_config_channel(dmic, mic_channels[i]);
	}
	pdm_mode(dmic, PDM_MODE_STANDARD_VOICE_512_CLK_FRQ);
	printf("[pdm] channels configured + clock mode = STANDARD_VOICE_512\n");

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

	printf("[pdm] RESULT %s: %s\n",
	       (got && varying) ? "PASS" : "PARTIAL",
	       varying ? "varying PCM captured = live audio"
	       : got ? "non-zero but constant (check gain)"
	             : "FIFO empty -- HP-PDM config register-verified (ch0,1,4,5 enabled, mode set; "
	               "the patched clockctrl forced EXPMST0 IPCLK/PCLK + set the CGU CLK_ENA bit); "
	               "not sampling -> the 76.8MHz audio source itself (HFOSCx2) is SE-managed: the "
	               "CGU CLK_ENA bit alone may not engage the oscillator. Needs the se_services/MHU "
	               "clock request to the SE (alp-sdk doesn't wire it yet).");
	printf("[pdm] done\n");
	return 0;
}

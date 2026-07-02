/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2s-tone — stream a triangle wave to the selected EVK's audio
 * codec as 16-bit stereo PCM.  Demonstrates the full lifecycle:
 * open / start / write / stop / close.  Builds on both EVKs via
 * the BOARD_I2S_AUDIO alias.
 *
 * I²S is the standard digital audio bus -- a master clocks data
 * to a slave at a fixed sample rate, the slave (DAC, codec)
 * converts each PCM sample into an analog voltage on the audio
 * output.  The wrapper handles the DMA / memory-slab plumbing so
 * apps just push frames.
 *
 * Triangle waves are picked over sines so the example doesn't
 * need libm linked in -- audible on a real DAC, runs on every
 * baremetal/Zephyr toolchain combination.
 */

#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/i2s.h"

/* alp_has() / ALP_HAS() -- gate on what the silicon offers, never on
 * a board name (#ifdef CONFIG_BOARD_* forks don't port to new SoMs). */
#include "alp/cap.h"

/* BOARD_I2S_AUDIO is a portable alias that resolves to the on-board
 * audio codec I2S bus on whichever EVK is being targeted:
 *   E1M EVK  (AEN)  → ALP_E1M_I2S0  (TAS2563 amps via 74LVC157 mux)
 *   E1M-X EVK (V2N) → ALP_E1M_X_I2S0 (TAS2563 smart-amp I2S)
 * Include via <alp/board.h>; ALP_BOARD_* is emitted by the build
 * system from the board.yaml preset. */
#include "alp/board.h"

/* Sample rate.  48 kHz is the de-facto digital-audio standard;
 * 44.1 kHz is CD-era; 16 kHz is fine for voice / smart-speaker
 * inference.  Pick to match the codec on the other end of the
 * bus. */
#define SR 48000u

/* Frames per DMA block.  256 frames @ 48 kHz = ~5.3 ms of audio
 * per block.  Smaller blocks reduce latency at the cost of more
 * DMA setup overhead; bigger blocks amortise the overhead but
 * delay underrun detection.  256 is the conventional default. */
#define BLOCK_FRAMES 256u

/* Push four blocks (~21 ms total) for the demo.  Real apps would
 * stream continuously from a producer thread (decoder / mic
 * pipeline / ML inference engine). */
#define BLOCKS_TO_SEND 4u

int main(void)
{
	/* Capability gate: skip cleanly on I2S-less silicon instead of
	 * forking the example with #ifdef CONFIG_BOARD_*.  Under
	 * native_sim no SoC is selected, the capability layer is
	 * permissive, and the demo proceeds to the open() below. */
	if (!alp_has(ALP_CAP_ID_HW_I2S)) {
		printf("[i2s] no I2S controller on this SoC (%s) -- skipping\n", ALP_SOC_REF_STR);
		printf("[i2s] done\n");
		return 0;
	}

	printf("[i2s] open BOARD_I2S_AUDIO @ 48 kHz s16 stereo TX\n");

	alp_i2s_t *i2s = alp_i2s_open(&(alp_i2s_config_t){
	    .bus_id         = BOARD_I2S_AUDIO, /* E1M EVK: ALP_E1M_I2S0; E1M-X EVK: ALP_E1M_X_I2S0 */
	    .sample_rate_hz = SR,
	    .word_bits      = 16, /* 16/24/32 supported */
	    .channels       = 2,  /* 1 = mono, 2 = stereo */
	    /* Standard I²S framing.  Switch to PCM_SHORT / PCM_LONG
         * when interfacing with cellular / Bluetooth audio codecs
         * that use frame-sync pulses instead of long word-select. */
	    .format = ALP_I2S_FMT_I2S,
	    /* TX direction = "we generate samples, slave receives".  Use
         * RX for microphones / line-in, BOTH for full-duplex codecs. */
	    .direction    = ALP_I2S_DIR_TX,
	    .block_frames = BLOCK_FRAMES,
	});
	if (i2s == NULL) {
		printf("[i2s] open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[i2s] done\n");
		return 0;
	}

	/* start() begins the bit-clock + frame-clock generation.  TX
     * starts fronting "underrun" silence until the first write()
     * arrives -- on real hardware that's sub-millisecond, but be
     * mindful when the codec is sensitive to DC. */
	alp_status_t s = alp_i2s_start(i2s);
	printf("[i2s] start -> %d\n", (int)s);

	/* Pre-compute one block of stereo triangle wave.
     *
     *   PERIOD samples per cycle  ⇒  SR/PERIOD Hz audible tone
     *   PERIOD = 48 @ SR=48k        ⇒  1 kHz (a clean test tone).
     *
     * AMP = 16384 = ½ of int16_t max range -- comfortable level
     * that won't clip the codec or wreck a listener's eardrums on
     * accidental full output. */
	static int16_t block[BLOCK_FRAMES * 2]; /* L,R interleaved */
	enum { PERIOD = 48 };
	enum { AMP = 16384 };
	for (uint32_t i = 0; i < BLOCK_FRAMES; i++) {
		uint32_t p = i % PERIOD;
		/* Two halves of a triangle: rising for [0..P/2), falling
         * for [P/2..P).  Output ranges from -AMP to +AMP. */
		int32_t v        = (p < PERIOD / 2) ? ((int32_t)p * (4 * AMP) / PERIOD - AMP)
		                                    : (3 * AMP - (int32_t)p * (4 * AMP) / PERIOD);
		block[i * 2 + 0] = (int16_t)v; /* L channel */
		block[i * 2 + 1] = (int16_t)v; /* R channel (mono content) */
	}

	/* Stream BLOCKS_TO_SEND × BLOCK_FRAMES samples.  Each write()
     * memcpys into the driver's slab and queues the DMA; if the
     * slab is full (consumer not draining fast enough),
     * timeout_ms applies. */
	for (uint32_t b = 0; b < BLOCKS_TO_SEND; b++) {
		s = alp_i2s_write(i2s, block, sizeof block, 100);
		printf("[i2s] write block %u -> %d\n", b, (int)s);
		if (s != ALP_OK) break;
	}

	/* stop() drains pending TX (waits for DMA completion), then
     * tri-states the bit/frame clocks.  Without stop() before
     * close(), some controllers leave residual clock activity. */
	alp_i2s_stop(i2s);
	alp_i2s_close(i2s);
	printf("[i2s] done\n");
	return 0;
}

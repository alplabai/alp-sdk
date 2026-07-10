/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libhelix-decode -- how to drive the Helix fixed-point MP3 decoder,
 * and how far you can get on a host (native_sim) build.
 *
 * The Helix MP3 decoder (real upstream:
 * github.com/ultraembedded/libhelix-mp3, RealNetworks RCSL/RPSL --
 * source-available, NOT Apache) is a fixed-point decoder written for
 * embedded ARM.  Its hot inner loops go through real/assembly.h,
 * which provides hand-tuned MULSHIFT32 for ARM (and Win32/x86 inline
 * asm) and `#error Unsupported platform` for anything else -- so the
 * REAL decoder compiles on an Ensemble E8 (Cortex-M55) target but
 * NOT on native_sim's generic x86-64 host.  alp-sdk therefore
 * references it as a west pin (extras-tier1 group, RCSL licence) and
 * never vendors its source into this tree.
 *
 * So this example runs the part of the pipeline that IS portable --
 * MP3 frame synchronisation + header parsing -- in plain C, printing
 * exactly what Helix's MP3FindSyncWord() + MP3GetLastFrameInfo()
 * would report (frame count, sample rate, samples-per-frame), and
 * shows the real MP3Decode() call sequence that turns each frame
 * into 16-bit PCM in the comments + README.  That real path is what
 * you compile when you build this example for an ARM SoM and fetch
 * the module with `west update --group-filter +extras-tier1`.
 *
 * [UNTESTED]: native_sim build (header-parse path); the real Helix
 * PCM decode has not been bench-run on silicon from this example.
 *
 * Real Helix API (compiled only on a supported ARM target):
 *
 *   #include "mp3dec.h"                 // from modules/lib/libhelix/pub/
 *   HMP3Decoder h = MP3InitDecoder();
 *   int off = MP3FindSyncWord(buf, nbytes);      // -> frame start
 *   unsigned char *p = buf + off;
 *   int left = nbytes - off;
 *   short pcm[1152 * 2];
 *   int err = MP3Decode(h, &p, &left, pcm, 0);   // -> PCM + advances p
 *   MP3FrameInfo fi;  MP3GetLastFrameInfo(h, &fi);
 *   // fi.samprate, fi.nChans, fi.outputSamps
 *   MP3FreeDecoder(h);
 *
 * What success looks like here (header-parse path):
 *
 *   [libhelix-decode] frame 0: layer 3, 44100 Hz, 1152 samples/frame
 *   [libhelix-decode] frame 1: layer 3, 44100 Hz, 1152 samples/frame
 *   [libhelix-decode] found 2 frame(s), 2304 total samples/channel
 *   [libhelix-decode] done
 */

#include <stdint.h>
#include <stdio.h>

/* Same canned stream as examples/audio/minimp3-decode: the first two
 * frames of a 32 kbps / 44.1 kHz / mono MPEG-1 Layer III clip (LAME
 * "Info" tag frame + one audio frame) plus trailing padding. */
static const uint8_t kCannedMp3[] = {
	0xff, 0xfb, 0x40, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x6e, 0x66, 0x6f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x04, 0x62, 0x00, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
	0x41, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x70, 0x70, 0x70, 0x70,
	0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x88, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xb8, 0xb8, 0xb8,
	0xb8, 0xb8, 0xb8, 0xb8, 0xb8, 0xb8, 0xb8, 0xb8, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0,
	0xd0, 0xd0, 0xd0, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xe8, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x61, 0x76,
	0x63, 0x36, 0x30, 0x2e, 0x33, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x24, 0x03, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x62, 0x3c, 0x3f, 0x62,
	0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xfb, 0x10, 0xc4, 0x00, 0x00, 0x04, 0x74, 0x13, 0x55,
	0x54, 0x90, 0x80, 0x30, 0xa6, 0x09, 0xaf, 0x37, 0x1a, 0x20, 0x02, 0x00, 0x01, 0xad, 0x39, 0x40,
	0x00, 0x01, 0x59, 0x3a, 0x3d, 0x50, 0x50, 0x08, 0x06, 0x09, 0x01, 0xf0, 0x7c, 0x1f, 0x07, 0xca,
	0x02, 0x00, 0x80, 0x61, 0x10, 0x7c, 0x1f, 0xd4, 0x08, 0x3b, 0x13, 0x87, 0xf8, 0x83, 0x70, 0x04,
	0x93, 0xf6, 0xc0, 0x60, 0x38, 0x1c, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x89, 0x2a, 0x99,
	0x14, 0x64, 0x08, 0xe9, 0x02, 0x48, 0x16, 0xa3, 0xf7, 0x85, 0x01, 0xf0, 0x13, 0x1b, 0xf0, 0x22,
	0x94, 0x2f, 0xa8, 0x1a, 0x12, 0xfc, 0x24, 0x0d, 0x2a, 0x0a, 0x00, 0x18, 0x30, 0x00, 0xff, 0xfb,
	0x12, 0xc4, 0x02, 0x83, 0xc5, 0x58, 0x1d, 0x20, 0x1d, 0xe0, 0x00, 0x28,
};

/* MPEG-1 Layer III sample-rate table, indexed by the 2-bit
 * sampling_frequency field of the frame header. */
static const int kMpeg1Rate[4] = { 44100, 48000, 32000, 0 /* reserved */ };

/* Portable equivalent of Helix's MP3FindSyncWord(): scan forward for
 * an 11-bit frame sync (0xFFE) that is followed by a valid MPEG-1
 * Layer III header.  Returns the offset, or -1 if none remains. */
static int find_sync(const uint8_t *buf, int len, int start)
{
	for (int i = start; i + 4 <= len; i++) {
		/* sync = 11 bits set; byte[1] bits 4-3 == 11b (MPEG-1),
		 * bits 2-1 == 01b (Layer III). */
		if (buf[i] != 0xff || (buf[i + 1] & 0xe0) != 0xe0) {
			continue;
		}
		int version = (buf[i + 1] >> 3) & 0x3; /* 3 == MPEG-1 */
		int layer   = (buf[i + 1] >> 1) & 0x3; /* 1 == Layer III */
		int rate_ix = (buf[i + 2] >> 2) & 0x3;
		if (version == 3 && layer == 1 && kMpeg1Rate[rate_ix] != 0) {
			return i;
		}
	}
	return -1;
}

int main(void)
{
	printf("[libhelix-decode] alp-sdk libhelix-decode starting\n");

	/* On a real ARM build you would MP3InitDecoder() here and call
	 * MP3Decode() inside the loop below; on native_sim we parse
	 * headers only (see file banner for why the RCSL fixed-point
	 * decoder can't compile for x86-64). */
	uint32_t total_samples = 0;
	int      frames        = 0;
	int      off           = 0;

	while ((off = find_sync(kCannedMp3, (int)sizeof(kCannedMp3), off)) >= 0) {
		int rate_ix = (kCannedMp3[off + 2] >> 2) & 0x3;
		int hz      = kMpeg1Rate[rate_ix];

		/* MPEG-1 Layer III is always 1152 samples/channel per
		 * frame -- the constant Helix's MP3GetLastFrameInfo()
		 * reports in MP3FrameInfo.outputSamps (per channel). */
		int samples_per_frame = 1152;

		printf("[libhelix-decode] frame %d: layer 3, %d Hz, %d samples/frame\n",
		       frames,
		       hz,
		       samples_per_frame);

		total_samples += (uint32_t)samples_per_frame;
		frames++;

		/* Advance past this sync so the next find_sync() starts
		 * after it.  A real decoder advances by the exact frame
		 * length MP3Decode() consumed; header-only, +2 past the
		 * sync bytes is enough to locate the next frame. */
		off += 2;
	}

	printf(
	    "[libhelix-decode] found %d frame(s), %u total samples/channel\n", frames, total_samples);

	if (frames == 0) {
		printf("[libhelix-decode] ERROR: no MP3 frames found\n");
		return 1;
	}

	printf("[libhelix-decode] done\n");
	return 0;
}

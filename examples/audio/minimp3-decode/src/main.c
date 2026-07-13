/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * minimp3-decode -- decode a canned MP3 byte array to 16-bit PCM
 * with the vendored minimp3 single-header decoder, then print the
 * sample count and RMS level of the audio it produced.
 *
 * minimp3 (github.com/lieff/minimp3, CC0-1.0) is a ~2500-line,
 * dependency-free MP3 decoder that lives entirely in one header.
 * alp-sdk vendors it verbatim under vendors/minimp3/include/ (see
 * that directory's README for WHY it's vendored instead of
 * west-fetched, and for a `-Wdouble-promotion` gotcha the unmodified
 * upstream source trips under this SDK's `-Werror` build).  It is a
 * pure-compute library -- no UART, no I2S, no filesystem -- so this
 * example needs no board-specific wiring and runs identically on
 * native_sim and real silicon.  `board.yaml`'s `libraries: [minimp3]`
 * only matters when you go on to stream the decoded PCM to a real
 * codec (see the "Where this leads" section in the README).
 *
 * What success looks like:
 *
 *   [minimp3-decode] decoded frame: 1152 samples, 1 ch, 44100 Hz
 *   [minimp3-decode] decoded frame: 1152 samples, 1 ch, 44100 Hz
 *   [minimp3-decode] total samples=2304 rms=1234.56
 *   [minimp3-decode] done
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* minimp3 is a single-file STB-style header: declarations are always
 * visible, but the ~2500-line implementation only compiles in
 * wherever exactly ONE translation unit defines
 * MINIMP3_IMPLEMENTATION before the #include.  This is that
 * translation unit.
 *
 * The pragma pair brackets the upstream implementation only (not our
 * own code below) -- see vendors/minimp3/README.md: unmodified
 * minimp3 compares a float against a double literal in
 * mp3d_scale_pcm(), which trips this SDK's -Wdouble-promotion
 * -Werror.  We silence it locally rather than patch vendored,
 * verbatim upstream source. */
#define MINIMP3_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include "minimp3.h"
#pragma GCC diagnostic pop

/* A real-world encoder emits a stream of back-to-back MP3 frames;
 * mp3dec_decode_frame() consumes exactly one frame per call and
 * reports how many input bytes it ate via
 * mp3dec_frame_info_t.frame_bytes, so the caller advances its read
 * pointer by that amount and calls again -- see the decode loop in
 * main() below.
 *
 * This buffer holds the first two frames of a 32 kbps/44.1 kHz/mono
 * MP3 (LAME's leading "Info" tag frame -- silent, but still a
 * syntactically valid MPEG-1 Layer III frame -- followed by one real
 * audio frame), plus a few trailing garbage bytes to demonstrate the
 * "not enough data for a full frame" case a streaming decoder must
 * also handle.  Generated with:
 *
 *   ffmpeg -f lavfi -i "sine=frequency=440:duration=0.2:sample_rate=44100" \
 *          -ac 1 -b:a 32k -codec:a libmp3lame - | <strip ID3v2 header, keep first 300 bytes>
 */
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

int main(void)
{
	printf("[minimp3-decode] alp-sdk minimp3-decode starting\n");

	/* mp3dec_t carries the decoder's cross-frame state (bit
	 * reservoir + MDCT overlap for the previous granule) -- init
	 * once, then reuse across every mp3dec_decode_frame() call for
	 * the same stream. Never zero-init it by hand; the layout is
	 * decoder-private. */
	mp3dec_t dec;
	mp3dec_init(&dec);

	/* mp3d_sample_t is int16_t unless the caller defines
	 * MINIMP3_FLOAT_OUTPUT (we don't -- int16 PCM is what every I2S
	 * DMA driver in this SDK expects, so there's no conversion step
	 * before a real playback path). MINIMP3_MAX_SAMPLES_PER_FRAME
	 * covers the worst case: 1152 samples * 2 channels. */
	mp3d_sample_t       pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	mp3dec_frame_info_t info;

	uint32_t total_samples = 0;
	int64_t  sum_sq        = 0; /* running sum of sample^2, for RMS */

	size_t offset = 0;
	while (offset < sizeof(kCannedMp3)) {
		int samples = mp3dec_decode_frame(
		    &dec, kCannedMp3 + offset, (int)(sizeof(kCannedMp3) - offset), pcm, &info);

		/* frame_bytes==0 means minimp3 couldn't find another
		 * complete frame in what's left of the buffer (ran out of
		 * data, or the tail is garbage/padding) -- that's the
		 * streaming decoder's normal end-of-input signal, not an
		 * error. */
		if (info.frame_bytes == 0) {
			break;
		}
		offset += (size_t)info.frame_bytes;

		/* `samples` is per-channel; a silent "Info" tag frame (see
		 * the buffer comment above) still reports the full 1152
		 * but every value is 0 -- that's expected, and why the
		 * RMS below is computed over ALL decoded frames rather
		 * than asserted per-frame. */
		if (samples > 0) {
			printf("[minimp3-decode] decoded frame: %d samples, %d ch, %d Hz\n",
			       samples,
			       info.channels,
			       info.hz);
			for (int i = 0; i < samples * info.channels; i++) {
				int32_t s = pcm[i];
				sum_sq += (int64_t)s * s;
			}
			total_samples += (uint32_t)(samples * info.channels);
		}
	}

	/* RMS (root-mean-square) is the standard loudness proxy for PCM:
	 * sqrt(mean(sample^2)).  A silence-only stream reads ~0; ordinary
	 * audio at 16-bit full scale reads in the low thousands. */
	double rms = total_samples ? sqrt((double)sum_sq / (double)total_samples) : 0.0;

	printf("[minimp3-decode] total samples=%u rms=%.2f\n", total_samples, rms);

	/* The canned buffer above always yields real audio samples --
	 * if this ever fires, the embedded array or the vendored decoder
	 * itself is broken, not the caller's usage. */
	if (total_samples == 0) {
		printf("[minimp3-decode] ERROR: decoded zero samples\n");
		return 1;
	}

	printf("[minimp3-decode] done\n");
	return 0;
}

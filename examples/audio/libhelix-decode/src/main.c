/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libhelix-decode -- decode a canned MP3 frame to 16-bit PCM with
 * the Helix fixed-point MP3 decoder and print the sample count.
 *
 * **[UNTESTED] -- does not build in this workspace.** The `libhelix`
 * west module (`west.yml`'s `extras-tier1` group, `path:
 * modules/lib/libhelix`) failed its pinned-revision checkout here:
 * `modules/lib/libhelix` is an initialised-but-empty git repo (still
 * on a placeholder branch, zero commits, no working tree) rather
 * than the vendor's actual source. There is consequently no
 * `mp3dec.h` anywhere in this workspace for the `#include` below to
 * resolve against, and no way to confirm from here whether the
 * checked-out module would even be the MP3 flavor of Helix (the
 * decoder family also ships an AAC flavor with a parallel
 * `AACInitDecoder`/`AACDecode`/`AACGetLastFrameInfo` API -- see
 * "If this turns out to be AAC" below).
 *
 * What follows is nonetheless the CORRECT, real Helix MP3 decoder
 * API -- `HMP3Decoder` / `MP3InitDecoder()` / `MP3FindSyncWord()` /
 * `MP3Decode()` / `MP3GetLastFrameInfo()` / `MP3FreeDecoder()` are
 * the actual exported symbols of `mp3dec.h` in every libhelix-mp3
 * fork (this API predates and outlives any specific packaging of
 * it). Once the module fetch is fixed (re-pin `west.yml`'s
 * `libhelix` revision to a real tag/commit and re-run `west
 * update`), this file should build with no logic changes -- only
 * CMakeLists.txt's include path needs wiring, per the comment there.
 *
 * What success would look like once buildable:
 *
 *   [libhelix-decode] alp-sdk libhelix-decode starting
 *   [libhelix-decode] decoded frame: 1152 samples, 1 ch, 44100 Hz
 *   [libhelix-decode] total samples=1152
 *   [libhelix-decode] done
 */

#include <stdint.h>
#include <stdio.h>

/* The real Helix MP3 decoder header.  NOT present in this workspace
 * -- see the [UNTESTED] note above.  Kept as a genuine #include
 * (not commented out) so this file teaches the real integration
 * shape rather than pseudocode. */
#include "mp3dec.h"

/* Same generation technique as examples/audio/minimp3-decode's
 * kCannedMp3 (see that example's README for the exact ffmpeg
 * command): the first two frames of a 32 kbps/44.1 kHz/mono MP3 --
 * LAME's leading silent "Info" tag frame followed by one real audio
 * frame -- plus a few trailing bytes so MP3FindSyncWord() has a
 * "ran out of data" case to demonstrate too. minimp3 and Helix both
 * decode standard MPEG-1 Layer III bitstreams, so the same canned
 * bytes exercise either decoder identically. */
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

/* Helix decodes one full MPEG frame (up to 2 channels * 2 granules *
 * 576 samples/granule) per MP3Decode() call -- 2304 int16 samples is
 * the documented worst case across every libhelix-mp3 packaging. */
#define HELIX_MAX_PCM_SAMPLES 2304

int main(void)
{
	printf("[libhelix-decode] alp-sdk libhelix-decode starting\n");

	/* HMP3Decoder is an opaque handle (a heap-allocated decoder
	 * state struct behind the scenes) -- MP3InitDecoder() owns the
	 * allocation; MP3FreeDecoder() at the end releases it.  Unlike
	 * minimp3's stack-friendly mp3dec_t, Helix's decoder state is
	 * NOT something the caller can put on the stack. */
	HMP3Decoder hDecoder = MP3InitDecoder();

	short pcm[HELIX_MAX_PCM_SAMPLES];

	unsigned char *readPtr       = (unsigned char *)kCannedMp3;
	int            bytesLeft     = (int)sizeof(kCannedMp3);
	uint32_t       total_samples = 0;

	while (bytesLeft > 0) {
		/* Unlike minimp3's mp3dec_decode_frame() (which tolerates
		 * a buffer that doesn't start exactly on a frame sync),
		 * Helix requires the caller to locate the next sync word
		 * itself first -- MP3FindSyncWord() returns the byte
		 * offset of the next 0xFFEx marker, or a negative value
		 * when none remains in what's left of the buffer (our
		 * normal end-of-input signal, mirroring minimp3's
		 * frame_bytes==0 case). */
		int offset = MP3FindSyncWord(readPtr, bytesLeft);
		if (offset < 0) {
			break;
		}
		readPtr += offset;
		bytesLeft -= offset;

		/* MP3Decode() advances readPtr/bytesLeft itself (by
		 * reference) past exactly the frame it consumed -- unlike
		 * minimp3, which reports frame_bytes and leaves advancing
		 * the pointer to the caller. */
		int err = MP3Decode(hDecoder, &readPtr, &bytesLeft, pcm, 0);
		if (err != ERR_MP3_NONE) {
			/* A real player would branch on the specific
			 * ERR_MP3_* code (see mp3dec.h); for this teaching
			 * example any decode error just ends the loop. */
			printf("[libhelix-decode] MP3Decode error %d, stopping\n", err);
			break;
		}

		MP3FrameInfo info;
		MP3GetLastFrameInfo(hDecoder, &info);
		printf("[libhelix-decode] decoded frame: %d samples, %d ch, %d Hz\n",
		       info.outputSamps,
		       info.nChans,
		       info.samprate);
		total_samples += (uint32_t)info.outputSamps;
	}

	/* Always free the decoder handle -- MP3InitDecoder() heap-
	 * allocates; there is no stack-scoped alternative in this API. */
	MP3FreeDecoder(hDecoder);

	printf("[libhelix-decode] total samples=%u\n", total_samples);

	if (total_samples == 0) {
		printf("[libhelix-decode] ERROR: decoded zero samples\n");
		return 1;
	}

	printf("[libhelix-decode] done\n");
	return 0;
}

/*
 * If this turns out to be AAC, not MP3:
 * ---------------------------------------
 * "libhelix" also names a Helix AAC decoder flavor with a parallel
 * API in an `aacdec.h` (`HAACDecoder`, `AACInitDecoder()`,
 * `AACDecode()`, `AACGetLastFrameInfo()`, `AACFlushCodec()`,
 * `AACFreeDecoder()`) -- same shape, different symbol prefix and a
 * different bitstream (ADTS/ADIF/RAW framing instead of MPEG frame
 * sync words, so MP3FindSyncWord()'s equivalent is
 * AACFindSyncWord()). Because the checkout failed, this repo cannot
 * inspect the module's `codecs/` layout to tell which flavor
 * `west.yml`'s pin actually resolves to. Whoever fixes the fetch
 * should check `modules/lib/libhelix/`'s top-level layout (an
 * `mp3dec.h` vs an `aacdec.h`, or a `codecs/mp3_dec/` vs
 * `codecs/aac_dec/` split are both common upstream conventions) and,
 * if it's AAC, swap this file's calls for the AAC equivalents and
 * decode a canned ADTS AAC frame instead of the MP3 bytes above.
 */

/* sound/src/snd_script.h -- the TEST image's per-effect loopback script,
 * shared with tools/audio_preview.c (--script) so the PC renders the exact
 * samples the image streams: same windows, same seeds, same synth build.
 *
 * Every window restarts the synth (tr_audio_init) so each clip is isolated
 * and deterministic; the stream itself never stops. Window lengths are in
 * 16 ms blocks (SND_BLOCK_MS), the I2S and mic block period at any rate.
 */
#ifndef SND_SCRIPT_H
#define SND_SCRIPT_H

#include <stdint.h>
#include <string.h>

#include "audio/tr_audio.h"
#include "ipc/tr_mbox.h" /* TR_CRASH_KIND_* */

#define SND_BLOCK_MS 16u
#define SND_FRAMES   (TR_AUDIO_RATE * SND_BLOCK_MS / 1000u) /* 256 at 16 kHz, 768 at 48 kHz */
#define SND_SEED     0x7E57u

enum { SND_W_SILENCE, SND_W_TONE, SND_W_MUSIC, SND_W_SFX };

typedef struct {
	uint8_t     type, kind, param;
	uint16_t    hz, amp; /* SND_W_TONE: frequency, peak amplitude (S16) */
	uint16_t    blocks;
	const char *name;
} snd_win_t;

/* 1 kHz at -12 and -3 dBFS, and 200/300/500 Hz at -6 dBFS: harmonic
 * distortion of the chain vs level, and where the speakers' usable low end
 * starts (200 Hz came back THD +33.9 dB on 2026-09-23). Music from bar 9 (lead + drums). Each effect alone. */
static const snd_win_t snd_windows[] = {
	{ SND_W_SILENCE, 0, 0, 0, 0, 31, "silence" },
	{ SND_W_TONE, 0, 0, 1000, 8231, 62, "tone1k_-12dBFS" },
	{ SND_W_TONE, 0, 0, 1000, 23197, 62, "tone1k_-3dBFS" },
	{ SND_W_TONE, 0, 0, 200, 16423, 62, "tone200_-6dBFS" },
	{ SND_W_TONE, 0, 0, 300, 16423, 62, "tone300_-6dBFS" },
	{ SND_W_TONE, 0, 0, 400, 16423, 62, "tone400_-6dBFS" },
	{ SND_W_TONE, 0, 0, 500, 16423, 62, "tone500_-6dBFS" },
	{ SND_W_MUSIC, TR_AEV_MUSIC, TR_MUSIC_PLAY, 0, 0, 625, "music" },
	{ SND_W_SFX, TR_AEV_FOOTSTEP, 0, 0, 0, 38, "footstep" },
	{ SND_W_SFX, TR_AEV_PICKUP, 0, 0, 0, 50, "pickup" },
	{ SND_W_SFX, TR_AEV_PICKUP, 6, 0, 0, 50, "pickup_combo6" },
	{ SND_W_SFX, TR_AEV_JUMP, 0, 0, 0, 50, "jump" },
	{ SND_W_SFX, TR_AEV_DUCK, 0, 0, 0, 50, "duck" },
	{ SND_W_SFX, TR_AEV_CRASH, TR_CRASH_KIND_LOW, 0, 0, 75, "crash" },
	{ SND_W_SFX, TR_AEV_CRASH, TR_CRASH_KIND_WIRE, 0, 0, 75, "crash_wire" },
	{ SND_W_SFX, TR_AEV_WIRE, 255, 0, 0, 62, "wire" },
	{ SND_W_SFX, TR_AEV_ATTRACT, 0, 0, 0, 100, "attract_jingle" },
	{ SND_W_SFX, TR_AEV_GAME_OVER, 0, 0, 0, 150, "game_over" },
};
#define SND_WINDOWS (sizeof(snd_windows) / sizeof(snd_windows[0]))

static const int16_t snd_sin256[257] = {
	0,      804,    1608,   2410,   3212,   4011,   4808,   5602,   6393,   7179,   7962,   8739,
	9512,   10278,  11039,  11793,  12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
	18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,
	25329,  25832,  26319,  26790,  27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
	30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,  32137,  32285,  32412,  32521,
	32609,  32678,  32728,  32757,  32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
	32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,  30273,  29956,  29621,  29268,
	28898,  28510,  28105,  27683,  27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
	23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,  18204,  17530,  16846,  16151,
	15446,  14732,  14010,  13279,  12539,  11793,  11039,  10278,  9512,   8739,   7962,   7179,
	6393,   5602,   4808,   4011,   3212,   2410,   1608,   804,    0,      -804,   -1608,  -2410,
	-3212,  -4011,  -4808,  -5602,  -6393,  -7179,  -7962,  -8739,  -9512,  -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
	-20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
	-31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
	-31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
	-20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278, -9512,  -8739,  -7962,  -7179,  -6393,  -5602,  -4808,  -4011,
	-3212,  -2410,  -1608,  -804,   0

};

/* Index of the first window of `type` (the verdict's silence/tone/music). */
static inline unsigned snd_window_of(uint8_t type)
{
	for (unsigned i = 0; i < SND_WINDOWS; i++) {
		if (snd_windows[i].type == type) return i;
	}
	return 0;
}

/* Block `blk` of window `w` into mono[SND_FRAMES]. *phase: tone phase. */
static inline void
snd_script_block(const snd_win_t *w, unsigned blk, int16_t *mono, uint32_t *phase)
{
	if (blk == 0u) {
		*phase = 0u;
		tr_audio_init(SND_SEED);
		if (w->type == SND_W_MUSIC) {
			tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
			tr_audio_seek(8u * 16u); /* bar 9: lead, kick, snare */
		} else if (w->type == SND_W_SFX) {
			tr_audio_event(w->kind, w->param);
		}
	}
	if (w->type == SND_W_TONE) {
		uint32_t inc = (uint32_t)(((uint64_t)w->hz << 32) / TR_AUDIO_RATE);
		for (unsigned i = 0; i < SND_FRAMES; i++) {
			uint32_t idx = *phase >> 24, fr = (*phase >> 8) & 0xFFFFu;
			int32_t  a = snd_sin256[idx], b = snd_sin256[idx + 1u];
			int32_t  v = a + (int32_t)(((int64_t)(b - a) * fr) >> 16);
			mono[i]    = (int16_t)((v * w->amp) >> 15);
			*phase += inc;
		}
	} else if (w->type == SND_W_SILENCE) {
		memset(mono, 0, SND_FRAMES * sizeof(int16_t));
	} else {
		tr_audio_render(mono, SND_FRAMES);
	}
}

#endif /* SND_SCRIPT_H */

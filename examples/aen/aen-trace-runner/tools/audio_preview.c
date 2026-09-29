/* tools/audio_preview.c -- render the music loop and every SFX to WAV files
 * for listening/reviewing on a PC (the same synth the HP runs):
 *
 *   cc -std=c11 -O2 -o /tmp/audio_preview tools/audio_preview.c src/audio/tr_audio.c
 *   /tmp/audio_preview OUTDIR          # OUTDIR/music.wav, OUTDIR/sfx_*.wav, OUTDIR/game.wav
 *   python3 tools/audio_spectro.py OUTDIR/music.wav ...   # one spectrogram PNG per clip
 *
 *   /tmp/audio_preview --script OUT   # (built with -Isrc) OUT.wav + OUT.txt: exactly the samples
 *                                     # the sound/ TEST image streams, window by window; used by
 *                                     # tools/audio_spectro.py --compare
 * Build with the image's -DTR_AUDIO_V2 / -DTR_AUDIO_RATE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/audio/tr_audio.h"
#include "../src/ipc/tr_mbox.h" /* TR_CRASH_KIND_* */
#include "../sound/src/snd_script.h"

#define SEC TR_AUDIO_RATE

static int16_t *s_pcm;
static uint32_t s_len, s_cap;

static void put(uint32_t samples)
{
	if (s_len + samples > s_cap) {
		s_cap = (s_len + samples) * 2u;
		s_pcm = realloc(s_pcm, s_cap * sizeof(int16_t));
		if (!s_pcm) exit(1);
	}
	tr_audio_render(s_pcm + s_len, samples);
	s_len += samples;
}

static void le32(FILE *f, uint32_t v)
{
	fputc((int)(v & 0xFFu), f), fputc((int)((v >> 8) & 0xFFu), f), fputc((int)((v >> 16) & 0xFFu), f),
	    fputc((int)(v >> 24), f);
}

static void le16(FILE *f, uint16_t v)
{
	fputc(v & 0xFF, f), fputc(v >> 8, f);
}

static void save(const char *dir, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fwrite("RIFF", 1, 4, f), le32(f, 36u + s_len * 2u), fwrite("WAVEfmt ", 1, 8, f);
	le32(f, 16), le16(f, 1), le16(f, 1), le32(f, TR_AUDIO_RATE), le32(f, TR_AUDIO_RATE * 2u), le16(f, 2),
	    le16(f, 16);
	fwrite("data", 1, 4, f), le32(f, s_len * 2u);
	for (uint32_t i = 0; i < s_len; i++) le16(f, (uint16_t)s_pcm[i]);
	fclose(f);
	printf("%s  %u ms  clipped %u\n", path, (unsigned)(s_len * 1000u / SEC), (unsigned)tr_audio_clipped());
	s_len = 0;
}

static int script(const char *stem)
{
	char path[512];
	snprintf(path, sizeof(path), "%s.txt", stem);
	FILE *t = fopen(path, "w");
	if (!t) {
		perror(path);
		return 1;
	}
	uint32_t phase = 0;
	for (unsigned w = 0; w < SND_WINDOWS; w++) {
		fprintf(t, "%s %u %u %u %u %u\n", snd_windows[w].name, (unsigned)(snd_windows[w].blocks * SND_FRAMES),
		        snd_windows[w].type, snd_windows[w].kind, snd_windows[w].param, snd_windows[w].hz);
		for (unsigned b = 0; b < snd_windows[w].blocks; b++) {
			if (s_len + SND_FRAMES > s_cap) {
				s_cap = (s_len + SND_FRAMES) * 2u;
				s_pcm = realloc(s_pcm, s_cap * sizeof(int16_t));
				if (!s_pcm) return 1;
			}
			snd_script_block(&snd_windows[w], b, s_pcm + s_len, &phase);
			s_len += SND_FRAMES;
		}
	}
	fclose(t);
	char dir[512], *slash;
	snprintf(dir, sizeof(dir), "%s", stem);
	slash = strrchr(dir, '/');
	const char *base = slash ? slash + 1 : dir;
	if (slash) *slash = 0;
	save(slash ? dir : ".", base);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--script") == 0) {
		return script(argv[2]);
	}
	if (argc != 2) {
		fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
		return 2;
	}
	static const struct {
		uint8_t kind, param;
		const char *name;
	} k_sfx[] = {
		{ TR_AEV_FOOTSTEP, 0, "sfx_footstep" }, { TR_AEV_PICKUP, 0, "sfx_pickup" },
		{ TR_AEV_PICKUP, 6, "sfx_pickup_combo6" }, { TR_AEV_JUMP, 0, "sfx_jump" },
		{ TR_AEV_DUCK, 0, "sfx_duck" },         { TR_AEV_CRASH, 1, "sfx_crash" },
		{ TR_AEV_CRASH, 3, "sfx_crash_wire" },  { TR_AEV_WIRE, 255, "sfx_wire" },
		{ TR_AEV_ATTRACT, 0, "sfx_attract_jingle" }, { TR_AEV_GAME_OVER, 0, "sfx_game_over" },
	};

	tr_audio_init(1u);
	tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
	put(TR_AUDIO_SONG_STEPS * TR_AUDIO_STEP);
	save(argv[1], "music");

	for (unsigned i = 0; i < sizeof(k_sfx) / sizeof(k_sfx[0]); i++) {
		tr_audio_init(1u);
		put(SEC / 10u);
		tr_audio_event(k_sfx[i].kind, k_sfx[i].param);
		do {
			put(256);
		} while (tr_audio_sfx_active());
		put(SEC / 5u);
		save(argv[1], k_sfx[i].name);
	}

	/* ~20 s of a played run: attract, start, steps, jumps, a pickup streak,
	 * a crash and the sting -- the mix as a player hears it. */
	tr_audio_init(1u);
	tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_ATTRACT);
	tr_audio_event(TR_AEV_ATTRACT, 0);
	put(4u * SEC);
	tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
	for (unsigned t = 0; t < 560u; t++) { /* 14 s of 40 Hz ticks */
		if (t % 8u == 0u) tr_audio_event(TR_AEV_FOOTSTEP, (uint8_t)((t / 8u) & 1u));
		if (t % 70u == 30u) tr_audio_event(TR_AEV_JUMP, 0);
		if (t % 110u == 60u) tr_audio_event(TR_AEV_DUCK, 0);
		if (t >= 200u && t < 400u && t % 40u == 0u) tr_audio_event(TR_AEV_PICKUP, (uint8_t)((t - 200u) / 40u));
		if (t >= 440u && t < 520u && t % 16u == 0u) tr_audio_event(TR_AEV_WIRE, (uint8_t)(100u + (t - 440u)));
		put(SEC / 40u);
	}
	tr_audio_event(TR_AEV_CRASH, TR_CRASH_KIND_WIRE);
	put(3u * SEC / 2u);
	tr_audio_event(TR_AEV_GAME_OVER, 0);
	put(3u * SEC);
	save(argv[1], "game");
	return 0;
}

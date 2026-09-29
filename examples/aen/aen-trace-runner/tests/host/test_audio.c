/* tests/host/test_audio.c -- src/audio/tr_audio.c: determinism, headroom,
 * every SFX audible and finite, the music loop. Also built for the A32 and
 * run under qemu (runner.sh), so the CRC below pins host == ARM bit for bit. */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "../../a32/common/crc32.c"
#include "../../src/audio/tr_audio.h"
#include "../../src/ipc/tr_mbox.h" /* TR_CRASH_KIND_WIRE */

#define BLOCK 256u
#define SEC   TR_AUDIO_RATE

static int16_t s_buf[BLOCK];

typedef struct {
	int32_t  peak;
	uint64_t sum_sq;
	uint32_t n, crc;
} stats_t;

static void render(stats_t *st, uint32_t samples)
{
	while (samples) {
		unsigned n = samples < BLOCK ? samples : BLOCK;
		tr_audio_render(s_buf, n);
		for (unsigned i = 0; i < n; i++) {
			int32_t a = abs(s_buf[i]);
			st->peak  = a > st->peak ? a : st->peak;
			st->sum_sq += (uint64_t)((int64_t)s_buf[i] * s_buf[i]);
		}
		st->crc = tr_crc32(st->crc, s_buf, n * sizeof(int16_t));
		st->n += n;
		samples -= n;
	}
}

static uint32_t rms(const stats_t *st)
{
	uint64_t m = st->sum_sq / (st->n ? st->n : 1u);
	uint32_t r = 0;
	while ((uint64_t)(r + 1u) * (r + 1u) <= m)
		r++;
	return r;
}

/* A scripted game: music, then every event kind, spaced out. */
static uint32_t scripted_crc(void)
{
	stats_t st = { 0 };

	tr_audio_init(1234u);
	tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_ATTRACT);
	tr_audio_event(TR_AEV_ATTRACT, 0);
	render(&st, 2u * SEC);
	tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
	for (uint8_t k = TR_AEV_FOOTSTEP; k < TR_AEV_COUNT; k++) {
		if (k == TR_AEV_MUSIC) continue;
		tr_audio_event(k, (uint8_t)(k * 37u));
		render(&st, SEC / 2u);
	}
	render(&st, 2u * SEC);
	return st.crc;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	/* --- determinism: same script, same seed -> same samples ------------- */
	uint32_t crc = scripted_crc();
	assert(scripted_crc() == crc);
	printf("test_audio: scripted CRC 0x%08X\n", (unsigned)crc);
	/* Goldens, identical on host and on the A32 under qemu (runner.sh builds
	 * all three configs). Regenerate only deliberately, old -> new in the
	 * commit. V1 must never move: it is the bench A/B baseline. */
#if !TR_AUDIO_V2
	assert(crc == 0xDBF70C58u);
#elif !TR_AUDIO_V3
	assert(crc == 0x3BD638D8u); /* V2, 16 kHz: bench A/B reference */
#elif TR_AUDIO_RATE == 16000u
	assert(crc == 0x2647F560u);
#else
	assert(crc == 0xFF25E646u);
#endif

	/* --- each SFX: audible, finite, silent afterwards --------------------- */
	static const struct {
		uint8_t     kind, param;
		const char *name;
	} k_sfx[] = {
		{ TR_AEV_FOOTSTEP, 0, "footstep" }, { TR_AEV_PICKUP, 0, "pickup" },
		{ TR_AEV_PICKUP, 5, "pickup x5" },  { TR_AEV_JUMP, 0, "jump" },
		{ TR_AEV_DUCK, 0, "duck" },         { TR_AEV_CRASH, 1, "crash low" },
		{ TR_AEV_CRASH, 3, "crash wire" },  { TR_AEV_WIRE, 255, "wire" },
		{ TR_AEV_ATTRACT, 0, "attract" },   { TR_AEV_GAME_OVER, 0, "game over" },
	};
	for (unsigned i = 0; i < sizeof(k_sfx) / sizeof(k_sfx[0]); i++) {
		stats_t st = { 0 };
		tr_audio_init(7u);
		tr_audio_event(k_sfx[i].kind, k_sfx[i].param);
		assert(tr_audio_sfx_active() > 0u);
		while (tr_audio_sfx_active() > 0u) {
			render(&st, BLOCK);
			assert(st.n <= 3u * SEC); /* bounded length */
		}
		stats_t tail = { 0 }, ring = { 0 };
		render(&ring, SEC / 20u); /* V3's master high-pass rings out for a few ms */
		render(&tail, BLOCK);
		printf("test_audio: %-10s %5u ms peak %5d rms %5u\n",
		       k_sfx[i].name,
		       (unsigned)(st.n * 1000u / SEC),
		       (int)st.peak,
		       (unsigned)rms(&st));
		assert(tail.peak == 0);     /* nothing left sounding */
		assert(st.peak >= 800);     /* audible */
		assert(st.n >= SEC / 100u); /* not a click */
		assert(tr_audio_clipped() == 0u);
	}
	/* footsteps stay subtle next to the effects that matter */
	{
		stats_t step = { 0 }, crash = { 0 };
		tr_audio_init(7u);
		tr_audio_event(TR_AEV_FOOTSTEP, 0);
		render(&step, SEC);
		tr_audio_init(7u);
		tr_audio_event(TR_AEV_CRASH, 1);
		render(&crash, SEC);
		assert(step.peak * 3 < crash.peak);
	}
	/* combo raises the chime's pitch: count zero crossings */
	{
		unsigned zc[2] = { 0, 0 };
		for (unsigned c = 0; c < 2u; c++) {
			tr_audio_init(7u);
			tr_audio_event(TR_AEV_PICKUP, (uint8_t)(c * 7u));
			tr_audio_render(s_buf, BLOCK); /* first 16 ms: only the first chime note sounds */
			for (unsigned i = 1; i < BLOCK; i++)
				zc[c] += (s_buf[i - 1] < 0) != (s_buf[i] < 0);
		}
		assert(zc[1] > zc[0]);
	}

	/* --- mixer headroom -------------------------------------------------- */
	{
		stats_t music = { 0 };
		tr_audio_init(99u);
		tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
		render(&music, TR_AUDIO_SONG_STEPS * TR_AUDIO_STEP); /* one whole loop */
		printf("test_audio: music loop %u s peak %d rms %u\n",
		       (unsigned)(music.n / SEC),
		       (int)music.peak,
		       (unsigned)rms(&music));
		assert(tr_audio_song_step() == 0u); /* wrapped exactly */
		assert(music.peak <= 23197);        /* music alone stays under -3 dBFS */
		assert(rms(&music) >= 1000u);
		assert(tr_audio_clipped() == 0u);

		/* worst case: full-level music with EVERY effect fired at once, twice */
		stats_t all = { 0 };
		for (unsigned rep = 0; rep < 2u; rep++) {
			for (uint8_t k = TR_AEV_FOOTSTEP; k < TR_AEV_COUNT; k++) {
				if (k != TR_AEV_MUSIC) tr_audio_event(k, 255);
			}
			render(&all, 3u * SEC);
		}
		printf("test_audio: everything at once peak %d, limiter bent %u samples\n",
		       (int)all.peak,
		       (unsigned)tr_audio_limited());
		assert(tr_audio_clipped() == 0u);
	}

	/* --- attract is quieter than play; OFF fades to digital silence ------ */
	{
		stats_t a = { 0 }, p = { 0 }, off = { 0 };
		tr_audio_init(5u);
		tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_ATTRACT);
		render(&a, 4u * SEC);
		tr_audio_init(5u);
		tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);
		render(&p, 4u * SEC);
		assert(rms(&a) * 4u < rms(&p) * 3u);
		tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_OFF);
		render(&off, SEC); /* fade */
		stats_t after = { 0 };
		render(&after, SEC);
		assert(after.peak == 0);
	}

#if TR_AUDIO_V3
	/* --- voice pool: a wire crash fits whole; a full pool gives up its
	 * OLDEST voices, never the event being started ------------------- */
	{
		tr_audio_init(3u);
		uint32_t c = tr_audio_event(TR_AEV_CRASH, TR_CRASH_KIND_WIRE);
		assert(tr_audio_event_voices(c) == 14u); /* 3 impact + 6 debris + zap + 4 crackle */
		tr_audio_init(3u);
		uint32_t a = tr_audio_event(TR_AEV_ATTRACT, 0);   /* 6 voices */
		uint32_t o = tr_audio_event(TR_AEV_GAME_OVER, 0); /* 5 */
		uint32_t w =
		    tr_audio_event(TR_AEV_WIRE, 255); /* 7: pool 16 full, 2 of the attract's stolen */
		assert(tr_audio_event_voices(a) == 4u && tr_audio_event_voices(o) == 5u &&
		       tr_audio_event_voices(w) == 7u);
		c = tr_audio_event(TR_AEV_CRASH, TR_CRASH_KIND_WIRE);
		assert(tr_audio_event_voices(c) ==
		       14u); /* all 14 kept: 4 attract + 5 game over + 5 wire gave way */
		assert(tr_audio_event_voices(a) == 0u && tr_audio_event_voices(o) == 0u &&
		       tr_audio_event_voices(w) == 2u);

		/* age != slot order: 8 footsteps fill the pool, their 15 ms clicks
		 * end (odd slots free), an attract jingle takes those. A wire crash
		 * then needs 12 steals: oldest-first takes the 8 footstep noises
		 * and the attract's 4 oldest notes; a slot-order sweep would take
		 * the whole jingle instead. */
		tr_audio_init(3u);
		uint32_t fs[8];
		for (unsigned i = 0; i < 8u; i++)
			fs[i] = tr_audio_event(TR_AEV_FOOTSTEP, (uint8_t)(i & 1u));
		tr_audio_render(s_buf, SEC / 50u); /* 20 ms */
		a             = tr_audio_event(TR_AEV_ATTRACT, 0);
		c             = tr_audio_event(TR_AEV_CRASH, TR_CRASH_KIND_WIRE);
		unsigned left = 0;
		for (unsigned i = 0; i < 8u; i++)
			left += tr_audio_event_voices(fs[i]);
		assert(tr_audio_event_voices(c) == 14u && left == 0u && tr_audio_event_voices(a) == 2u);
	}

	/* --- the V3 master high-pass (4th order, 450 Hz): the speakers' dead zone out -- */
	{
		static const struct {
			double hz, lo_db, hi_db;
		} k_pts[] = {
			{ 100.0, -99.0, -45.0 }, { 200.0, -99.0, -25.0 }, { 300.0, -17.0, -11.0 },
			{ 450.0, -4.5, -1.5 },   { 1000.0, -0.6, 0.3 },   { 4000.0, -0.3, 0.3 },
		};
		for (unsigned k = 0; k < sizeof(k_pts) / sizeof(k_pts[0]); k++) {
			tr_hpf_t bq = { 0 };
			double   in = 0, out = 0;
			for (unsigned i = 0; i < 2u * SEC; i++) {
				int32_t x =
				    (int32_t)lround(16000.0 * sin(2.0 * 3.141592653589793 * k_pts[k].hz * i / SEC));
				int32_t y = tr_audio_hpf(&bq, x);
				if (i >= SEC) { /* settled */
					in += (double)x * x;
					out += (double)y * y;
				}
			}
			double db = 10.0 * log10(out / in);
			printf("test_audio: hpf %6.0f Hz %+6.2f dB\n", k_pts[k].hz, db);
			assert(db >= k_pts[k].lo_db && db <= k_pts[k].hi_db);
		}
		tr_hpf_t bq = { 0 }; /* DC in, silence out; zero in stays exactly zero */
		int32_t  y  = 0;
		for (unsigned i = 0; i < SEC; i++)
			y = tr_audio_hpf(&bq, 12345);
		assert(y == 0);
		for (unsigned i = 0; i < SEC / 10u; i++)
			y = tr_audio_hpf(&bq, 0);
		assert(y == 0);
	}
#endif
	puts("test_audio: ok");
	return 0;
}

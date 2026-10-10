/* tests/host/test_vol.c -- src/ipc/tr_vol.c: the shared volume word (cold garbage reads as the
 * default), the HP's gain (0 / max / ramp) and the HE's adoption rules (encoder, switch, the
 * bench's request word). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_vol.h"

static void test_word(void)
{
	uint32_t p = 77u;

	assert(tr_vol_word(0u) == (TR_VOL_TAG | 0u) && tr_vol_word(100u) == (TR_VOL_TAG | 100u));
	assert(tr_vol_word(250u) == (TR_VOL_TAG | 100u)); /* never builds an invalid word */
	assert(tr_vol_valid(tr_vol_word(40u), &p) && p == 40u);
	p = 77u;
	assert(!tr_vol_valid(TR_VOL_TAG | 101u, &p) && p == 77u); /* untouched when invalid */
	assert(!tr_vol_valid(0x564E0028u, &p));                   /* wrong tag */
	assert(!tr_vol_valid(0x00000028u, &p));                   /* a bare percent, no tag */
	assert(!tr_vol_valid(TR_VOL_TAG | 0x8000u, &p));          /* a stray bit below the tag */

	/* Cold SRAM0: zeros, ones, the usual power-on patterns and an old image's words all read as
	 * the default, never as a level (a garbage 0 would be a muted game). */
	const uint32_t junk[] = { 0x00000000u, 0xFFFFFFFFu, 0xA5A5A5A5u, 0x5A5A5A5Au, 0xDEADBEEFu,
		                      0x42320000u, 0x00000064u, 0x564F0065u, 0x564FFFFFu, 0xCDCDCDCDu };
	for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
		assert(tr_vol_read(junk[i]) == TR_VOL_DEFAULT);
	}
	assert(tr_vol_read(tr_vol_word(0u)) == 0u && tr_vol_read(tr_vol_word(35u)) == 35u);
}

static void test_gain(void)
{
	assert(tr_vol_gain(0u) == 0u && tr_vol_gain(100u) == TR_VOL_UNITY);
	assert(tr_vol_gain(50u) == TR_VOL_UNITY / 2u && tr_vol_gain(1u) == 655u);
	assert(tr_vol_gain(101u) == TR_VOL_UNITY); /* clamped */

	int16_t       a[256], ref[256];
	tr_vol_ramp_t r;

	for (unsigned i = 0; i < 256u; i++) {
		a[i] = ref[i] = (int16_t)((int)i * 257 - 32768);
	}
	a[0] = ref[0] = -32768;
	a[255] = ref[255] = 32767;

	/* 100 %: bit-exact, the steady common case */
	tr_vol_ramp_init(&r, 100u);
	tr_vol_apply(&r, a, 256u, 100u);
	assert(memcmp(a, ref, sizeof(a)) == 0);

	/* 0 %: silence, steady */
	tr_vol_ramp_init(&r, 0u);
	tr_vol_apply(&r, a, 256u, 0u);
	for (unsigned i = 0; i < 256u; i++) {
		assert(a[i] == 0);
	}

	/* 50 % steady: half, rounded; the extremes do not wrap */
	memcpy(a, ref, sizeof(a));
	tr_vol_ramp_init(&r, 50u);
	tr_vol_apply(&r, a, 256u, 50u);
	assert(a[0] == -16384 && a[255] == 16384);
	for (unsigned i = 0; i < 256u; i++) {
		int d = (int)a[i] - (int)(ref[i] / 2);
		assert(d >= -1 && d <= 1);
	}
}

static void test_ramp(void)
{
	/* A constant full-scale signal makes the gain itself readable. 100 -> 0 across one block: a
	 * straight descent, every step the same size to within rounding, ending at silence. */
	int16_t       b[256];
	tr_vol_ramp_t r;

	for (unsigned i = 0; i < 256u; i++) {
		b[i] = 30000;
	}
	tr_vol_ramp_init(&r, 100u);
	tr_vol_apply(&r, b, 256u, 0u);
	assert(b[255] == 0 && b[0] < 30000 && b[0] > 29000);
	int maxstep = 0;
	for (unsigned i = 1; i < 256u; i++) {
		int step = (int)b[i - 1] - (int)b[i];
		assert(step >= 0); /* monotonic: no overshoot, no click */
		if (step > maxstep) {
			maxstep = step;
		}
	}
	assert(maxstep <= 30000 / 255 + 2); /* ~118 per sample, never a jump */
	assert(r.gain == 0u);

	/* the next block at 0 stays silent; then 0 -> 100 ramps up and lands on unity bit-exact */
	for (unsigned i = 0; i < 256u; i++) {
		b[i] = 30000;
	}
	tr_vol_apply(&r, b, 256u, 0u);
	assert(b[0] == 0 && b[255] == 0);
	for (unsigned i = 0; i < 256u; i++) {
		b[i] = 30000;
	}
	tr_vol_apply(&r, b, 256u, 100u);
	assert(b[0] > 0 && b[0] < 300 && b[255] == 30000);
	for (unsigned i = 1; i < 256u; i++) {
		assert(b[i] >= b[i - 1]);
	}
	for (unsigned i = 0; i < 256u; i++) {
		b[i] = 30000;
	}
	tr_vol_apply(&r, b, 256u, 100u);
	for (unsigned i = 0; i < 256u; i++) {
		assert(b[i] == 30000);
	}

	/* a block length that does not divide the ramp evenly still lands exactly on the target */
	{
		int16_t c[240];

		for (unsigned i = 0; i < 240u; i++) {
			c[i] = 32767;
		}
		tr_vol_ramp_init(&r, 0u);
		tr_vol_apply(&r, c, 240u, 100u);
		assert(c[239] == 32767 && r.gain == TR_VOL_UNITY);
		for (unsigned i = 1; i < 240u; i++) {
			assert(c[i] >= c[i - 1]);
		}
	}

	/* a target change with no samples is harmless (n == 0 never divides) */
	tr_vol_apply(&r, b, 0u, 10u);
}

static volatile tr_vol_t rec;

static void fresh(tr_vol_he_t *he)
{
	memset((void *)&rec, 0xA5, sizeof(rec)); /* cold SRAM0 */
	tr_vol_he_boot(he, &rec);
}

/* A boot at the default, then moved to 100 % so the step arithmetic below starts from full. */
static void fresh100(tr_vol_he_t *he)
{
	fresh(he);
	he->pct        = 100u;
	he->unmute_pct = 100u;
	rec.vol        = tr_vol_word(100u);
}

static void test_boot(void)
{
	tr_vol_he_t he;

	/* the HE boots at the default (30 %), never at 100 %: published before anything else runs */
	assert(TR_VOL_DEFAULT == 30u);
	fresh(&he);
	assert(he.pct == 30u && rec.vol == tr_vol_word(30u) && rec.seq == 0u && rec.rejects == 0u);
	assert(rec.req == 0u); /* the 0xA5A5A5A5 req left over: dropped at boot */
	assert(!tr_vol_he_step(&he, &rec, 0, false));
	assert(he.pct == 30u && rec.rejects == 0u);
	/* the HP reads a cold or garbage word as that same level, so the stream starts at 30 % too */
	assert(tr_vol_read(0xA5A5A5A5u) == 30u && tr_vol_read(0u) == 30u);
	tr_vol_ramp_t r;
	int16_t       b[256];

	tr_vol_ramp_init(&r, tr_vol_read(0xA5A5A5A5u));
	for (unsigned i = 0; i < 256u; i++) {
		b[i] = 30000;
	}
	tr_vol_apply(&r, b, 256u, tr_vol_read(0xA5A5A5A5u));
	assert(b[0] == 9000 && b[255] == 9000); /* 30 %, steady, from the first block */
}

static void test_he(void)
{
	tr_vol_he_t he;

	fresh100(&he);
	assert(he.pct == 100u && rec.vol == tr_vol_word(100u));

	/* encoder: 5 % a detent, clamped both ends, any burst size */
	assert(tr_vol_he_step(&he, &rec, -1, false) && he.pct == 95u && rec.vol == tr_vol_word(95u));
	assert(rec.seq == 1u);
	assert(tr_vol_he_step(&he, &rec, 1, false) && he.pct == 100u);
	assert(!tr_vol_he_step(&he, &rec, 3, false) && he.pct == 100u); /* clamped: no change, no seq */
	assert(rec.seq == 2u);
	assert(tr_vol_he_step(&he, &rec, -7, false) && he.pct == 65u);
	assert(tr_vol_he_step(&he, &rec, -1000000, false) && he.pct == 0u); /* a wild burst */
	assert(rec.vol == tr_vol_word(0u));
	assert(!tr_vol_he_step(&he, &rec, -1, false) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 2147483647, false) && he.pct == 100u);
	assert(tr_vol_he_step(&he, &rec, (-2147483647 - 1), false) && he.pct == 0u);
	/* a burst whose x5 would wrap an int32 (0x20000000 * 5 = 0xA0000000) still clamps to full */
	assert(tr_vol_he_step(&he, &rec, 19, false) && he.pct == 95u);
	assert(tr_vol_he_step(&he, &rec, 0x20000000, false) && he.pct == 100u);

	/* switch: mute and back to the last non-zero level */
	fresh100(&he);
	assert(tr_vol_he_step(&he, &rec, -12, false) && he.pct == 40u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u && rec.vol == tr_vol_word(0u));
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 40u);
	/* turned down to 0 in one burst, the switch restores the last level the HE adopted ... */
	assert(tr_vol_he_step(&he, &rec, -8, false) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 40u);
	/* ... one detent at a time, the last audible step */
	for (int i = 0; i < 7; i++) {
		assert(tr_vol_he_step(&he, &rec, -1, false));
	}
	assert(he.pct == 5u);
	assert(tr_vol_he_step(&he, &rec, -1, false) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 5u);
	/* an unmute from a fresh boot goes back to the boot level */
	fresh(&he);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 30u);
}

static void test_mute(void)
{
	tr_vol_he_t he;

	fresh100(&he);
	assert(tr_vol_he_step(&he, &rec, -12, false) && he.pct == 40u);
	/* mute, then turn up: unmutes and steps from the SAVED level, not from 0 */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u && rec.vol == tr_vol_word(0u));
	assert(tr_vol_he_step(&he, &rec, 1, false) && he.pct == 45u && rec.vol == tr_vol_word(45u));
	/* mute, then turn down: from the saved level too */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, -2, false) && he.pct == 35u);
	/* a press after a turn mutes again and the next press restores that level */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 35u);
	/* the saved level survives a muted second press only once: unmuted, a press mutes again */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 35u);
	/* muted at the top, one detent up restores full */
	assert(tr_vol_he_step(&he, &rec, 13, false) && he.pct == 100u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 1, false) && he.pct == 100u);
	/* muted at the lowest audible step, one detent down lands on 0 unmuted: the press resumes it */
	assert(tr_vol_he_step(&he, &rec, -19, false) && he.pct == 5u);
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	assert(!tr_vol_he_step(&he, &rec, -1, false) && he.pct == 0u); /* 5 - 5 = 0: still silent */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 5u);
	/* a request while muted replaces the saved level; a muting request keeps it */
	assert(tr_vol_he_step(&he, &rec, 0, true) && he.pct == 0u);
	rec.req = tr_vol_word(70u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 70u);
	rec.req = tr_vol_word(0u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 0u);
	assert(tr_vol_he_step(&he, &rec, 1, false) && he.pct == 75u);
}

static void test_request(void)
{
	tr_vol_he_t he;

	fresh100(&he);
	uint32_t seq = rec.seq;

	/* a valid request is adopted, and the word is handed back (0) */
	rec.req = tr_vol_word(30u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 30u && rec.vol == tr_vol_word(30u));
	assert(rec.seq == seq + 1u && rec.rejects == 0u && rec.req == 0u);
	assert(!tr_vol_he_step(&he, &rec, 0, false)); /* nothing pending */

	/* the local control moved on; the SAME word written again is a new request and takes it back */
	assert(tr_vol_he_step(&he, &rec, 1, false) && he.pct == 35u);
	rec.req = tr_vol_word(30u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 30u && rec.req == 0u);
	rec.req = tr_vol_word(0u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 0u && rec.req == 0u);

	/* invalid requests are refused, counted every time (the same bad word twice is two), and change nothing */
	rec.req = TR_VOL_TAG | 101u;
	assert(!tr_vol_he_step(&he, &rec, 0, false) && he.pct == 0u && rec.rejects == 1u);
	assert(rec.req == 0u);
	rec.req = 0x00000032u; /* 50 without the tag */
	assert(!tr_vol_he_step(&he, &rec, 0, false) && rec.rejects == 2u);
	rec.req = 0x00000032u;
	assert(!tr_vol_he_step(&he, &rec, 0, false) && rec.rejects == 3u && rec.req == 0u);
	assert(!tr_vol_he_step(&he, &rec, 0, false) && rec.rejects == 3u); /* no request, no count */
	rec.req = 0xFFFFFFFFu;
	assert(!tr_vol_he_step(&he, &rec, 0, false) && rec.rejects == 4u && he.pct == 0u);
	assert(rec.vol == tr_vol_word(0u));

	/* a repeated identical valid request works each time */
	for (int i = 0; i < 3; i++) {
		rec.req = tr_vol_word(60u);
		assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 60u && rec.req == 0u);
		assert(tr_vol_he_step(&he, &rec, -4, false) && he.pct == 40u);
	}
	assert(rec.rejects == 4u);

	/* a stale valid req from before the boot is dropped, not obeyed ... */
	memset((void *)&rec, 0, sizeof(rec));
	rec.req = tr_vol_word(10u);
	tr_vol_he_boot(&he, &rec);
	assert(rec.req == 0u);
	assert(!tr_vol_he_step(&he, &rec, 0, false) && he.pct == TR_VOL_DEFAULT);
	/* ... but a new one is */
	rec.req = tr_vol_word(20u);
	assert(tr_vol_he_step(&he, &rec, 0, false) && he.pct == 20u);
}

static void test_single_writer(void)
{
	tr_vol_he_t he;

	fresh100(&he);
	assert(tr_vol_he_step(&he, &rec, -10, false) && he.pct == 50u);
	/* someone writes vol over SWD (the wrong word): the HE puts its own level back */
	rec.vol = tr_vol_word(90u);
	assert(!tr_vol_he_step(&he, &rec, 0, false));
	assert(rec.vol == tr_vol_word(50u) && he.pct == 50u);
	rec.vol = 0xDEADBEEFu;
	(void)tr_vol_he_step(&he, &rec, 0, false);
	assert(rec.vol == tr_vol_word(50u));
	/* the HP reads what the HE published, and a garbage vol would be 100 % there */
	assert(tr_vol_read(rec.vol) == 50u);
}

int main(void)
{
	test_word();
	test_gain();
	test_ramp();
	test_boot();
	test_he();
	test_mute();
	test_request();
	test_single_writer();
	puts("test_vol: ok");
	return 0;
}

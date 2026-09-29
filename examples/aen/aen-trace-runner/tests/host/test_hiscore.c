/* tests/host/test_hiscore.c -- the booth's high-score table and the tilt
 * initials entry (src/game/hiscore.c): insert order, ties, a full table,
 * and the entry state machine (cycle, confirm, back, timeout, walk-away). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/hiscore.h"

static tr_intent_t lane(int8_t d)
{
	tr_intent_t in = tr_intent_none();

	in.lane_delta = d;
	return in;
}

static tr_intent_t jump(void)
{
	tr_intent_t in = tr_intent_none();

	in.jump = true;
	return in;
}

static tr_intent_t duck(void)
{
	tr_intent_t in = tr_intent_none();

	in.duck = true;
	return in;
}

int main(void)
{
	/* 1. The table: empty at boot; a score takes the place it earns,
	 * highest first; a zero score never enters. */
	{
		tr_hiscore_t t;

		tr_hs_init(&t);
		assert(t.n == 0u && t.last == TR_HS_NONE);
		assert(tr_hs_rank(&t, 0u) < 0);
		assert(tr_hs_rank(&t, 1u) == 0);
		assert(tr_hs_insert(&t, 500u, "AAA") == 0);
		assert(tr_hs_insert(&t, 900u, "BBB") == 0);
		assert(tr_hs_insert(&t, 700u, "CCC") == 1);
		assert(t.n == 3u && t.last == 1u);
		assert(t.e[0].score == 900u && t.e[1].score == 700u && t.e[2].score == 500u);
		assert(strcmp(t.e[0].name, "BBB") == 0 && strcmp(t.e[1].name, "CCC") == 0 &&
		       strcmp(t.e[2].name, "AAA") == 0);
	}

	/* 2. Ties: the earlier holder keeps the place -- an equal score goes
	 * below it; on a full table, equalling the last place does not enter. */
	{
		tr_hiscore_t t;

		tr_hs_init(&t);
		(void)tr_hs_insert(&t, 300u, "ONE");
		assert(tr_hs_rank(&t, 300u) == 1);
		assert(tr_hs_insert(&t, 300u, "TWO") == 1);
		assert(strcmp(t.e[0].name, "ONE") == 0 && strcmp(t.e[1].name, "TWO") == 0);
		(void)tr_hs_insert(&t, 100u, "C  ");
		(void)tr_hs_insert(&t, 90u, "D  ");
		(void)tr_hs_insert(&t, 80u, "E  ");
		assert(t.n == TR_HS_N);
		assert(tr_hs_rank(&t, 80u) < 0 && tr_hs_insert(&t, 80u, "NOP") < 0);
		assert(t.e[4].score == 80u && strcmp(t.e[4].name, "E  ") == 0);
		/* one more than the last place enters and pushes the last out */
		assert(tr_hs_insert(&t, 81u, "NEW") == 4 && t.e[4].score == 81u && t.n == TR_HS_N);
		assert(tr_hs_insert(&t, 1000u, "TOP") == 0 && t.e[0].score == 1000u && t.e[4].score == 90u);
		for (unsigned i = 1; i < TR_HS_N; i++) {
			assert(t.e[i - 1].score >= t.e[i].score); /* always in order */
		}
		assert(tr_hs_top(&t) == 1000u);
		tr_hs_init(&t);
		assert(tr_hs_top(&t) == 0u);
	}

	/* 3. A long mixed stream keeps the table sorted, the n best, stable. */
	{
		tr_hiscore_t t;
		uint32_t     rng = 7u, best[TR_HS_N] = { 0 };

		tr_hs_init(&t);
		for (int k = 0; k < 2000; k++) {
			rng           = rng * 1664525u + 1013904223u;
			uint32_t s    = (rng >> 16) % 300u;
			int      want = tr_hs_rank(&t, s), got = tr_hs_insert(&t, s, "XYZ");

			assert(want == got);
			/* the reference: the TR_HS_N best seen so far */
			for (int i = 0; i < TR_HS_N; i++) {
				if (s > best[i]) {
					memmove(&best[i + 1], &best[i], (TR_HS_N - 1 - (size_t)i) * sizeof(best[0]));
					best[i] = s;
					break;
				}
			}
		}
		for (int i = 0; i < TR_HS_N; i++) {
			assert(t.e[i].score == best[i]);
		}
	}

	/* 4. Initials: starts on the default, cycles the current letter with a
	 * tilt left / right (wrapping), toward confirms and moves on, the third
	 * confirm finishes; away steps back one letter. */
	{
		tr_initials_t e;

		tr_ini_start(&e, "PRO", 2);
		assert(strcmp(e.name, "PRO") == 0 && e.pos == 0u && e.rank == 2 &&
		       e.why == TR_INI_ENTERING);
		assert(tr_ini_step(&e, lane(1)) && e.name[0] == 'Q');
		assert(tr_ini_step(&e, lane(-1)) && tr_ini_step(&e, lane(-1)) && e.name[0] == 'O');
		for (int k = 0; k < 14; k++) { /* O is the 15th letter */
			assert(tr_ini_step(&e, lane(-1)));
		}
		assert(e.name[0] == 'A');
		assert(tr_ini_step(&e, lane(-1)) && e.name[0] == ' '); /* wraps to the last symbol */
		assert(tr_ini_step(&e, lane(1)) && e.name[0] == 'A');
		assert(tr_ini_step(&e, jump()) && e.pos == 1u);
		assert(tr_ini_step(&e, lane(1)) && strcmp(e.name, "ASO") == 0);
		assert(tr_ini_step(&e, duck()) && e.pos == 0u); /* back to the first */
		assert(tr_ini_step(&e, duck()) && e.pos == 0u); /* never before it */
		assert(tr_ini_step(&e, jump()) && tr_ini_step(&e, jump()) && e.pos == 2u);
		for (int k = 0; k < 5; k++) {
			assert(tr_ini_step(&e, tr_intent_none())); /* no input: waits */
		}
		assert(!tr_ini_step(&e, jump()) && e.why == TR_INI_DONE && strcmp(e.name, "ASO") == 0);
		assert(!tr_ini_step(&e, lane(1)) && strcmp(e.name, "ASO") == 0); /* finished: frozen */
		/* the alphabet the HUD shows: letters, digits, space -- one of each */
		const char *a = TR_INI_ALPHABET;

		assert(strlen(a) == 37u && strchr(a, '8') && strchr(a, 'E') && strchr(a, ' '));
	}

	/* 5. Walk-away: TR_INI_IDLE_FRAMES frames without a gesture commit the
	 * letters as they stand (the unconfirmed ones keep their default). A
	 * gesture restarts that count. */
	{
		tr_initials_t e;
		uint32_t      f = 0;

		tr_ini_start(&e, "E8 ", 0);
		assert(tr_ini_step(&e, lane(1)) && e.name[0] == 'F');
		for (f = 1; tr_ini_step(&e, tr_intent_none()); f++) {
			assert(f < TR_INI_IDLE_FRAMES);
		}
		assert(f == TR_INI_IDLE_FRAMES && e.why == TR_INI_WALKAWAY && strcmp(e.name, "F8 ") == 0);
		/* ~6 s of real time at either panel rate */
		assert(TR_INI_IDLE_FRAMES == TR_HZ_FRAMES(240));
	}

	/* 6. Timeout: however busy the player is, the entry ends after
	 * TR_INI_TIMEOUT_FRAMES, committing what is shown. */
	{
		tr_initials_t e;
		uint32_t      f = 0;

		tr_ini_start(&e, "SOL", 4);
		while (tr_ini_step(&e, lane((f & 1u) ? 1 : -1))) { /* fidgeting, never confirming */
			f++;
			assert(f < TR_INI_TIMEOUT_FRAMES);
		}
		assert(f + 1u == TR_INI_TIMEOUT_FRAMES && e.why == TR_INI_TIMEOUT);
		assert(strlen(e.name) == 3u && e.pos < 3u);
		assert(TR_INI_TIMEOUT_FRAMES == TR_HZ_FRAMES(800) &&
		       TR_INI_IDLE_FRAMES < TR_INI_TIMEOUT_FRAMES);
	}

	/* 7. A default that is not three letters is padded / cut to three. */
	{
		tr_initials_t e;

		tr_ini_start(&e, "E8", 0);
		assert(strcmp(e.name, "E8 ") == 0);
		tr_ini_start(&e, "PIXEL", 0);
		assert(strcmp(e.name, "PIX") == 0);
		tr_ini_start(&e, "a?", 0); /* outside the alphabet: a space */
		assert(strcmp(e.name, "   ") == 0);
	}

	/* 8. Where the letters come from (main.c enter_high_score()): no HUD
	 * layer -> none (the default stands, no silent frozen frame); a camera
	 * build -> the tracker; else the tilt, if the IMU is up. */
	assert(tr_hs_entry_input(false, true, TR_MODE_ATTRACT) == TR_HS_IN_NONE);
	assert(tr_hs_entry_input(false, true, TR_MODE_VISION) == TR_HS_IN_NONE);
	assert(tr_hs_entry_input(true, true, TR_MODE_ATTRACT) == TR_HS_IN_TILT);
	assert(tr_hs_entry_input(true, true, TR_MODE_TILT) == TR_HS_IN_TILT);
	assert(tr_hs_entry_input(true, false, TR_MODE_ATTRACT) == TR_HS_IN_NONE);
	assert(tr_hs_entry_input(true, false, TR_MODE_VISION) == TR_HS_IN_VISION);

	/* 9. The run's celebration is armed from the table (score.h hs_top). */
	{
		tr_hiscore_t t;
		tr_score_t   s;

		tr_hs_init(&t);
		tr_score_init(&s);
		tr_score_run_start(&s);
		tr_hs_arm(&s, &t);
		assert(s.hs_top == 0u);
		(void)tr_hs_insert(&t, 4321u, "ACE");
		tr_hs_arm(&s, &t);
		assert(s.hs_top == 4321u);
	}

	/* 10. Tilt entry from raw board samples, as main.c runs it: a player
	 * who spells the farthest letters (18 symbols away either way) with
	 * held tilts, returning to level and flicking toward after each, is
	 * done well inside the timeout; the tilt then still counts them as
	 * present (a new run, not attract). A player who walks away mid-entry
	 * commits the letters, and the tilt reads it as a walk-away. */
	{
		const int16_t lvl = 0, edge = TR_TILT_EDGE_Q8 + 10, pitch = TR_TILT_EDGE_Q8 + 10;
		tr_initials_t e;
		tr_tilt_t     t;
		uint32_t      f    = 0;
		const char   *want = "IX4"; /* 'P'->'I' 7 back; 'R'->'X' 6 on; 'O'->'4' 18 either way */

		tr_tilt_init(&t);
		t.playing = true;
		tr_ini_start(&e, "PRO", 0);
		for (int i = 0; i < 3; i++) {
			const char *al = TR_INI_ALPHABET;
			int     at = (int)(strchr(al, e.name[i]) - al), to = (int)(strchr(al, want[i]) - al);
			int     fwd = (to - at + 37) % 37;
			int16_t x   = (int16_t)((fwd <= 37 - fwd ? 1 : -1) * TR_TILT_STEER_SIGN * edge);

			while (e.name[i] != want[i]) { /* hold until it shows */
				assert(tr_ini_tilt_frame(&e, &t, x, lvl));
				f++;
			}
			for (int k = 0; k < 3; k++, f++) {
				assert(tr_ini_tilt_frame(&e, &t, lvl, lvl)); /* back to level */
			}
			assert(e.name[i] == want[i]);
			bool more = tr_ini_tilt_frame(&e, &t, lvl, pitch); /* toward: next */

			f++;
			assert(more == (i < 2));
			for (int k = 0; more && k < 3; k++, f++) {
				assert(tr_ini_tilt_frame(&e, &t, lvl, lvl));
			}
		}
		assert(e.why == TR_INI_DONE && strcmp(e.name, want) == 0);
		printf("hiscore: worst-case letters entered in %.1f s (timeout %.0f s)\n",
		       (double)f / TR_PANEL_HZ,
		       (double)TR_INI_TIMEOUT_FRAMES / TR_PANEL_HZ);
		assert(f * 2u < TR_INI_TIMEOUT_FRAMES);
		assert(!tr_tilt_run_over(&t) && t.playing); /* still here: the next run */

		tr_tilt_init(&t);
		t.playing = true;
		tr_ini_start(&e, "SOL", 1);
		(void)tr_ini_tilt_frame(&e, &t, (int16_t)(TR_TILT_STEER_SIGN * edge), lvl);
		while (tr_ini_tilt_frame(&e, &t, lvl, lvl)) {
		}
		assert(e.why == TR_INI_WALKAWAY && strcmp(e.name, "TOL") == 0);
		assert(tr_tilt_run_over(&t) && !t.playing); /* gone: back to attract */
		/* a single held flick repeats only after TR_INI_REPEAT_DELAY */
		tr_ini_start(&e, "AAA", 0);
		tr_tilt_init(&t);
		for (uint32_t k = 0; k < TR_INI_REPEAT_DELAY; k++) {
			(void)tr_ini_tilt_frame(&e, &t, (int16_t)(TR_TILT_STEER_SIGN * edge), lvl);
		}
		assert(e.name[0] == 'C'); /* the flick, then the first repeat on the delay's frame */
	}

	printf("hiscore: table of %u, initials idle %u / timeout %u frames at %u Hz\n",
	       (unsigned)TR_HS_N,
	       (unsigned)TR_INI_IDLE_FRAMES,
	       (unsigned)TR_INI_TIMEOUT_FRAMES,
	       (unsigned)TR_PANEL_HZ);
	return 0;
}

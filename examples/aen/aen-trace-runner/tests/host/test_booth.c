/* tests/host/test_booth.c -- the booth flow a VISION build lives in: attract
 * with nobody there, the join lobby, a run, a run's end. Replays REAL 2026W36-0009
 * poses (tr_pose_empty_room.h: nobody in front of the camera, MoveNet's
 * low-confidence flicker; tr_pose_silicon.h: the maintainer standing)
 * through the same chain main.c runs every tick: tr_pose_box() ->
 * tr_presence_step() -> tr_attract_step() (+ tr_ini_vision_frame() at the
 * initials). One new pose per tick, the HP's ~30 Hz against the HE's 30 Hz.
 *
 * Silicon finding (build 4612458, 60 s, empty room): the game never reached
 * attract -- PLAY -> crash -> initials -> PLAY for the whole minute, no
 * input, because a lone flickering shoulder counted as a player. */
#include <assert.h>
#include <stdio.h>

#include "../../src/game/attract.h"
#include "../../src/game/hiscore.h"
#include "../../src/vision/pose.h"
#include "tr_pose_empty_room.h"
#include "tr_pose_silicon.h"

#define FRAME_W 400
#define FRAME_H 640
#define MINUTE  1800 /* ticks at 30 Hz */

typedef struct {
	tr_presence_t pr;
	tr_attract_t  a;
	tr_track_t    t;
	uint32_t      seq;
	bool          present;
} sim_t;

/* `attract`: a vision boot (attract, nobody has joined); else mid-run. */
static void sim_init(sim_t *s, bool attract)
{
	tr_presence_init(&s->pr);
	tr_attract_init(&s->a);
	tr_track_init(&s->t, FRAME_W, FRAME_H);
	tr_track_calibrate(&s->t, tr_pose_box(&tr_sil_stand[0]), FRAME_W);
	s->seq     = 0u;
	s->present = false;
	if (attract) {
		tr_attract_enter(&s->a);
	}
}

/* One HE tick on a freshly published pose (NULL: the HP published none). */
static tr_box_t sim_box(sim_t *s, const tr_pose_t *p)
{
	tr_box_t b = { .valid = false };

	if (p != NULL) {
		b = tr_pose_box(p);
		s->seq += 2u; /* the pslot seqlock's even generations */
		b.seq = s->seq;
	}
	s->present =
	    tr_presence_step(&s->pr, b, /*need_torso=*/s->a.active); /* the torso is the bar to join */
	return b;
}

static tr_attract_ev_t sim_tick(sim_t *s, const tr_pose_t *p)
{
	tr_box_t b = sim_box(s, p);

	(void)tr_track_update(&s->t, b);
	return tr_attract_step(&s->a, &s->t, s->present, 1u);
}

/* A synthetic empty room: every keypoint at 40, except a LONE shoulder at
 * TR_POSE_KP_MIN every 30th pose -- one partial detection a second. */
static tr_pose_t flicker_pose(int i)
{
	tr_pose_t p;

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] =
		    (tr_kp_t){ .x = (int16_t)(180 + 4 * k), .y = (int16_t)(480 + 3 * k), .score = 40u };
	}
	if (i % 30 == 0) {
		p.kp[TR_KP_LSHO].score = TR_POSE_KP_MIN;
	}
	return p;
}

/* Mid-run, the room goes empty (`empty` supplies its poses): the run ends
 * in attract within the presence window + TR_ATTRACT_ENTER_TICKS, exactly
 * once, and attract then holds for a minute with nobody joining. */
static void expect_attract_holds(sim_t *s, tr_pose_t (*empty)(int), const char *what)
{
	int entered_at = -1;

	for (int i = 0; i < MINUTE; i++) {
		tr_pose_t       p  = empty(i);
		tr_attract_ev_t ev = sim_tick(s, &p);

		if (ev == TR_ATTRACT_ENTERED) {
			assert(entered_at < 0);
			entered_at = i;
		}
		assert(ev != TR_ATTRACT_LEFT);
		if (entered_at >= 0) {
			assert(s->a.active && !tr_attract_joining(&s->a));
		}
	}
	printf("  %s: attract after %d ticks\n", what, entered_at);
	assert(entered_at >= 0 && entered_at < TR_PRESENT_WIN + TR_ATTRACT_ENTER_TICKS);
}

/* The standing player, close: hips out of frame (shoulders only). */
static tr_pose_t hipless(int i)
{
	tr_pose_t p = tr_sil_stand[i % TR_SIL_STAND_N];

	p.kp[TR_KP_LHIP].score     = 0u;
	p.kp[TR_KP_LHIP + 1].score = 0u;
	return p;
}

static tr_pose_t silicon_empty(int i)
{
	return tr_empty_room[i % TR_EMPTY_ROOM_N];
}

int main(void)
{
	/* 1. Empty room, partial detections: a lone shoulder once a second is
	 * nobody -- attract within the timeout, and it stays. (Was: every
	 * lone shoulder reset the idle count; attract never came.) */
	{
		sim_t s;

		sim_init(&s, false);
		expect_attract_holds(&s, flicker_pose, "flicker room");
	}

	/* 2. The recorded silicon empty room (low-confidence flicker, 13 of 31
	 * poses with a shoulder at 77): attract, not PLAY. From a run, and
	 * from a vision boot -- the boot never joins anybody. */
	{
		sim_t s;

		sim_init(&s, false);
		expect_attract_holds(&s, silicon_empty, "silicon empty room");

		sim_init(&s, true);
		for (int i = 0; i < MINUTE; i++) {
			assert(sim_tick(&s, &tr_empty_room[i % TR_EMPTY_ROOM_N]) == TR_ATTRACT_STAY);
			assert(s.a.active && !tr_attract_joining(&s.a) && !s.present);
			assert(!tr_presence_is(&s.pr, false)); /* nor main.c's STEP BACK hint */
		}
		/* No pose at all (the HP not publishing) is nobody too. */
		for (int i = 0; i < MINUTE; i++) {
			assert(sim_tick(&s, NULL) == TR_ATTRACT_STAY && s.a.active);
		}
	}

	/* 3. A player steps in: attract -> the join lobby ("STEP INTO VIEW",
	 * tr_attract_joining()) for TR_ATTRACT_JOIN_TICKS of presence -> PLAY
	 * (TR_ATTRACT_LEFT), and then they keep playing. (Was: the first box
	 * started a run, no lobby.) */
	{
		sim_t s;
		int   left_at = -1, joining = 0;

		sim_init(&s, true);
		for (int i = 0; i < 300 && left_at < 0; i++) {
			tr_attract_ev_t ev = sim_tick(&s, &tr_sil_stand[i % TR_SIL_STAND_N]);

			if (ev == TR_ATTRACT_LEFT) {
				left_at = i;
			} else {
				assert(ev == TR_ATTRACT_STAY && s.a.active);
				joining += tr_attract_joining(&s.a);
			}
		}
		printf("  player steps in: lobby %d ticks, PLAY at tick %d\n", joining, left_at);
		assert(left_at >= TR_ATTRACT_JOIN_TICKS - 1 &&
		       left_at < TR_PRESENT_WIN + TR_ATTRACT_JOIN_TICKS);
		assert(joining == TR_ATTRACT_JOIN_TICKS - 1);
		assert(!s.a.active);
		for (int i = 0; i < MINUTE; i++) {
			assert(sim_tick(&s, &tr_sil_stand[i % TR_SIL_STAND_N]) == TR_ATTRACT_STAY &&
			       !s.a.active);
		}
	}

	/* 4. A player leaves mid-run: the run ends (TR_ATTRACT_ENTERED) and
	 * attract holds -- the recorded empty room behind them. (Was: the
	 * flicker kept them "there"; the run played on without anyone.) */
	{
		sim_t s;

		sim_init(&s, false);
		for (int i = 0; i < 300; i++) {
			assert(sim_tick(&s, &tr_sil_stand[i % TR_SIL_STAND_N]) == TR_ATTRACT_STAY &&
			       !s.a.active);
		}
		expect_attract_holds(&s, silicon_empty, "player leaves mid-run");
	}

	/* 5. A lobby the player abandons half-way resets: no run starts on
	 * the strength of an earlier, broken presence. */
	{
		sim_t s;

		sim_init(&s, true);
		for (int i = 0; i < TR_PRESENT_WIN + TR_ATTRACT_JOIN_TICKS / 2; i++) {
			assert(sim_tick(&s, &tr_sil_stand[i % TR_SIL_STAND_N]) == TR_ATTRACT_STAY);
		}
		assert(tr_attract_joining(&s.a));
		for (int i = 0; i < MINUTE; i++) {
			assert(sim_tick(&s, &tr_empty_room[i % TR_EMPTY_ROOM_N]) == TR_ATTRACT_STAY);
		}
		assert(s.a.active && !tr_attract_joining(&s.a));
	}

	/* 6. Initials with nobody there: the empty room's flicker picks no
	 * letters, the entry idles out, and the run ends in attract -- not a
	 * new run. (Was: the flicker's intent fed the entry, and every run
	 * over restarted PLAY.) A player still there plays again. */
	{
		sim_t         s;
		tr_initials_t e;
		int           frames = 0;

		sim_init(&s, false);
		tr_ini_start(&e, "AAA", 0);
		for (bool more = true; more; frames++) {
			tr_box_t b = sim_box(&s, &tr_empty_room[frames % TR_EMPTY_ROOM_N]);

			more = tr_ini_vision_frame(&e, &s.t, b, s.present);
			assert(frames <= TR_INI_TIMEOUT_FRAMES);
		}
		printf("  initials, nobody: over after %d frames (why %u)\n", frames, e.why);
		assert(e.why == TR_INI_WALKAWAY && frames == TR_INI_IDLE_FRAMES);
		assert(e.name[0] == 'A' && e.name[1] == 'A' && e.name[2] == 'A');
		assert(tr_attract_run_over(&s.a, s.present) && s.a.active);
		for (int i = 0; i < MINUTE; i++) {
			assert(sim_tick(&s, &tr_empty_room[i % TR_EMPTY_ROOM_N]) == TR_ATTRACT_STAY &&
			       s.a.active);
		}

		sim_init(&s, false);
		for (int i = 0; i < TR_PRESENT_WIN; i++) {
			(void)sim_box(&s, &tr_sil_stand[i % TR_SIL_STAND_N]);
		}
		assert(s.present && !tr_attract_run_over(&s.a, s.present) && !s.a.active);
	}

	/* 7. The presence rule itself: counted per NEW pose (the HE re-reads
	 * one pose every tick until the HP publishes again), and the real
	 * standing player is present through the recording once the window
	 * has filled. */
	{
		tr_presence_t pr;
		tr_box_t      torso = tr_pose_box(&tr_sil_stand[0]);

		assert(torso.valid && torso.h > 0);
		tr_presence_init(&pr);
		torso.seq = 2u;
		for (int i = 0; i < 100; i++) {
			assert(!tr_presence_step(&pr, torso, true)); /* one pose, however often re-read */
		}
		for (int i = 0; i < TR_SIL_STAND_N; i++) {
			tr_box_t b = tr_pose_box(&tr_sil_stand[i]);

			b.seq        = (uint32_t)(4 + 2 * i);
			bool present = tr_presence_step(&pr, b, true);

			assert(present == (i + 1 >= TR_PRESENT_MIN));
		}
	}

	/* 8. Mid-run, the player steps close and their hips leave the frame:
	 * the tracker still follows the shoulders, so the run plays on. (Was:
	 * the torso rule ended the run ~90 ticks later.) The same player
	 * cannot JOIN like that: attract holds, no lobby, and the window that
	 * main.c's STEP BACK hint reads says someone is there. */
	{
		sim_t s;

		sim_init(&s, false);
		for (int i = 0; i < 150; i++) {
			assert(sim_tick(&s, &tr_sil_stand[i % TR_SIL_STAND_N]) == TR_ATTRACT_STAY);
		}
		for (int i = 0; i < MINUTE; i++) {
			tr_pose_t p = hipless(i);

			assert(sim_tick(&s, &p) == TR_ATTRACT_STAY && !s.a.active && s.present);
		}

		sim_init(&s, true);
		for (int i = 0; i < MINUTE; i++) {
			tr_pose_t p = hipless(i);

			assert(sim_tick(&s, &p) == TR_ATTRACT_STAY && s.a.active && !tr_attract_joining(&s.a));
			assert(!s.present);
		}
		assert(tr_presence_is(&s.pr, false));
	}

	/* 9. An absent pose re-read is one pose too: capture_box() tags a
	 * no-shoulder pose invalid WITH its seq, and it counts once, like a
	 * torso -- else a slow HP biases the window toward "nobody". */
	{
		tr_presence_t pr;
		tr_box_t      none = { .valid = false, .seq = 1000u };

		tr_presence_init(&pr);
		for (int i = 0; i < TR_PRESENT_WIN; i++) {
			tr_box_t b = tr_pose_box(&tr_sil_stand[0]);

			b.seq = (uint32_t)(2 + 2 * i);
			assert(tr_presence_step(&pr, b, true) == (i + 1 >= TR_PRESENT_MIN));
		}
		for (int i = 0; i < 100; i++) {
			assert(tr_presence_step(&pr, none, true)); /* 14 of 15, however often re-read */
		}
	}

	return 0;
}

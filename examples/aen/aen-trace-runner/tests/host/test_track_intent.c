/* tests/host/test_track_intent.c -- body intent from the torso, in the
 * silicon geometry: UPRIGHT 400x640 (portrait) frame, legs out of frame.
 *
 * Every scenario opens the way main.c does: tr_track_calibrate() with a box
 * from the 2 s title screen that is NOT the player standing ready (a passer-by,
 * someone seated or further back). The old tracker took its stance baseline
 * from that box once and then mostly froze it -- the maintainer's "it always
 * sees me jumping even though I don't jump". Then: the recorded silicon
 * replays (tr_pose_silicon.h) and the synthetic sequences of the ruling. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "tr_pose_silicon.h"

#define FW 400
#define FH 640
#define HI 110 /* a confident keypoint score (TR_POSE_KP_MIN is 77) */

/* A person seen from the waist-ish up: torso centre (cx, cy), torso length s
 * (shoulder-mid to hip-mid). Anything below the frame bottom scores low, the
 * way MoveNet reports a keypoint it cannot see. */
static tr_pose_t person(int cx, int cy, int s)
{
	static const struct {
		int16_t dx, dy; /* in % of s, from the torso centre */
	} body[TR_POSE_KP] = {
		{ 0, -85 },  { 5, -90 },   { -5, -90 },  { 10, -88 }, { -10, -88 }, { 30, -50 },
		{ -30, -50 }, { 38, -5 },  { -38, -5 },  { 36, 35 },  { -36, 35 },  { 18, 50 },
		{ -18, 50 }, { 18, 110 },  { -18, 110 }, { 18, 170 }, { -18, 170 },
	};
	tr_pose_t p;

	for (int k = 0; k < TR_POSE_KP; k++) {
		int y = cy + s * body[k].dy / 100;

		p.kp[k].x     = (int16_t)(cx + s * body[k].dx / 100);
		p.kp[k].y     = (int16_t)(y < FH ? y : FH - 1);
		p.kp[k].score = (uint8_t)(y < FH ? HI : 20);
	}
	return p;
}

static tr_pose_t jitter(tr_pose_t p, int amp)
{
	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k].x = (int16_t)(p.kp[k].x + (rand() % (2 * amp + 1)) - amp);
		p.kp[k].y = (int16_t)(p.kp[k].y + (rand() % (2 * amp + 1)) - amp);
	}
	return p;
}

static tr_intent_t feed(tr_track_t *t, tr_pose_t p)
{
	return tr_track_update(t, tr_pose_box(&p));
}

/* The box main.c's TR_INPUT_NPU capture_box() returns: the pose and the
 * pslot seq it was published under. */
static tr_box_t seq_box(tr_pose_t p, uint32_t seq)
{
	tr_box_t b = tr_pose_box(&p);

	b.seq = seq;
	return b;
}

/* main.c's boot: calibrate on whatever the title screen last saw -- here a
 * smaller person further down the frame than the player who then plays. */
static void boot(tr_track_t *t)
{
	tr_pose_t passer = person(200, 600, 55);

	tr_track_init(t, FW, FH);
	tr_track_calibrate(t, tr_pose_box(&passer), FW);
	assert(t->calibrated);
}

/* Standing still with keypoint jitter: nothing. Returns the frames fed. */
static void stand(tr_track_t *t, int cx, int cy, int s, int frames)
{
	for (int n = 0; n < frames; n++) {
		tr_intent_t i = feed(t, jitter(person(cx, cy, s), 3));

		assert(i.source == TR_INPUT_VISION && !i.jump && !i.duck && i.lane_delta == 0);
	}
}

/* 1. The recorded silicon: walking toward the camera, and standing. The
 * samples are ~4.5 pose frames apart; each is held for 4 frames, which makes
 * every move a harsher step than the real one. */
static void silicon_replay(void)
{
	tr_track_t t;
	int        jumps = 0, ducks = 0, valid = 0;

	boot(&t);
	for (int n = 0; n < TR_SIL_APPROACH_N; n++) {
		for (int r = 0; r < 4; r++) {
			tr_intent_t i = tr_track_update(&t, tr_pose_box(&tr_sil_approach[n]));

			jumps += i.jump;
			ducks += i.duck;
			valid += i.source == TR_INPUT_VISION;
		}
	}
	printf("silicon approach: %d valid frames, %d jump, %d duck\n", valid, jumps, ducks);
	assert(valid > 100 && jumps == 0);

	boot(&t);
	jumps = ducks = 0;
	for (int n = 0; n < TR_SIL_STAND_N; n++) {
		for (int r = 0; r < 4; r++) {
			tr_intent_t i = tr_track_update(&t, tr_pose_box(&tr_sil_stand[n]));

			jumps += i.jump;
			ducks += i.duck;
			assert(i.lane_delta == 0); /* torso x ~215: the centre lane (133..266) */
		}
	}
	printf("silicon stand: %d jump, %d duck\n", jumps, ducks);
	assert(jumps == 0 && ducks == 0);
}

/* 2. Standing still, jittering: nothing, ever. */
static void standing_still(void)
{
	tr_track_t t;

	boot(&t);
	stand(&t, 200, 560, 90, 150);
}

/* 3. A real jump: torso up 0.4 s for 300 ms (9 frames), scale constant. JUMP
 * within 2 frames, held at least TR_TRACK_HOLD_MIN, released after landing,
 * and the landing is no duck. */
static void real_jump(void)
{
	tr_track_t t;
	int        first = -1, last = -1;

	boot(&t);
	stand(&t, 200, 560, 90, 40);
	for (int n = 0; n < 40; n++) {
		int         cy = n < 9 ? 560 - 36 : 560;
		tr_intent_t i  = feed(&t, jitter(person(200, cy, 90), 3));

		assert(!i.duck && i.lane_delta == 0);
		if (i.jump) {
			first = first < 0 ? n : first;
			last  = n;
		}
	}
	printf("real jump: frames %d..%d\n", first, last);
	assert(first >= 0 && first <= 1);         /* within 2 frames of the rise */
	assert(last - first + 1 >= TR_TRACK_HOLD_MIN); /* the lamp is visible */
	assert(last < 9 + TR_TRACK_HOLD_MIN);      /* released once back down */
	stand(&t, 200, 560, 90, 30);

	/* Held up (a step onto something, a tiptoe): capped, then neutral. */
	int held = 0;
	for (int n = 0; n < 90; n++) {
		held += feed(&t, jitter(person(200, 520, 90), 3)).jump;
	}
	assert(held > 0 && held <= TR_TRACK_JUMP_MAX);
}

/* 4. A duck: the torso drops 0.45 s (the hips leave the frame: the
 * shoulder-only fallback), held for 20 frames. */
static void duck(void)
{
	tr_track_t t;
	int        ducks = 0;

	boot(&t);
	stand(&t, 200, 560, 90, 40);
	for (int n = 0; n < 20; n++) {
		tr_intent_t i = feed(&t, jitter(person(200, 560 + 40, 90), 3));

		assert(!i.jump && i.lane_delta == 0);
		ducks += i.duck;
	}
	printf("duck: %d of 20\n", ducks);
	assert(ducks >= 17);
	for (int n = 0; n < 10; n++) {
		assert(!feed(&t, jitter(person(200, 560, 90), 3)).jump);
	}
	stand(&t, 200, 560, 90, 30);
}

/* 5. A lateral step: the player's left is screen left (the mirror is in the
 * pixels). One lane per crossing, nothing else. */
static void lateral(void)
{
	tr_track_t t;
	int        sum = 0;

	boot(&t);
	stand(&t, 200, 560, 90, 30);
	for (int n = 0; n < 30; n++) {
		tr_intent_t i = feed(&t, jitter(person(60, 560, 90), 3));

		assert(!i.jump && !i.duck);
		sum += i.lane_delta;
	}
	assert(sum == -1);
	for (int n = 0; n < 30; n++) {
		tr_intent_t i = feed(&t, jitter(person(340, 560, 90), 3));

		assert(!i.jump && !i.duck);
		sum += i.lane_delta;
	}
	assert(sum == +1);
}

/* 6. Walking toward the camera: scale x2 in 1 s, the torso centre rising
 * the way it did on silicon (dcy ~ 3.2 ds, tr_sil_approach). A torso that
 * rises this fast looks like a jump on position alone; it is the growth
 * (|dh| > 0.25 dcy) and the widening shoulders that say "closer". Nothing. */
static void approach(void)
{
	tr_track_t t;

	boot(&t);
	stand(&t, 200, 560, 90, 30);
	for (int n = 0; n <= 30; n++) {
		int         s = 90 + 3 * n;
		tr_intent_t i = feed(&t, jitter(person(200, 560 - 32 * (s - 90) / 10, s), 3));

		assert(!i.jump && !i.duck);
	}
	stand(&t, 200, 272, 180, 30);
}

/* 7. A second person swaps in -- after a gap, and straight in with no gap --
 * taller and closer. Re-baseline, nothing false; a jump still works after. */
static void swap(void)
{
	tr_track_t t;

	boot(&t);
	stand(&t, 200, 580, 80, 40);
	for (int n = 0; n < 15; n++) {
		assert(tr_track_update(&t, (tr_box_t){ .valid = false }).source == TR_INPUT_NONE);
	}
	stand(&t, 200, 500, 120, 60);
	stand(&t, 200, 580, 80, 60); /* and straight back, no gap */

	bool jumped = false;
	for (int n = 0; n < 9; n++) {
		jumped |= feed(&t, jitter(person(200, 580 - 32, 80), 3)).jump;
	}
	assert(jumped);
}

/* 8. A 12 % step BACK (the 2026W36-0009 probe: 298 of 300 standing frames read
 * as one endless duck). The torso shrinks and sinks in the frame like a
 * squat, but the shoulders narrow with it: a squat or lean keeps them.
 * 12 % is only 2 % past the width gate, inside this jitter's reach (about a
 * third of seeds let one duck through), so what is guaranteed is the cap:
 * at most one TR_TRACK_DUCK_MAX lamp, then nothing -- the baseline has
 * followed, and a real duck from there still works. */
static void retreat(void)
{
	tr_track_t t;
	int        ducks = 0, late = 0;

	boot(&t);
	stand(&t, 200, 560, 90, 40);
	for (int n = 0; n < 300; n++) {
		tr_intent_t i = feed(&t, jitter(person(200, 590, 79), 3));

		assert(!i.jump && i.lane_delta == 0);
		ducks += i.duck;
		late += n >= 100 && i.duck;
	}
	printf("retreat: %d duck of 300\n", ducks);
	assert(ducks <= TR_TRACK_DUCK_MAX + 1 && late == 0);
	for (int n = 0; n < 10; n++) {
		ducks += feed(&t, jitter(person(200, 590 + 36, 79), 3)).duck;
	}
	assert(ducks >= 7);
}

/* 9. A duck held far past any obstacle (~5 s): the lamp goes out after
 * TR_TRACK_DUCK_MAX and the crouch becomes the baseline -- standing up
 * again is neither a jump nor a duck. */
static void long_duck(void)
{
	tr_track_t t;
	int        ducks = 0, late = 0;

	boot(&t);
	stand(&t, 200, 560, 90, 40);
	for (int n = 0; n < 150; n++) {
		tr_intent_t i = feed(&t, jitter(person(200, 600, 90), 3));

		assert(!i.jump);
		ducks += i.duck;
		late += n >= 90 && i.duck;
	}
	printf("long duck: %d of 150 frames\n", ducks);
	assert(ducks >= TR_TRACK_DUCK_MAX - 2 && ducks <= TR_TRACK_DUCK_MAX + 1 && late == 0);
	for (int n = 0; n < 60; n++) {
		tr_intent_t i = feed(&t, jitter(person(200, 560, 90), 3));

		assert(!i.jump && !i.duck);
	}
}

/* 10. main.c hands the tracker the LAST accepted pose on every HE tick,
 * repeated until the HP publishes a new seq. One outlier pose (a torso
 * yanked up 0.45 s -- a misfire, a hand across the shoulders) re-read on
 * two ticks is still one pose: no debounce, no jump. */
static void repeated_seq(void)
{
	tr_track_t t;
	uint32_t   seq = 2u;

	boot(&t);
	for (int n = 0; n < 40; n++) {
		assert(!tr_track_update(&t, seq_box(jitter(person(200, 560, 90), 3), seq += 2u)).jump);
	}
	tr_box_t outlier = seq_box(person(200, 560 - 40, 90), seq += 2u);

	for (int r = 0; r < 3; r++) {
		tr_intent_t i = tr_track_update(&t, outlier);

		assert(!i.jump && !i.duck);
	}
	for (int n = 0; n < 20; n++) {
		tr_box_t b = seq_box(jitter(person(200, 560, 90), 3), seq += 2u);

		for (int r = 0; r < 2; r++) {
			tr_intent_t i = tr_track_update(&t, b);

			assert(!i.jump && !i.duck);
		}
	}
}

/* 11. A torso 3 px long (the hips mis-found a few px under the shoulders):
 * below TR_TRACK_MIN_SCALE, whichever branch measured it -- lane only. */
static void tiny_scale(void)
{
	tr_track_t t;

	boot(&t);
	for (int n = 0; n < 300; n++) {
		tr_box_t    b = { .x = (int16_t)(175 + rand() % 5 - 2), .y = (int16_t)(500 + rand() % 7 - 3),
				  .w = (int16_t)(50 + rand() % 5 - 2), .h = 3, .confidence = HI, .valid = true };
		tr_intent_t i = tr_track_update(&t, b);

		assert(i.source == TR_INPUT_VISION && !i.jump && !i.duck);
	}
}

int main(int argc, char **argv)
{
	static void (*const scenario[])(void) = { silicon_replay, standing_still, real_jump, duck,
						  lateral,        approach,       swap,      retreat,
						  long_duck,      repeated_seq,   tiny_scale };
	int                   only            = argc > 1 ? atoi(argv[1]) : -1; /* one scenario, by index */

	srand(11);
	for (int k = 0; k < (int)(sizeof scenario / sizeof scenario[0]); k++) {
		if (only < 0 || only == k) {
			scenario[k]();
		}
	}
	return 0;
}

/* tests/host/test_arms.c -- the arm-raise controls (src/vision/arms.h): the
 * gesture classifier on synthetic keypoint sets, through the same path the
 * firmware takes (tr_pose_box() measures the wrists, tr_track_update() turns
 * the edges into a tr_intent_t).
 *
 * Built twice by runner.sh: as is (TR_CAM_MIRROR=1, the release build: the
 * player looks at a selfie) and with -DTR_CAM_MIRROR=0 (a camera that sees
 * the player face to face). The scene is the same physical player either
 * way; only the picture of them changes, and the player's LEFT arm must read
 * as LEFT in both. The chain from a sensor flip through the rotation is in
 * test_cam_mirror.c. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../src/vision/cam_rot.h" /* TR_CAM_MIRROR */
#include "../../src/vision/pose.h"
#include "../../src/vision/track.h"

#define UW 400 /* the upright portrait frame; the classifier is frame-size free */
#define UH 640
#define SW 80  /* shoulder width, px */
#define HI 200 /* a confident keypoint score (TR_ARM_KP_MIN is 77) */

typedef enum { DOWN, LEVEL, UP } arm_t;

/* One wrist, as an offset from its shoulder in shoulder widths x 100:
 * DOWN -80 (hanging), LEVEL 0 (held out sideways), UP +120 (raised). */
static int rise_pct(arm_t a)
{
	return a == UP ? 120 : a == LEVEL ? 0 : -80;
}

/* The player facing the camera as it is DRAWN under TR_CAM_MIRROR, with MoveNet's labels
 * (the keypoints at larger x are "left"). `left` / `right` are the player's
 * own arms. */
static int g_left_wri,
    g_right_wri; /* the keypoints of the player's own wrists, last player_pct() */

static tr_pose_t player_pct(int left_pct, int right_pct)
{
	enum { CX = UW / 2, CY = 220 };
	/* x offset of the player's LEFT side on screen: right of centre when the
	 * camera faces the player (no mirror), left of centre in a selfie. */
	int       ls = TR_CAM_MIRROR ? -1 : +1;
	tr_pose_t p  = { 0 };

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ CX, CY, HI };
	}
	/* MoveNet's "left" keypoints are whichever sit at larger x. */
	int l_idx_is_player_left = ls > 0;
	int a = l_idx_is_player_left ? TR_KP_LSHO : TR_KP_RSHO; /* the player's left shoulder idx */
	int b = l_idx_is_player_left ? TR_KP_RSHO : TR_KP_LSHO;

	p.kp[a]     = (tr_kp_t){ (int16_t)(CX + ls * SW / 2), CY, HI };
	p.kp[b]     = (tr_kp_t){ (int16_t)(CX - ls * SW / 2), CY, HI };
	g_left_wri  = a + 4;
	g_right_wri = b + 4;
	p.kp[a + 4] = (tr_kp_t){ (int16_t)(CX + ls * SW), (int16_t)(CY - left_pct * SW / 100), HI };
	p.kp[b + 4] = (tr_kp_t){ (int16_t)(CX - ls * SW), (int16_t)(CY - right_pct * SW / 100), HI };
	p.kp[TR_KP_LHIP] = (tr_kp_t){ (int16_t)(CX + SW / 3), CY + 150, HI };
	p.kp[TR_KP_RHIP] = (tr_kp_t){ (int16_t)(CX - SW / 3), CY + 150, HI };
	return p;
}

static tr_pose_t player(arm_t left, arm_t right)
{
	return player_pct(rise_pct(left), rise_pct(right));
}

static tr_track_t g_t;

static void start(void)
{
	tr_pose_t p = player(DOWN, DOWN);

	tr_track_init(&g_t, UH);
	tr_track_calibrate(&g_t, tr_pose_box(&p));
	assert(g_t.calibrated);
}

/* Feed `n` new poses of the same keypoints; the lane steps and jumps they
 * produced in total. */
typedef struct {
	int lane, jumps, steps; /* sum of lane_delta, jump events, ticks with a lane step */
} seen_t;

static seen_t feed(tr_pose_t p, int n)
{
	seen_t s = { 0 };

	for (int k = 0; k < n; k++) {
		tr_intent_t in = tr_track_update(&g_t, tr_pose_box(&p));

		assert(in.source == TR_INPUT_VISION);
		s.lane += in.lane_delta;
		s.jumps += in.jump;
		s.steps += in.lane_delta != 0;
	}
	return s;
}

#define HOLD 20 /* poses: well past TR_ARM_SETTLE_POSES */

int main(void)
{
	seen_t s;

	/* 1. The left arm up is ONE lane left, and not again while it stays up. */
	start();
	s = feed(player(DOWN, DOWN), 5);
	assert(s.lane == 0 && s.jumps == 0);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1 && s.steps == 1 && s.jumps == 0);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0 && s.jumps == 0); /* held: edge-triggered, no repeat */

	/* ...until it has been lowered: then the next raise is a new step. */
	s = feed(player(DOWN, DOWN), 3);
	assert(s.lane == 0);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1 && s.steps == 1);

	/* 2. The right arm up is one lane right. */
	start();
	feed(player(DOWN, DOWN), 5);
	s = feed(player(DOWN, UP), HOLD);
	assert(s.lane == +1 && s.steps == 1 && s.jumps == 0);
	s = feed(player(DOWN, UP), HOLD);
	assert(s.lane == 0);

	/* 3. Both arms up together is a jump and NOTHING else: no lane step, ever,
	 * however far apart (within the window) the two wrists cross the line. */
	for (int lag = 0; lag < TR_ARM_SETTLE_POSES; lag++) {
		for (int first_left = 0; first_left < 2; first_left++) {
			start();
			feed(player(DOWN, DOWN), 5);
			s         = feed(first_left ? player(UP, DOWN) : player(DOWN, UP), lag);
			seen_t s2 = feed(player(UP, UP), HOLD);

			assert(s.lane + s2.lane == 0 && s.steps + s2.steps == 0);
			assert(s.jumps + s2.jumps == 1);
		}
	}
	/* Held up, then one arm dropped: no step on the arm that stayed, no jump
	 * repeat. */
	start();
	feed(player(DOWN, DOWN), 5);
	s = feed(player(UP, UP), HOLD);
	assert(s.jumps == 1 && s.lane == 0);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.jumps == 0 && s.lane == 0);
	s = feed(player(DOWN, DOWN), 3);
	s = feed(player(UP, UP), HOLD); /* lowered, so it counts again */
	assert(s.jumps == 1 && s.lane == 0);

	/* A staggered both-arms raise, the second arm later than the window while the first is
	 * still up: the first arm has stepped its lane, the second makes it the jump. */
	for (int first_left = 0; first_left < 2; first_left++) {
		start();
		feed(player(DOWN, DOWN), 5);
		s = feed(first_left ? player(UP, DOWN) : player(DOWN, UP), 3 * TR_ARM_SETTLE_POSES);
		assert(s.lane == (first_left ? -1 : +1) && s.jumps == 0);
		s = feed(player(UP, UP), HOLD);
		assert(s.lane == 0 && s.jumps == 1);
	}
	/* ...but the second arm after the first came DOWN is its own lane step. */
	start();
	feed(player(DOWN, DOWN), 5);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1);
	feed(player(DOWN, DOWN), 3);
	s = feed(player(DOWN, UP), HOLD);
	assert(s.lane == +1 && s.jumps == 0);

	/* 4. Hysteresis: an arm hovering between the lowered and the raised line
	 * neither re-arms nor fires. Held level with the shoulder, it re-arms. */
	start();
	feed(player(DOWN, DOWN), 5);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1);
	assert(TR_ARM_DOWN_PCT < 35 && 35 < TR_ARM_UP_PCT);
	s = feed(player_pct(35, -80), HOLD);
	assert(s.lane == 0);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0); /* never dropped below the low line: still the same raise */
	feed(player(LEVEL, DOWN), 3);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1);

	/* 5. Low confidence is no action. A wrist below the gate: that arm is
	 * unknown. A shoulder below the gate: both are. */
	start();
	feed(player(DOWN, DOWN), 5);
	{
		tr_pose_t p = player(UP, DOWN);

		p.kp[TR_KP_LWRI].score = TR_ARM_KP_MIN - 1;
		p.kp[TR_KP_RWRI].score = TR_ARM_KP_MIN - 1;
		s                      = feed(p, HOLD);
		assert(s.lane == 0 && s.jumps == 0);

		p                      = player(UP, UP);
		p.kp[TR_KP_LSHO].score = TR_ARM_KP_MIN - 1;
		s                      = feed(p, HOLD);
		assert(s.lane == 0 && s.jumps == 0);

		/* The same gate through the box: the whole pose is untrusted. */
		p = player(UP, DOWN);
		for (int k = 0; k < TR_POSE_KP; k++) {
			p.kp[k].score = 20;
		}
		tr_intent_t in = tr_track_update(&g_t, tr_pose_box(&p));

		assert(in.source == TR_INPUT_NONE && in.lane_delta == 0 && !in.jump);
	}
	/* An arm that fades out while it is up does not step again when it comes back. */
	start();
	feed(player(DOWN, DOWN), 5);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1);
	{
		tr_pose_t p = player(UP, DOWN);

		p.kp[TR_KP_LWRI].score = 0;
		p.kp[TR_KP_RWRI].score = 0;
		feed(p, 3);
	}
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0);

	/* 6. Side-on (the shoulders on one point): no left or right to tell. */
	start();
	feed(player(DOWN, DOWN), 5);
	{
		tr_pose_t p = player(UP, DOWN);

		p.kp[TR_KP_LSHO].x = p.kp[TR_KP_RSHO].x + TR_ARM_MIN_SHOULDER_PX - 1;
		s                  = feed(p, HOLD);
		assert(s.lane == 0 && s.jumps == 0);
	}

	/* 7. A pose re-read on several ticks is one pose: the edge counts once
	 * (the HE re-reads the last pslot pose every tick until a new seq). */
	start();
	{
		uint32_t  seq = 2u;
		tr_pose_t dn = player(DOWN, DOWN), up = player(UP, DOWN);
		int       lane = 0;

		for (int k = 0; k < 5; k++) {
			tr_box_t b = tr_pose_box(&dn);

			b.seq = seq += 2u;
			for (int r = 0; r < 3; r++) {
				lane += tr_track_update(&g_t, b).lane_delta;
			}
		}
		/* The raise sits for 2 poses, each re-read 3 times: fewer than the settle window.
		 * It must not fire early on the re-reads. */
		for (int k = 0; k < TR_ARM_SETTLE_POSES - 1; k++) {
			tr_box_t b = tr_pose_box(&up);

			b.seq = seq += 2u;
			for (int r = 0; r < 3; r++) {
				lane += tr_track_update(&g_t, b).lane_delta;
			}
		}
		assert(lane == 0);
		tr_box_t b = tr_pose_box(&up);

		b.seq = seq += 2u;
		for (int r = 0; r < 3; r++) {
			lane += tr_track_update(&g_t, b).lane_delta;
		}
		assert(lane == -1);
	}

	/* 8. An arm already up when the tracker starts (a joiner waving) is spent
	 * until lowered; and so is one after a resync (a pause, a run reset). */
	start();
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0);
	feed(player(DOWN, DOWN), 3);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == -1);
	tr_track_resync(&g_t);
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0);

	/* 9. Hands up from the torso box's point of view change nothing about the
	 * torso: raising both arms is no duck, and the box ignores the wrists. */
	{
		tr_pose_t dn = player(DOWN, DOWN), up = player(UP, UP);
		tr_box_t  a = tr_pose_box(&dn), b = tr_pose_box(&up);

		assert(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
		assert(a.arm_raise[TR_ARM_LEFT] < 0 && b.arm_raise[TR_ARM_LEFT] >= TR_ARM_UP_PCT);
		assert(b.arm_raise[TR_ARM_RIGHT] >= TR_ARM_UP_PCT);
	}

	/* 11. The arm clock is TIME. A wrist that leaves the top of the frame (unjudged)
	 * or is parked in the hysteresis band after its rise never confirms the
	 * gesture; the other arm seconds later is a lane step, not a jump. */
	for (int how = 0; how < 2; how++) {
		start();
		feed(player(DOWN, DOWN), 5);
		feed(player(UP, DOWN), 1); /* the left wrist crosses the line... */
		if (how == 0) {
			tr_pose_t gone = player(UP, DOWN);

			gone.kp[g_left_wri].score = 0; /* ...and goes out of frame */
			feed(gone, 2 * TR_ARM_SETTLE_POSES);
		} else {
			feed(player_pct(35, -80), 2 * TR_ARM_SETTLE_POSES); /* ...and parks in the band */
		}
		s = feed(player(DOWN, UP), HOLD);
		assert(s.lane == +1 && s.jumps == 0);
	}
	/* The first judged pose primes PER ARM: an all-unknown pose primes nothing, so an arm
	 * that is already up when it is first seen is spent, not a step. */
	start();
	{
		tr_pose_t blind = player(DOWN, DOWN);

		blind.kp[g_left_wri].score  = 0;
		blind.kp[g_right_wri].score = 0;
		feed(blind, 1);
	}
	s = feed(player(UP, DOWN), HOLD);
	assert(s.lane == 0 && s.jumps == 0);
	/* ...and one arm judged first does not prime the other. */
	start();
	{
		tr_pose_t half = player(DOWN, UP);

		half.kp[g_left_wri].score = 0;
		feed(half, 3);
	}
	s = feed(player(UP, UP), HOLD); /* the left is first judged only now, already up: spent */
	assert(s.lane == 0 && s.jumps == 0);
	feed(player(DOWN, DOWN), 3);
	s = feed(player(UP, UP), HOLD); /* both lowered, then both raised: a jump */
	assert(s.lane == 0 && s.jumps == 1);

	/* 10. Left is the player's left, in this build's picture of them. */
	{
		tr_pose_t p = player(UP, DOWN);
		tr_box_t  b = tr_pose_box(&p);

		assert(b.arm_raise[TR_ARM_LEFT] >= TR_ARM_UP_PCT && b.arm_raise[TR_ARM_RIGHT] < 0);
		/* ...which is the screen's LEFT half under a selfie mirror and the RIGHT half
		 * without one: find the raised wrist and see which side it is drawn on. */
		int up_x =
		    p.kp[TR_KP_LWRI].y < p.kp[TR_KP_RWRI].y ? p.kp[TR_KP_LWRI].x : p.kp[TR_KP_RWRI].x;

		assert((up_x < UW / 2) == (TR_CAM_MIRROR != 0));
	}

	printf("PASS: tests/host/test_arms.c (TR_CAM_MIRROR=%d)\n", TR_CAM_MIRROR);
	return 0;
}

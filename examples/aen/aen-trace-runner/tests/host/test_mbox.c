/* tests/host/test_mbox.c */
#include <assert.h>
#include <string.h>
#include "../../src/game/state.h"
#include "../../src/game/zone.h"
#include "../../src/ipc/tr_mbox.h"
#include "../../src/render/panel_rot.h"

/* Test barrier: on host there's no real memory-ordering hazard to model, but
 * the torn-read case needs a way to simulate "the writer started a new
 * publish between the two reads" without a second thread. tr_mbox_take_in()
 * calls barrier() exactly twice -- once between reading seq and copying the
 * payload, once between the copy and the re-read of seq. This callback
 * bumps in_seq itself on a chosen call, standing in for a writer that raced
 * in right then. */
static volatile tr_mbox_t g_mbox;
static int                g_tear_on_call; /* 0 = never; N = bump seq during Nth barrier() call */
static int                g_call_count;

static void noop_barrier(void)
{
}

static void tearing_barrier(void)
{
	g_call_count++;
	if (g_call_count == g_tear_on_call)
		g_mbox.in_seq = g_mbox.in_seq + 1u; /* simulate a concurrent publish */
}

static tr_game_t make_game(void)
{
	tr_game_t g;
	tr_game_init(&g, 99u);
	g.lane       = 2;
	g.airborne   = true;
	g.air_ticks  = 7;
	g.ducking    = false;
	g.duck_ticks = 0;
	g.score      = 1234;
	g.alive      = true;
	g.tick       = 555;
	g.rng        = 0xDEADBEEFu;

	/* All three entity kinds, including a still-FREE slot, at the array's
	 * edges too -- proves the copy touches every one of TR_MAX_ENTITIES
	 * (16) slots, not just a prefix. */
	memset(g.ents, 0, sizeof(g.ents));
	g.ents[0]  = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 0, .y = 10, .low = true };
	g.ents[1]  = (tr_entity_t){ .kind = TR_ENT_PICKUP, .lane = 1, .y = -3, .low = false };
	g.ents[2]  = (tr_entity_t){ .kind = TR_ENT_FREE, .lane = 0, .y = 0, .low = false };
	g.ents[15] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 2, .y = 32000, .low = false };

	return g;
}

int main(void)
{
	/* --- tr_frame_in_from_game(): field-by-field round trip --- */
	tr_game_t     g = make_game();
	tr_frame_in_t f;
	tr_frame_in_from_game(&f, &g, TR_BANNER_STEP_BACK, true, false);

	assert(f.tick == g.tick);
	assert(f.score == g.score);
	assert(f.rng == g.rng);
	assert(f.flags == (TR_FLAG_ALIVE | TR_FLAG_AIRBORNE | TR_FLAG_ATTRACT_ACTIVE));
	assert(!(f.flags & TR_FLAG_DUCKING));
	assert(!(f.flags & TR_FLAG_PAUSED));
	assert(f.lane == g.lane);
	assert(f.air_ticks == g.air_ticks);
	assert(f.duck_ticks == g.duck_ticks);
	assert(f.banner == TR_BANNER_STEP_BACK);
	assert(f.track_h == 0); /* not game state; left to the caller */

	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		assert(f.ents[i].kind == (uint8_t)g.ents[i].kind);
		assert(f.ents[i].lane == g.ents[i].lane);
		assert(f.ents[i].low == (g.ents[i].low ? 1 : 0));
		assert(f.ents[i].y == g.ents[i].y);
	}
	/* Pins the free-slot and edge-index cases explicitly, not just the loop
	 * above -- a broken loop bound (e.g. off by one) could still pass the
	 * loop while missing these. */
	assert(f.ents[2].kind == TR_ENT_FREE);
	assert(f.ents[15].kind == TR_ENT_OBSTACLE && f.ents[15].y == 32000);

	/* A second banner/attract/paused combination -- proves those three
	 * caller-supplied args (not read from tr_game_t) actually land. */
	tr_frame_in_t f2;
	tr_frame_in_from_game(&f2, &g, TR_BANNER_GAME_OVER, false, true);
	assert(f2.banner == TR_BANNER_GAME_OVER);
	assert(f2.flags & TR_FLAG_PAUSED);
	assert(!(f2.flags & TR_FLAG_ATTRACT_ACTIVE));

	/* --- publish/take `in`: happy path --- */
	memset((void *)&g_mbox, 0, sizeof(g_mbox));
	f.track_h = 1280;
	assert(f.rotation == 0 && TR_MBOX_VERSION == 2u); /* a fresh snapshot: not turned */
	f.rotation = 270; /* does not fit a byte: the field is 16 bits */
	tr_mbox_publish_in(&g_mbox, &f, TR_FB_A, noop_barrier);
	{
		tr_frame_in_t r270;
		uint32_t      rfb, rseq;

		assert(tr_mbox_take_in(&g_mbox, 0u, &r270, &rfb, &rseq, noop_barrier));
		assert(r270.rotation == 270 && tr_rot_valid(r270.rotation));
	}
	f.rotation = 180; /* carried as is; the renderer refuses it (tr_rot_valid) */
	tr_mbox_publish_in(&g_mbox, &f, TR_FB_A, noop_barrier);
	{
		tr_frame_in_t r180;
		uint32_t      rfb, rseq;

		assert(tr_mbox_take_in(&g_mbox, 0u, &r180, &rfb, &rseq, noop_barrier));
		assert(r180.rotation == 180 && !tr_rot_valid(r180.rotation));
	}
	memset((void *)&g_mbox, 0, sizeof(g_mbox));
	f.rotation = 90;
	tr_mbox_publish_in(&g_mbox, &f, TR_FB_A, noop_barrier);
	assert(g_mbox.in_seq == 1u);
	assert(g_mbox.in_fb == TR_FB_A);

	tr_frame_in_t got;
	uint32_t      fb, seq;
	bool          ok = tr_mbox_take_in(&g_mbox, 0u, &got, &fb, &seq, noop_barrier);
	assert(ok);
	assert(seq == 1u && fb == TR_FB_A);
	assert(got.tick == f.tick && got.score == f.score && got.track_h == 1280);
	assert(got.rotation == 90); /* the rotation byte crosses the mailbox */
	assert(memcmp(got.ents, f.ents, sizeof(got.ents)) == 0);

	/* --- no-new-frame: same last_seq as what's already published --- */
	ok = tr_mbox_take_in(&g_mbox, 1u, &got, &fb, &seq, noop_barrier);
	assert(!ok);

	/* --- torn read: seq changes between the two barrier() calls --- */
	g_call_count   = 0;
	g_tear_on_call = 2; /* bump seq right after the payload copy, before the re-read */
	ok             = tr_mbox_take_in(&g_mbox, 0u, &got, &fb, &seq, tearing_barrier);
	assert(!ok);
	assert(g_mbox.in_seq == 2u); /* the simulated racer's bump really landed */

	/* A tear on the FIRST barrier (before the payload copy) must also be
	 * caught -- the re-read still disagrees with the seq read at entry.
	 * last_seq=0 (not the mbox's current in_seq) so the call doesn't
	 * short-circuit on the "no new frame" check before barrier() ever runs. */
	g_call_count   = 0;
	g_tear_on_call = 1;
	ok             = tr_mbox_take_in(&g_mbox, 0u, &got, &fb, &seq, tearing_barrier);
	assert(!ok);

	/* --- publish/take `out`: happy path + no-new-frame --- */
	memset((void *)&g_mbox, 0, sizeof(g_mbox));
	tr_frame_out_t out = { .fb        = TR_FB_B,
		                   .ticks0    = 111,
		                   .ticks1    = 222,
		                   .tris      = 4000,
		                   .dropped   = 3,
		                   .frames    = 40,
		                   .heartbeat = 9 };
	tr_mbox_publish_out(&g_mbox, &out, 7u, noop_barrier);
	assert(g_mbox.out_seq == 7u && g_mbox.out_fb == TR_FB_B);

	tr_frame_out_t got_out;
	uint32_t       oseq;
	ok = tr_mbox_take_out(&g_mbox, 0u, &got_out, &oseq, noop_barrier);
	assert(ok && oseq == 7u);
	assert(got_out.tris == 4000 && got_out.dropped == 3 && got_out.frames == 40 &&
	       got_out.heartbeat == 9);

	ok = tr_mbox_take_out(&g_mbox, 7u, &got_out, &oseq, noop_barrier);
	assert(!ok);

	/* --- P6/P7 packet fields: crash_* from a crashed run, zeros otherwise --- */
	{
		tr_game_t     cg;
		tr_frame_in_t f;

		tr_game_init(&cg, 3u);
		cg.ents[7] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 2, .y = 1110, .low = true };
		tr_frame_in_from_game(&f, &cg, TR_BANNER_NONE, false, false);
		assert(!(f.flags & TR_FLAG_CRASH) && f.crash_tick == 0 && f.crash_ent == 0 &&
		       f.crash_kind == 0 && f.phase == 0 && f.crash_frac == 0);
		assert(f.hz == TR_PANEL_HZ); /* P3d: the A32 eases over this refresh's real time */

		cg.alive       = false;
		cg.crashed     = true;
		cg.hit         = 7;
		cg.crash_ticks = 33;
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, true, false);
		assert((f.flags & TR_FLAG_CRASH) && !(f.flags & TR_FLAG_ALIVE) &&
		       (f.flags & TR_FLAG_ATTRACT_ACTIVE));
		assert(f.crash_tick == tr_hz_to40(33u) && f.crash_ent == 7 && f.crash_lane == 2 &&
		       f.crash_kind == TR_CRASH_KIND_LOW);
		assert(f.hz == TR_PANEL_HZ);
		/* P3d: crash time in 40 Hz frames, whole + fraction, is the real
		 * time exactly -- at 30 Hz 4/3 a frame, evenly (crash_tick alone
		 * steps 1, 1, 2) */
		for (uint32_t k = 0; k < 60u; k++) {
			double want = (double)k * 40.0 / TR_PANEL_HZ, got;

			cg.crash_ticks = k;
			tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
			got = (double)f.crash_tick + (double)f.crash_frac / 65536.0;
			assert(got <= want + 1e-9 && got > want - 1.0 / 65536.0);
		}
		cg.crash_ticks = 33;
		cg.ents[7].low = false;
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
		assert(f.crash_kind == TR_CRASH_KIND_HIGH);

		/* P4b: a live wire of either height is TR_CRASH_KIND_WIRE (the
		 * renderer reads its height from ents[crash_ent].low), and travels
		 * as wire kind 3. */
		cg.ents[7].kind = TR_ENT_WIRE;
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
		assert(f.crash_kind == TR_CRASH_KIND_WIRE && f.ents[7].kind == 3u && f.ents[7].low == 0u);
		assert(tr_game_crash_kind(&cg) == TR_CRASH_KIND_WIRE);
		cg.ents[7].low = true;
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
		assert(f.crash_kind == TR_CRASH_KIND_WIRE && f.ents[7].low == 1u);
		cg.ents[7].kind = TR_ENT_OBSTACLE;
		assert(tr_game_crash_kind(&cg) == TR_CRASH_KIND_LOW);

		/* main.c's g_idle (a zeroed, never-run game) is dead but did not
		 * crash: title/calibration frames must not animate a crash. */
		tr_game_t idle;

		memset(&idle, 0, sizeof(idle));
		tr_frame_in_from_game(&f, &idle, TR_BANNER_STAND, false, false);
		assert(!(f.flags & (TR_FLAG_CRASH | TR_FLAG_ALIVE)));

		/* The new fields travel through the mailbox, inside the old
		 * `in` block (offsets pinned by tr_mbox.h's static asserts). */
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
		f.phase = 0x8123;
		memset((void *)&g_mbox, 0, sizeof(g_mbox));
		tr_mbox_publish_in(&g_mbox, &f, TR_FB_A, noop_barrier);

		tr_frame_in_t back;
		uint32_t      bfb, bseq;

		assert(tr_mbox_take_in(&g_mbox, 0u, &back, &bfb, &bseq, noop_barrier));
		assert(memcmp(&back, &f, sizeof(f)) == 0 && back.phase == 0x8123 &&
		       back.crash_tick == tr_hz_to40(33u));
	}

	/* P15 zones: tr_frame_in_from_game() leaves the zone off (an old HE's
	 * packet: no TR_FLAG_ZONE, zone 0, no gate is what a renderer must
	 * assume); tr_frame_in_set_zone() sets flag + fields, and they cross the
	 * mailbox inside the old `in` block. */
	{
		tr_zone_t z = { 0 };

		tr_frame_in_from_game(&f, &g, TR_BANNER_NONE, false, false);
		assert(!(f.flags & TR_FLAG_ZONE) && f.zone == 0u && f.gate_y == 0);
		tr_zone_reset(&z);
		z.zone   = TR_ZONE_RF;
		z.gate_y = 777;
		tr_frame_in_set_zone(&f, &z);
		assert((f.flags & TR_FLAG_ZONE) && f.zone == TR_ZONE_RF && f.gate_y == 777);
		z.gate_y = TR_ZONE_NO_GATE;
		tr_frame_in_set_zone(&f, &z);
		assert(f.gate_y == TR_ZONE_NO_GATE);
		assert(sizeof(tr_frame_in_t) + sizeof(g_mbox.pad2) == 0x140 - 0x080);
		/* ... beside P16's fields (the integrated packet: own bytes, own
		 * flag bits), in either order. */
		tr_react_t r;

		tr_react_init(&r);
		tr_frame_in_p16(&f, TR_CHAR_PIXEL, &r, true, 5000u);
		assert((TR_FLAG_CHAR | TR_FLAG_IDLE | TR_FLAG_ZONE) == 0x700u);
		assert((f.flags & 0x700u) == 0x700u && f.character == TR_CHAR_PIXEL && f.idle_ms == 5u);
		assert(f.zone == TR_ZONE_RF && f.gate_y == TR_ZONE_NO_GATE);

		tr_frame_in_t back;
		uint32_t      bfb, bseq;

		memset((void *)&g_mbox, 0, sizeof(g_mbox));
		tr_mbox_publish_in(&g_mbox, &f, TR_FB_B, noop_barrier);
		assert(tr_mbox_take_in(&g_mbox, 0u, &back, &bfb, &bseq, noop_barrier));
		assert(back.zone == TR_ZONE_RF && back.gate_y == TR_ZONE_NO_GATE &&
		       (back.flags & TR_FLAG_ZONE));
		assert(back.character == TR_CHAR_PIXEL && back.idle_ms == 5u &&
		       (back.flags & TR_FLAG_CHAR));
	}

	/* Booth crash shake: TR_FLAG_SHAKE (bit 12; 8..11 are taken) + the
	 * `shake` byte in what was zone_pad -- the block is 176 B since the rotation field. A crashed run's
	 * packet carries it from the fatal frame: full at the hit, decaying
	 * in real time to 0 within TR_SHAKE_FRAMES40 40 Hz frames, at either
	 * panel rate. A live run's packet: no flag, 0 -- what an old HE sent. */
	{
		tr_game_t cg;

		assert(TR_FLAG_SHAKE == (1u << 12) && (TR_FLAG_SHAKE & 0xFFFu) == 0u);
		assert(sizeof(tr_frame_in_t) == 176 && offsetof(tr_frame_in_t, shake) == 169);
		tr_game_init(&cg, 3u);
		tr_frame_in_from_game(&f, &cg, TR_BANNER_NONE, false, false);
		assert(!(f.flags & TR_FLAG_SHAKE) && f.shake == 0u);
		cg.alive      = false;
		cg.crashed    = true;
		cg.hit        = 1;
		uint32_t prev = 256u, shaking = 0;

		for (uint32_t k = 0; k < TR_CRASH_FRAMES; k++) {
			cg.crash_ticks = (uint8_t)k;
			tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
			assert(f.flags & TR_FLAG_SHAKE);
			assert(f.shake <= prev && f.shake == tr_crash_shake(tr_hz_to40(k)));
			assert(k != 0u || f.shake == 255u);
			if (tr_hz_to40(k) >= TR_SHAKE_FRAMES40) {
				assert(f.shake == 0u);
			}
			shaking += f.shake != 0u;
			prev = f.shake;
		}
		/* a few frames, then still: ~0.4 s of real time either way */
		assert(shaking >= 5u && shaking <= TR_HZ_FRAMES(TR_SHAKE_FRAMES40) + 1u);
		assert(TR_SHAKE_FRAMES40 * 25u <= 500u); /* no longer than half a second */
		tr_frame_in_t back;
		uint32_t      bfb, bseq;

		cg.crash_ticks = 1u;
		tr_frame_in_from_game(&f, &cg, TR_BANNER_GAME_OVER, false, false);
		memset((void *)&g_mbox, 0, sizeof(g_mbox));
		tr_mbox_publish_in(&g_mbox, &f, TR_FB_A, noop_barrier);
		assert(tr_mbox_take_in(&g_mbox, 0u, &back, &bfb, &bseq, noop_barrier));
		assert(back.shake == f.shake && back.shake != 0u && (back.flags & TR_FLAG_SHAKE));
	}

	return 0;
}

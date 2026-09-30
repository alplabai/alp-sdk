/* tests/host/test_aring.c -- src/ipc/tr_aring.c (HE -> HP sound-event ring)
 * and src/game/sfx.c (which game moments become events). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/sfx.h"
#include "../../src/ipc/tr_aring.h"
#include "../../src/ipc/tr_mbox.h"

static volatile tr_aring_t s_ring;
static int                 s_barriers;

static void barrier(void)
{
	s_barriers++;
}

/* Snapshot of the ring at each barrier call, for the ordering checks. */
static struct {
	uint32_t head, tail;
	uint8_t  slot_kind, slot_param;
} s_snap[4];
static unsigned s_snap_n;

static void trace_barrier(void)
{
	if (s_snap_n < 4u) {
		volatile tr_aev_t *slot =
		    &s_ring.ev[(s_ring.head - (s_snap_n ? 1u : 0u)) & (TR_ARING_CAP - 1u)];
		s_snap[s_snap_n].head       = s_ring.head;
		s_snap[s_snap_n].tail       = s_ring.tail;
		s_snap[s_snap_n].slot_kind  = slot->kind;
		s_snap[s_snap_n].slot_param = slot->param;
	}
	s_snap_n++;
}

static unsigned drain(uint8_t *kinds, uint8_t *params, unsigned max)
{
	tr_aev_t e;
	unsigned n = 0;
	while (n < max && tr_aring_pop(&s_ring, &e, barrier)) {
		kinds[n]  = e.kind;
		params[n] = e.param;
		n++;
	}
	return n;
}

static void test_ring(void)
{
	tr_aev_t e;
	uint8_t  k[128], p[128];

	/* garbage in SRAM, no magic: the HP sees nothing */
	memset((void *)&s_ring, 0xA5, sizeof(s_ring));
	assert(!tr_aring_pop(&s_ring, &e, barrier));

	tr_aring_init(&s_ring, barrier);
	assert(s_ring.magic == TR_ARING_MAGIC && s_ring.version == TR_ARING_VERSION);
	assert(s_ring.head == 0u && s_ring.tail == 0u && s_ring.dropped == 0u && s_ring.hp_state == 0u);
	assert(!tr_aring_pop(&s_ring, &e, barrier));

	/* FIFO order, params preserved */
	for (unsigned i = 0; i < 10u; i++)
		assert(tr_aring_push(&s_ring, (uint8_t)(1u + i % 9u), (uint8_t)i, barrier));
	assert(drain(k, p, 128) == 10u);
	for (unsigned i = 0; i < 10u; i++)
		assert(k[i] == 1u + i % 9u && p[i] == i);

	/* full: CAP accepted, the rest refused and counted, nothing overwritten */
	for (unsigned i = 0; i < TR_ARING_CAP + 5u; i++)
		(void)tr_aring_push(&s_ring, TR_AEV_JUMP, (uint8_t)i, barrier);
	assert(s_ring.dropped == 5u);
	assert(drain(k, p, 128) == TR_ARING_CAP);
	for (unsigned i = 0; i < TR_ARING_CAP; i++)
		assert(p[i] == i);

	/* 32-bit index wrap */
	s_ring.head = s_ring.tail = 0xFFFFFFF0u;
	for (unsigned i = 0; i < 40u; i++)
		assert(tr_aring_push(&s_ring, TR_AEV_DUCK, (uint8_t)i, barrier));
	assert(s_ring.head == 0x00000018u);
	assert(drain(k, p, 128) == 40u);
	for (unsigned i = 0; i < 40u; i++)
		assert(p[i] == i);

	/* head more than CAP ahead (corrupt / producer restarted): resync, no stale replay */
	s_ring.head = s_ring.tail + TR_ARING_CAP + 3u;
	assert(!tr_aring_pop(&s_ring, &e, barrier));
	assert(s_ring.tail == s_ring.head);
	assert(!tr_aring_pop(&s_ring, &e, barrier));

	/* HE reboot with the HP alive: indices kept, unread events dropped */
	(void)tr_aring_push(&s_ring, TR_AEV_PICKUP, 1, barrier);
	s_ring.hp_heartbeat = 1234u;
	s_ring.hp_state     = TR_ARING_HP_RUNNING;
	uint32_t t          = s_ring.tail;
	tr_aring_init(&s_ring, barrier);
	assert(s_ring.tail == t && s_ring.head == t && s_ring.hp_heartbeat == 1234u);
	assert(s_ring.hp_state == TR_ARING_HP_OFF); /* a live HP re-asserts it; none -> HUD "--" */
	assert(!tr_aring_pop(&s_ring, &e, barrier));

	/* ORDER, not just counts: a hook barrier snapshots the ring at every
	 * call. Push: at its 1st barrier the slot already holds the event and
	 * head is not bumped yet; at its 2nd, head is. Pop: a barrier between
	 * the head read and the slot read, another before the tail bump. */
	uint32_t h0 = s_ring.head, t0 = s_ring.tail;
	s_barriers = 0;
	s_snap_n   = 0;
	(void)tr_aring_push(&s_ring, TR_AEV_WIRE, 77, trace_barrier);
	assert(s_snap_n == 2);
	assert(s_snap[0].slot_kind == TR_AEV_WIRE && s_snap[0].slot_param == 77 &&
	       s_snap[0].head == h0);
	assert(s_snap[1].head == h0 + 1u);
	s_snap_n = 0;
	assert(tr_aring_pop(&s_ring, &e, trace_barrier) && e.kind == TR_AEV_WIRE && e.param == 77);
	assert(s_snap_n == 2 && s_snap[0].tail == t0 && s_snap[1].tail == t0 && s_ring.tail == t0 + 1u);

	/* an HE built against another layout: the HP reads nothing */
	(void)tr_aring_push(&s_ring, TR_AEV_JUMP, 0, barrier);
	s_ring.version = TR_ARING_VERSION + 1u;
	assert(!tr_aring_pop(&s_ring, &e, barrier));
	s_ring.version = TR_ARING_VERSION;
	(void)drain(k, p, 128);
}

#define TRACK_H 1280

static uint8_t s_combo; /* the score's combo the next watch() reports */

static unsigned watch(tr_sfx_watch_t *w, const tr_game_t *g, bool attract, tr_aev_t *ev)
{
	unsigned n = tr_sfx_watch(w, g, s_combo, attract, TRACK_H, ev);
	assert(n <= TR_SFX_MAX_EVENTS);
	return n;
}

static bool has(const tr_aev_t *ev, unsigned n, uint8_t kind, int param)
{
	for (unsigned i = 0; i < n; i++) {
		if (ev[i].kind == kind && (param < 0 || ev[i].param == param)) return true;
	}
	return false;
}

static void test_watch(void)
{
	tr_sfx_watch_t w;
	tr_game_t      g;
	tr_aev_t       ev[TR_SFX_MAX_EVENTS];
	unsigned       n;

	tr_sfx_watch_init(&w);
	tr_game_init(&g, 1u);

	/* first frame in attract: music at attract level + the jingle, nothing else */
	n = watch(&w, &g, true, ev);
	assert(n == 2u && has(ev, n, TR_AEV_MUSIC, TR_MUSIC_ATTRACT) && has(ev, n, TR_AEV_ATTRACT, 0));
	assert(watch(&w, &g, true, ev) == 0u); /* same state: silence */

	/* a player takes over: play-level music, no jingle */
	n = watch(&w, &g, false, ev);
	assert(n == 1u && has(ev, n, TR_AEV_MUSIC, TR_MUSIC_PLAY));

	/* jump on the rising edge only; no footsteps in the air */
	g.tick     = 3;
	g.airborne = true;
	n          = watch(&w, &g, false, ev);
	assert(n == 1u && has(ev, n, TR_AEV_JUMP, -1));
	g.tick = 8;
	n      = watch(&w, &g, false, ev);
	assert(n == 0u);

	/* landed: footsteps every TR_SFX_STEP_TICKS, alternating feet */
	g.airborne     = false;
	unsigned steps = 0, feet = 0;
	for (uint32_t t = 9; t < 9u + 4u * TR_SFX_STEP_TICKS;
	     t++) { /* any 4 x TR_SFX_STEP_TICKS ticks: 4 steps */
		g.tick = t;
		n      = watch(&w, &g, false, ev);
		if (has(ev, n, TR_AEV_FOOTSTEP, -1)) {
			steps++;
			feet += ev[0].param;
		}
	}
	assert(steps == 4u && feet == 2u);

	/* duck: rising edge */
	g.tick++;
	g.ducking = true;
	n         = watch(&w, &g, false, ev);
	assert(has(ev, n, TR_AEV_DUCK, -1));
	g.ducking = false;

	/* pickups: +10 is a pickup, +1 is not; the pitch follows the score's
	 * combo the HUD shows (x1..x5 -> 0..4), capped and reset with it */
	g.tick = 101;
	g.score += 1u;
	n = watch(&w, &g, false, ev);
	assert(!has(ev, n, TR_AEV_PICKUP, -1));
	uint8_t combo[7] = { 1, 2, 3, 4, 5, 5, 1 }, want[7] = { 0, 1, 2, 3, 4, 4, 0 };
	for (unsigned i = 0; i < 7u; i++) {
		g.tick = 102u + 40u * i;
		g.score += 10u;
		s_combo = combo[i];
		n       = watch(&w, &g, false, ev);
		assert(has(ev, n, TR_AEV_PICKUP, want[i]));
	}
	s_combo = 0u;

	/* crash: low obstacle -> kind LOW, once */
	g.tick++;
	g.alive   = false;
	g.crashed = true;
	g.hit     = 3;
	g.ents[3] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 1, .y = 1100, .low = true };
	n         = watch(&w, &g, false, ev);
	assert(has(ev, n, TR_AEV_CRASH, TR_CRASH_KIND_LOW));
	g.crash_ticks = 5;
	assert(watch(&w, &g, false, ev) == 0u); /* crash frames: no repeat */

	/* new run (tick back to small): per-run state reset silently, then a
	 * high-obstacle crash reports HIGH */
	tr_game_init(&g, 2u);
	g.tick = 1;
	assert(watch(&w, &g, false, ev) == 0u);
	g.tick    = 2;
	g.alive   = false;
	g.crashed = true;
	g.hit     = 0;
	g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 0, .y = 1100, .low = false };
	n         = watch(&w, &g, false, ev);
	assert(has(ev, n, TR_AEV_CRASH, TR_CRASH_KIND_HIGH));

	/* live wire ahead: hum every TR_SFX_WIRE_TICKS at tr_game_wire_level();
	 * hitting it crashes with TR_CRASH_KIND_WIRE */
	tr_sfx_watch_init(&w);
	tr_game_init(&g, 4u);
	(void)watch(&w, &g, false, ev);
	g.ents[5]     = (tr_entity_t){ .kind = TR_ENT_WIRE, .lane = g.lane, .y = 600, .low = true };
	unsigned hums = 0;
	for (uint32_t t = 1; t <= 4u * TR_SFX_WIRE_TICKS; t++) {
		g.tick = t;
		n      = watch(&w, &g, false, ev);
		for (unsigned i = 0; i < n; i++) {
			if (ev[i].kind == TR_AEV_WIRE) {
				hums++;
				assert(t == TR_SFX_WIRE_TICKS * hums); /* exactly every TR_SFX_WIRE_TICKS */
				assert(ev[i].param == tr_game_wire_level(&g, TRACK_H) && ev[i].param > 0u);
			}
		}
	}
	assert(hums == 4u && TR_SFX_WIRE_TICKS == 6u); /* 12 frames at the 0.5x pace */
	g.tick++;
	g.alive = false, g.crashed = true, g.hit = 5;
	n = watch(&w, &g, false, ev);
	assert(has(ev, n, TR_AEV_CRASH, TR_CRASH_KIND_WIRE));
	tr_game_init(&g, 5u); /* no wire: no hum */
	tr_sfx_watch_init(&w);
	(void)watch(&w, &g, false, ev);
	for (uint32_t t = 1; t <= 2u * TR_SFX_WIRE_TICKS; t++) {
		g.tick = t;
		n      = watch(&w, &g, false, ev);
		assert(!has(ev, n, TR_AEV_WIRE, -1));
	}

	/* worst case fits: attract flips on the same frame as jump + pickup + step + crash */
	tr_sfx_watch_init(&w);
	tr_game_init(&g, 3u);
	(void)watch(&w, &g, false, ev);
	g.tick     = TR_SFX_STEP_TICKS;
	g.airborne = false;
	g.ducking  = true;
	g.score    = 10u;
	g.alive    = true;
	n          = watch(&w, &g, true, ev);
	assert(has(ev, n, TR_AEV_ATTRACT, 0) && has(ev, n, TR_AEV_DUCK, -1) &&
	       has(ev, n, TR_AEV_PICKUP, 0) && has(ev, n, TR_AEV_FOOTSTEP, -1));
}

int main(void)
{
	test_ring();
	test_watch();
	puts("test_aring: ok");
	return 0;
}

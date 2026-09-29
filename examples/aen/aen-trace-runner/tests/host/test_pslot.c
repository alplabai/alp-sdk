/* tests/host/test_pslot.c -- src/ipc/tr_pslot.c: the HP -> HE pose-slot
 * seqlock (odd-before/even-after -- tr_pslot.h's header note explains why
 * the simpler single-bump protocol an earlier version of this file used was
 * reviewed and found wrong). Same torn-read simulation technique as
 * test_mbox.c: barrier() is called by tr_pslot_read() exactly twice (once
 * between the seq read and the body copy, once between the copy and the
 * re-read), so a test double that bumps seq on a chosen call stands in for
 * a second-core writer racing in mid-read. tr_pslot_write() itself now
 * calls barrier() three times (odd bump, body, even bump); this file's
 * tearing_barrier is only ever passed to tr_pslot_read(), never to a write,
 * so that third call never matters here. */
#include <assert.h>
#include <string.h>

#include "../../src/ipc/tr_pslot.h"

static volatile tr_pslot_t g_slot;
static int                 g_tear_on_call; /* 0 = never; N = bump seq during the Nth barrier() call */
static int                 g_call_count;

static void noop_barrier(void)
{
}

static void tearing_barrier(void)
{
	g_call_count++;
	if (g_call_count == g_tear_on_call) {
		g_slot.seq = g_slot.seq + 1u; /* simulate a concurrent tr_pslot_write() */
	}
}

static tr_pose_t make_pose(int16_t x0)
{
	tr_pose_t p = { 0 };

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ .x = (int16_t)(x0 + k), .y = (int16_t)(100 + k), .score = (uint8_t)(k * 10) };
	}
	return p;
}

int main(void)
{
	/* --- happy path: write then read. seq ends at 2 (0 -> 1 odd -> 2 even),
	 * not 1: the write's own two bumps, not test_mbox.c's single-bump one. --- */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	tr_pose_t p1 = make_pose(10);
	uint8_t   thumb[TR_PSLOT_THUMB_BYTES];

	memset(thumb, 0x5A, sizeof(thumb));
	tr_pslot_write(&g_slot, &p1, 9220u, 1500u, TR_HP_STATE_RUNNING, thumb, 42u, noop_barrier);
	assert(g_slot.magic == TR_PSLOT_MAGIC && g_slot.version == TR_PSLOT_VERSION);
	assert(g_slot.seq == 2u && !(g_slot.seq & 1u));
	assert(g_slot.frame_no == 42u);

	tr_pslot_t out;
	uint32_t   seq;
	bool       ok = tr_pslot_read(&g_slot, 0u, &out, &seq, noop_barrier);

	assert(ok && seq == 2u);
	assert(out.infer_us == 9220u && out.pre_us == 1500u && out.hp_state == TR_HP_STATE_RUNNING);
	assert(memcmp(out.pose.kp, p1.kp, sizeof(p1.kp)) == 0);
	assert(memcmp((void *)out.thumb, thumb, sizeof(thumb)) == 0);

	/* --- no-new-frame: last_seq already matches --- */
	ok = tr_pslot_read(&g_slot, 2u, &out, &seq, noop_barrier);
	assert(!ok);

	/* --- a second write with thumb == NULL leaves the thumbnail unchanged
	 * (e.g. hp_state NO_CAMERA has nothing fresh to downsample) --- */
	tr_pose_t p2 = make_pose(200);

	tr_pslot_write(&g_slot, &p2, 0u, 0u, TR_HP_STATE_NO_CAMERA, NULL, 43u, noop_barrier);
	assert(g_slot.seq == 4u);
	ok = tr_pslot_read(&g_slot, 0u, &out, &seq, noop_barrier);
	assert(ok && seq == 4u && out.hp_state == TR_HP_STATE_NO_CAMERA);
	assert(memcmp((void *)out.thumb, thumb, sizeof(thumb)) == 0); /* still the first write's bytes */
	assert(memcmp(out.pose.kp, p2.kp, sizeof(p2.kp)) == 0);       /* pose itself DID update */

	/* --- torn read: the writer races in between the seq read and the body
	 * copy (tear_on_call 1) --- */
	g_call_count   = 0;
	g_tear_on_call = 1;
	/* last_seq=0, not the slot's current seq, so the call does not
	 * short-circuit on "nothing new" before barrier() ever runs. */
	ok = tr_pslot_read(&g_slot, 0u, &out, &seq, tearing_barrier);
	assert(!ok);
	assert(g_slot.seq == 5u); /* the simulated racer's bump really landed (now odd) */

	/* --- torn read: the writer races in between the copy and the re-check
	 * (tear_on_call 2) --- */
	g_call_count   = 0;
	g_tear_on_call = 2;
	/* Seed an even seq first (5 is odd from the previous case -- a real
	 * reader would already reject that at entry, see the next test; this
	 * one is specifically about a race landing AFTER the odd-entry check). */
	g_slot.seq = 6u;
	ok         = tr_pslot_read(&g_slot, 0u, &out, &seq, tearing_barrier);
	assert(!ok);
	assert(g_slot.seq == 7u);

	/* --- THE FIX'S REGRESSION TEST: a write left mid-publish (odd seq,
	 * torn body) with NO further activity during the read at all -- this is
	 * exactly what the old single-bump protocol missed: its only defence
	 * was "did seq change across the copy", and here it never does (no
	 * racer fires mid-read; the tear already happened before tr_pslot_read
	 * is even called, modelling a writer that stalled or the HP core
	 * reset/faulted between its odd bump and its even bump). Confirmed by
	 * hand against the pre-fix tr_pslot.c: this exact setup returned
	 * ok=true with a torn kp[0]/kp[1] mix. The fix's entry check
	 * (`s0 & 1u`) rejects it before ever touching the body. --- */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	g_slot.magic   = TR_PSLOT_MAGIC;
	g_slot.version = TR_PSLOT_VERSION;
	g_slot.pose    = make_pose(10);   /* the "old" publish's keypoints ... */
	g_slot.pose.kp[0].x = 999;        /* ... except kp[0], already overwritten by the stalled "new" one */
	g_slot.seq = 1u;                  /* ODD: mid-publish, never finished */
	ok         = tr_pslot_read(&g_slot, 0u, &out, &seq, noop_barrier);
	assert(!ok);

	return 0;
}

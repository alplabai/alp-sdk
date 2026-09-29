/* tests/host/test_cam_view.c -- src/ipc/tr_cam_view.c: the HP -> A32 live
 * camera view descriptor's seqlock (same odd-before/even-after protocol as
 * tr_pslot.c, same torn-read simulation technique as test_pslot.c), plus
 * tr_cam_view_still_seq() -- the second check a reader needs after copying
 * pixel rows out of the published buffer, since the buffer itself can be
 * recycled after tr_cam_view_read() returns true but before a slower pixel
 * copy finishes (tr_cam_view.h's own header note on the accepted tear risk).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_cam_view.h"

static volatile tr_cam_view_t g_slot;
static int                    g_tear_on_call;
static int                    g_call_count;

static void noop_barrier(void)
{
}

static void tearing_barrier(void)
{
	g_call_count++;
	if (g_call_count == g_tear_on_call) {
		g_slot.seq = g_slot.seq + 1u; /* simulate a concurrent tr_cam_view_write() */
	}
}

int main(void)
{
	/* 1. Happy path: write then read, seq ends even (0 -> 1 -> 2). */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	tr_cam_view_write(&g_slot, 0x02480000u, 7u, 640u, 400u, 90u, 1u, noop_barrier);
	assert(g_slot.magic == TR_CAM_VIEW_MAGIC && g_slot.version == TR_CAM_VIEW_VERSION);
	assert(g_slot.seq == 2u && !(g_slot.seq & 1u));

	tr_cam_view_t out;

	assert(tr_cam_view_read(&g_slot, &out, noop_barrier));
	assert(out.buf_addr == 0x02480000u && out.frame_no == 7u && out.width == 640u &&
	       out.height == 400u && out.rotate == 90u && out.mirror == 1u);

	/* 2. Never published (slot still zeroed): magic/version reject it,
	 * same as tr_pslot_read()'s equivalent case. */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	assert(!tr_cam_view_read(&g_slot, &out, noop_barrier));

	/* 3. Mid-publish (odd seq): rejected on sight, same as tr_pslot_read(). */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	tr_cam_view_write(&g_slot, 0x02480000u, 1u, 640u, 400u, 90u, 1u, noop_barrier);
	g_slot.seq = g_slot.seq + 1u; /* even -> odd, simulating a write left mid-publish */
	assert(!tr_cam_view_read(&g_slot, &out, noop_barrier));

	/* 4. Torn read: a second write starts (or finishes) mid-copy. Same
	 * technique as test_pslot.c -- tearing_barrier bumps seq on a chosen
	 * call of the two barrier() calls tr_cam_view_read() makes. */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	tr_cam_view_write(&g_slot, 0x02480000u, 1u, 640u, 400u, 90u, 1u, noop_barrier);
	g_tear_on_call = 2; /* the barrier BETWEEN the body copy and the re-check */
	g_call_count   = 0;
	assert(!tr_cam_view_read(&g_slot, &out, tearing_barrier));

	/* 5. tr_cam_view_still_seq(): the second check for a pixel copy that
	 * ran AFTER a successful tr_cam_view_read() -- true while nothing
	 * else has published again, false the instant something has (the
	 * buffer this frame's `out` pointed at may now hold different bytes). */
	memset((void *)&g_slot, 0, sizeof(g_slot));
	tr_cam_view_write(&g_slot, 0x02480000u, 1u, 640u, 400u, 90u, 1u, noop_barrier);
	assert(tr_cam_view_read(&g_slot, &out, noop_barrier));
	uint32_t seq = g_slot.seq;

	assert(tr_cam_view_still_seq(&g_slot, seq)); /* nothing changed yet */
	tr_cam_view_write(&g_slot,
	                  0x024C4000u,
	                  2u,
	                  640u,
	                  400u,
	                  90u,
	                  1u,
	                  noop_barrier);              /* the HP recycled the buffer */
	assert(!tr_cam_view_still_seq(&g_slot, seq)); /* the pixel copy must be discarded */

	printf("PASS: tests/host/test_cam_view.c\n");
	return 0;
}

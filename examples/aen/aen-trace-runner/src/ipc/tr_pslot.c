/* src/ipc/tr_pslot.c -- see tr_pslot.h. Pure C, no Zephyr headers, no arch
 * intrinsics: the barrier is the caller's function pointer, same shape as
 * tr_mbox.c, so this builds and host-tests identically to it. */
#include "tr_pslot.h"

#include <string.h>

void tr_pslot_write(volatile tr_pslot_t *s, const tr_pose_t *pose, uint32_t infer_us, uint32_t pre_us,
                     uint32_t hp_state, const uint8_t *thumb, uint32_t frame_no, void (*barrier)(void))
{
	s->seq = s->seq + 1u; /* even -> odd: announce a write in progress BEFORE any body byte moves */
	barrier();             /* the odd seq must be visible before the body write starts */
	s->magic    = TR_PSLOT_MAGIC;
	s->version  = TR_PSLOT_VERSION;
	s->frame_no = frame_no;
	s->pose     = *pose;
	s->infer_us = infer_us;
	s->pre_us   = pre_us;
	s->hp_state = hp_state;
	if (thumb != NULL) {
		memcpy((void *)s->thumb, thumb, sizeof(s->thumb));
	}
	barrier(); /* body fully visible before the even bump publishes it */
	s->seq = s->seq + 1u; /* odd -> even: publish complete */
	barrier();             /* seq write retired before the caller signals/returns */
}

bool tr_pslot_read(const volatile tr_pslot_t *s, uint32_t last_seq, tr_pslot_t *out, uint32_t *seq,
                    void (*barrier)(void))
{
	uint32_t s0 = s->seq;

	if (s0 == last_seq) {
		return false; /* nothing new since last_seq */
	}
	if (s0 & 1u) {
		return false; /* the HP is mid-publish: a torn body would be read even if seq never
		                * changes again before the re-check below (a writer stalled or reset
		                * mid-publish leaves an odd seq sitting still) -- reject on sight,
		                * don't wait to see if it changes */
	}

	barrier(); /* order the seq read before the body read */
	*out = *s;
	barrier(); /* order the body read before the re-check below */

	if (s->seq != s0) {
		return false; /* torn: the HP started (or finished) a new publish mid-copy */
	}
	if (out->magic != TR_PSLOT_MAGIC || out->version != TR_PSLOT_VERSION) {
		return false; /* the HP has never published (slot still zeroed/stale) */
	}

	*seq = s0;
	return true;
}

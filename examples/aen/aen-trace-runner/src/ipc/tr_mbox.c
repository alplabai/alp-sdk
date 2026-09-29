/* src/ipc/tr_mbox.c -- mailbox publish/consume, field-by-field game-state
 * snapshotting. Pure C: no Zephyr headers, no arch intrinsics, no libc calls
 * beyond what <string.h> gives. The barrier is supplied by the caller (a
 * function pointer wrapping __DSB() on the M55, `dsb sy`/`dmb ish` on the
 * A32, a no-op or a test double on host) so this file builds identically on
 * all three.
 *
 * Who calls which half:
 *   M55  (writer of `in`, reader of `out`): tr_mbox_publish_in(), tr_mbox_take_out()
 *   A32  (reader of `in`, writer of `out`): tr_mbox_take_in(),    tr_mbox_publish_out()
 */
#include "tr_mbox.h"

#include <string.h>

void tr_frame_in_from_game(tr_frame_in_t   *out,
                           const tr_game_t *g,
                           uint8_t          banner,
                           bool             attract_active,
                           bool             paused)
{
	memset(out, 0, sizeof(*out));

	out->tick  = g->tick;
	out->score = g->score;
	out->rng   = g->rng;

	out->flags = (g->alive ? TR_FLAG_ALIVE : 0u) | (g->airborne ? TR_FLAG_AIRBORNE : 0u) |
	             (g->ducking ? TR_FLAG_DUCKING : 0u) |
	             (attract_active ? TR_FLAG_ATTRACT_ACTIVE : 0u) | (paused ? TR_FLAG_PAUSED : 0u) |
	             (g->crashed ? TR_FLAG_CRASH : 0u);

	out->lane       = g->lane;
	out->air_ticks  = g->air_ticks;
	out->duck_ticks = g->duck_ticks;
	out->banner     = banner;
	out->hz         = TR_PANEL_HZ; /* the frame's refresh: the A32 scales its easing by it */

	/* track_h is display geometry, not game state -- tr_game_t doesn't carry
	 * it (tr_game_step() takes it as a parameter instead). Left zeroed here;
	 * the caller sets out->track_h itself once it knows the panel height. */

	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		const tr_entity_t *e = &g->ents[i];
		out->ents[i].kind    = (uint8_t)e->kind;
		out->ents[i].lane    = e->lane;
		out->ents[i].low     = e->low ? 1u : 0u;
		out->ents[i].y       = e->y;
	}

	if (g->crashed) {
		const tr_entity_t *e = &g->ents[g->hit % TR_MAX_ENTITIES];

		out->crash_tick =
		    (uint8_t)tr_hz_to40(g->crash_ticks); /* the A32 animates in 40 Hz frames */
		/* and the part of a 40 Hz frame the floor dropped (30 Hz: 0, 1/3, 2/3) */
		out->crash_frac =
		    (uint16_t)(((uint32_t)g->crash_ticks * 40u % TR_PANEL_HZ) * 65536u / TR_PANEL_HZ);
		out->crash_ent  = g->hit;
		out->crash_lane = e->lane;
		out->crash_kind = tr_game_crash_kind(g);
		out->flags |= TR_FLAG_SHAKE; /* booth: the camera shake is ours now */
		out->shake = tr_crash_shake(out->crash_tick);
	}
}

void tr_frame_in_p16(tr_frame_in_t    *out,
                     uint8_t           character,
                     const tr_react_t *r,
                     bool              idle,
                     uint32_t          idle_us)
{
	uint32_t ims = idle_us / 1000u;

	out->flags |= TR_FLAG_CHAR | (idle ? TR_FLAG_IDLE : 0u);
	out->character  = character < TR_CHAR_N ? character : (uint8_t)TR_CHAR_PROBE;
	out->react      = r->kind;
	out->react_side = (uint8_t)r->side;
	out->react_seq  = r->seq;
	out->react_ms   = tr_react_ms(r);
	out->idle_ms    = idle ? (uint16_t)(ims > 65535u ? 65535u : ims) : 0u;
}

void tr_frame_in_set_zone(tr_frame_in_t *out, const tr_zone_t *z)
{
	out->zone   = (uint8_t)(z->zone % TR_ZONES);
	out->gate_y = z->gate_y;
	out->flags |= TR_FLAG_ZONE;
}

uint8_t tr_game_crash_kind(const tr_game_t *g)
{
	const tr_entity_t *e = &g->ents[g->hit % TR_MAX_ENTITIES];

	if (!g->crashed) {
		return 0u;
	}
	return e->kind == TR_ENT_WIRE ? TR_CRASH_KIND_WIRE
	       : e->low               ? TR_CRASH_KIND_LOW
	                              : TR_CRASH_KIND_HIGH;
}

void tr_mbox_publish_in(volatile tr_mbox_t  *m,
                        const tr_frame_in_t *in,
                        uint32_t             fb,
                        void (*barrier)(void))
{
	m->in    = *in;
	m->in_fb = fb;
	barrier(); /* payload + in_fb visible before the seq bump advertises them */
	m->in_seq = m->in_seq + 1u;
	barrier(); /* seq write retired before the caller signals/returns */
}

bool tr_mbox_take_in(const volatile tr_mbox_t *m,
                     uint32_t                  last_seq,
                     tr_frame_in_t            *out,
                     uint32_t                 *fb,
                     uint32_t                 *seq,
                     void (*barrier)(void))
{
	uint32_t s0 = m->in_seq;
	if (s0 == last_seq) return false; /* no new frame since last_seq */

	barrier(); /* order the seq read before the payload read */
	*out = m->in;
	*fb  = m->in_fb;
	barrier(); /* order the payload read before the re-check below */

	if (m->in_seq != s0) return false; /* torn: writer started a new publish mid-copy */

	*seq = s0;
	return true;
}

void tr_mbox_publish_out(volatile tr_mbox_t   *m,
                         const tr_frame_out_t *out,
                         uint32_t              seq,
                         void (*barrier)(void))
{
	m->out_fb        = out->fb;
	m->out_ticks0    = out->ticks0;
	m->out_ticks1    = out->ticks1;
	m->out_tris      = out->tris;
	m->out_dropped   = out->dropped;
	m->out_frames    = out->frames;
	m->out_heartbeat = out->heartbeat;
	barrier();
	m->out_seq = seq;
	barrier();
}

bool tr_mbox_take_out(const volatile tr_mbox_t *m,
                      uint32_t                  last_seq,
                      tr_frame_out_t           *out,
                      uint32_t                 *seq,
                      void (*barrier)(void))
{
	uint32_t s0 = m->out_seq;
	if (s0 == last_seq) return false;

	barrier();
	out->fb        = m->out_fb;
	out->ticks0    = m->out_ticks0;
	out->ticks1    = m->out_ticks1;
	out->tris      = m->out_tris;
	out->dropped   = m->out_dropped;
	out->frames    = m->out_frames;
	out->heartbeat = m->out_heartbeat;
	barrier();

	if (m->out_seq != s0) return false;

	*seq = s0;
	return true;
}

/* src/platform/pwr_ring.h -- the +5V net's last TR_PWR_N power samples (hud.h), the HUD's power
 * graph: pushed by platform/rail5v_power.c's poll, read by platform/hud_l2.c. Pure C, header
 * only, so tests/host/test_pwr_ring.c runs exactly what the HE runs.
 *
 * Both sides run on the HE's one main thread (the poll in the game loop, the HUD in the present
 * that follows it), so there is no lock and no seqlock: a push is never torn by a read.
 */
#ifndef TR_PLATFORM_PWR_RING_H
#define TR_PLATFORM_PWR_RING_H

#include <stdint.h>

#include "../hud/hud.h" /* TR_PWR_N, TR_PWR_GAP */

typedef struct {
	int16_t  v[TR_PWR_N]; /* a ring: sample number n lives at v[n % TR_PWR_N] */
	uint32_t seq;         /* samples pushed so far */
} tr_pwr_ring_t;

static inline void tr_pwr_ring_init(tr_pwr_ring_t *r)
{
	for (int i = 0; i < TR_PWR_N; i++) {
		r->v[i] = TR_PWR_GAP;
	}
	r->seq = 0;
}

/* Push one sample, mW; negative (the caller's "no sample") is a gap, and a value an int16 cannot
 * hold is clamped, never wrapped into a negative that would read as a gap. */
static inline void tr_pwr_ring_push(tr_pwr_ring_t *r, int32_t mw)
{
	r->v[r->seq % TR_PWR_N] = mw < 0           ? (int16_t)TR_PWR_GAP
	                          : mw > INT16_MAX ? INT16_MAX
	                                           : (int16_t)mw;
	r->seq++;
}

/* The window oldest first into out[TR_PWR_N] (hud.h tr_hud_view_t.pwr); samples older than the
 * first push are gaps. Returns the push count (tr_hud_view_t.pwr_seq). */
static inline uint32_t tr_pwr_ring_read(const tr_pwr_ring_t *r, int16_t out[TR_PWR_N])
{
	uint32_t seq = r->seq;

	for (uint32_t i = 0; i < TR_PWR_N; i++) {
		/* out[i] is sample number seq - TR_PWR_N + i */
		out[i] = seq + i < TR_PWR_N ? (int16_t)TR_PWR_GAP : r->v[(seq - TR_PWR_N + i) % TR_PWR_N];
	}
	return seq;
}

#endif /* TR_PLATFORM_PWR_RING_H */

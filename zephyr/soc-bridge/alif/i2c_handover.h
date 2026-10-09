/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The flag-word protocol of the alp,i2c-handover glue (i2c_handover.c), pure C
 * so a host test can drive it (examples/aen/aen-trace-runner/tests/host/
 * test_i2c_handover.c).
 *
 * Three 32-bit words at the node's flag-address, in always-on SRAM both cores see:
 *
 *   state    0 not released; CLEAN the releasing core stopped its controller and
 *            it went idle; DIRTY it was released but the controller never went
 *            idle (the acquiring core runs its bus recovery next, as it always
 *            does, and is told).
 *   nonce    the releasing core's per-boot value, written BEFORE state.
 *   consumed the acquiring core's last nonce it took the bus for.
 *
 * A release is taken exactly once: the acquiring core takes it when state is
 * CLEAN or DIRTY and nonce differs from consumed, then records the nonce in
 * consumed and clears state. A release that was already taken therefore never
 * satisfies a later wait.
 *
 * Residual window, by design: a release that was published but NOT yet taken
 * survives a warm reset (SRAM0 is always on). If the releasing core resets
 * before the acquiring core took it, an acquiring core that reaches its wait
 * before the releasing core's boot-time reset of the state can take that old
 * release while the releasing core is about to configure the bus again. On a
 * shield with no bridge (the releasing core keeps the bus disabled) this is
 * harmless; with a bridge on the bus it needs the two cores to be reset
 * separately, which the rule below forbids.
 *
 * WARM BOOT of the releasing core alone (the acquiring core keeps running and owns
 * the bus): the record then reads "taken" -- state 0, nonce nonzero, nonce equal to
 * consumed -- and the acquiring core's liveness word (the node's alive-address, e.g.
 * a heartbeat counter) advances. Both are needed: cold SRAM can look "taken" (all
 * zero apart from the nonce test, or any pattern with nonce == consumed), and only a
 * running acquiring core moves a counter. The acquiring core drives that word from a
 * timer it starts right after it took the bus (not from its main loop, whose passes can take
 * seconds), so it moves only while that core is up AND holds the bus. A releasing core that
 * detects this must not
 * touch the controller at all (no driver init, no reset, no NVIC line, no release):
 * alp_i2c_handover_warm_boot() reports it to the drivers that would have.
 *
 * Reset both cores together, and start the releasing core first, whenever the
 * releasing core has no alive-address. An acquiring core that is already past its
 * wait is not told when such a releasing core resets and configures the bus again;
 * a restarted acquiring core waits for a release that will not come again.
 */
#ifndef ALP_I2C_HANDOVER_H
#define ALP_I2C_HANDOVER_H

#include <stdbool.h>
#include <stdint.h>

#define ALP_I2C_HANDOVER_CLEAN 0x31433249u /* 'I2C1' */
#define ALP_I2C_HANDOVER_DIRTY 0x21433249u /* 'I2C!' */

typedef struct {
	volatile uint32_t state;
	volatile uint32_t nonce;
	volatile uint32_t consumed;
} alp_i2c_handover_t;

#define ALP_I2C_HANDOVER_NOT_YET 0
#define ALP_I2C_HANDOVER_TAKEN   1 /* released clean */
#define ALP_I2C_HANDOVER_DIRTIED 2 /* released, controller never went idle */

#ifndef ALP_I2C_HANDOVER_BARRIER
#define ALP_I2C_HANDOVER_BARRIER() __asm__ volatile("" ::: "memory")
#endif

/* Releasing core, first thing at every boot. */
static inline void alp_i2c_handover_reset(alp_i2c_handover_t *w)
{
	w->state = 0u;
}

/* Releasing core, when it is done with the bus. `nonce` should differ per boot; one
 * equal to the consumed word is bumped so the acquiring core cannot mistake it for
 * the release it already took. */
static inline void alp_i2c_handover_publish(alp_i2c_handover_t *w, uint32_t nonce, int dirty)
{
	if (nonce == w->consumed) {
		nonce++;
	}
	w->nonce = nonce;
	ALP_I2C_HANDOVER_BARRIER();
	w->state = dirty ? ALP_I2C_HANDOVER_DIRTY : ALP_I2C_HANDOVER_CLEAN;
	ALP_I2C_HANDOVER_BARRIER();
}

/* Releasing core, boot: the acquiring core took the last release (nothing pending, a nonce
 * that was published and then consumed). Cold SRAM passes this by accident only when
 * nonce == consumed and nonzero, and never with a state word other than 0. */
static inline bool alp_i2c_handover_taken(const alp_i2c_handover_t *w)
{
	return w->state == 0u && w->nonce != 0u && w->nonce == w->consumed;
}

/* Releasing core, boot: the acquiring core is running AND had taken the bus. `alive_a`/`alive_b`
 * are two samples of its liveness word, taken far enough apart for it to have moved. */
static inline bool
alp_i2c_handover_is_warm(const alp_i2c_handover_t *w, uint32_t alive_a, uint32_t alive_b)
{
	return alp_i2c_handover_taken(w) && alive_a != alive_b;
}

/* True when this releasing core booted warm (see above); valid from the glue's boot-time sample
 * on. Always false where the node has no alive-address. */
bool alp_i2c_handover_warm_boot(void);

/* Releasing core, FIRST thing at boot (before any acquiring core can reach its wait): remember
 * whether the record reads "taken" and, if it does not, clear `state`. A release published by
 * an earlier boot that was never taken (this core died before the other took it, the whole SoC
 * was reset) must not be taken now: the acquiring core would own the bus before this core's
 * drivers are done with it, and this core would later see a "taken" record and a moving
 * liveness word and call itself warm. Returns the "taken" verdict. */
static inline bool alp_i2c_handover_boot_snapshot(alp_i2c_handover_t *w)
{
	bool taken = alp_i2c_handover_taken(w);

	if (!taken) {
		w->state = 0u;
		ALP_I2C_HANDOVER_BARRIER();
	}
	return taken;
}

/* Liveness sampling: step and window. The acquiring core's timer period is
 * ALP_I2C_HANDOVER_ALIVE_PERIOD_MS, so the word moves at least ALP_I2C_HANDOVER_ALIVE_WINDOW_US /
 * period times in the window (i2c_handover.c asserts at least 2). */
#define ALP_I2C_HANDOVER_ALIVE_STEP_US   2000u
#define ALP_I2C_HANDOVER_ALIVE_WINDOW_US 100000u
#define ALP_I2C_HANDOVER_ALIVE_PERIOD_MS 10u

/* Releasing core, boot: is this a warm boot? `taken` is alp_i2c_handover_boot_snapshot()'s
 * verdict, `alive` the other core's liveness word (NULL: none, always cold). The word is sampled
 * every `step_us` for up to `window_us`, `delay_us` being the caller's wait; no wait at all when
 * the record is not "taken". */
static inline bool alp_i2c_handover_sample_warm(const alp_i2c_handover_t *w,
                                                bool                      taken,
                                                const volatile uint32_t  *alive,
                                                void (*delay_us)(uint32_t),
                                                uint32_t step_us,
                                                uint32_t window_us)
{
	uint32_t a, b;

	if (alive == NULL || !taken) {
		return false;
	}
	a = b = *alive;
	for (uint32_t t = 0; t < window_us && b == a; t += step_us) {
		delay_us(step_us);
		b = *alive;
	}
	return alp_i2c_handover_is_warm(w, a, b);
}

/* Acquiring core, polled: NOT_YET, or TAKEN / DIRTIED once it holds the bus (the
 * release is consumed). */
static inline int alp_i2c_handover_try_acquire(alp_i2c_handover_t *w)
{
	uint32_t s = w->state;

	ALP_I2C_HANDOVER_BARRIER(); /* the nonce is read after the state it belongs to */
	if ((s != ALP_I2C_HANDOVER_CLEAN && s != ALP_I2C_HANDOVER_DIRTY) || w->nonce == w->consumed) {
		return ALP_I2C_HANDOVER_NOT_YET;
	}
	w->consumed = w->nonce;
	ALP_I2C_HANDOVER_BARRIER();
	w->state = 0u;
	ALP_I2C_HANDOVER_BARRIER();
	return s == ALP_I2C_HANDOVER_CLEAN ? ALP_I2C_HANDOVER_TAKEN : ALP_I2C_HANDOVER_DIRTIED;
}

#endif /* ALP_I2C_HANDOVER_H */

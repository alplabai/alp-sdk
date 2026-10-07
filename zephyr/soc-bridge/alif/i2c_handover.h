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
 * consumed and clears state. A magic left in SRAM0 by a release that was
 * already taken (or by a previous run: SRAM0 survives a warm reset) therefore
 * never satisfies a later wait.
 *
 * Known hazard: an acquiring core that is already past its wait is not told
 * when the releasing core resets and configures the bus again; reset both.
 */
#ifndef ALP_I2C_HANDOVER_H
#define ALP_I2C_HANDOVER_H

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

/* Acquiring core, polled: NOT_YET, or TAKEN / DIRTIED once it holds the bus (the
 * release is consumed). */
static inline int alp_i2c_handover_try_acquire(alp_i2c_handover_t *w)
{
	uint32_t s = w->state;

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

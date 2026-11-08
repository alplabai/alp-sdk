/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure bookkeeping for counter_alif_lptimer.c (no register access, no Zephyr
 * includes) so it can be unit-tested against a fake down-counter.
 *
 * The LPTIMER channel is a single DOWN-counter with one LOADCOUNT register and no
 * compare register, so an alarm can only be armed by reloading it, which throws
 * the running count away.  The driver keeps a software `base` (ticks elapsed
 * before the current hardware load, mod 2^32) and reports a virtual free-running
 * down-counter:
 *
 *     value = 0xFFFFFFFF - (base + (load - CURRENTVAL))
 *
 * In free-run (load == 0xFFFFFFFF, base == 0) that is exactly CURRENTVAL.
 *
 * RESIDUAL ERROR.  A fold reads CURRENTVAL and then reloads LOADCOUNT; the
 * clocks that elapse between the read and the reload taking effect (a few 32 kHz
 * ticks, incl. the HWRM 13.1.4.4 clock-domain sync) are lost, so each
 * set_alarm()/cancel_alarm() loses a few ticks of virtual time.  The count
 * never jumps back.  ISR latency is NOT lost: the ISR folds (period + clocks
 * since the reload), valid while latency < one alarm period.
 */
#ifndef ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_CLOCK_H_
#define ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_CLOCK_H_

#include <stdbool.h>
#include <stdint.h>

#define LPTIMER_CLK_FREE_LOAD 0xFFFFFFFFU

struct lptimer_clock {
	uint32_t base; /* ticks elapsed before the current load, mod 2^32 */
	uint32_t load; /* LOADCOUNT as programmed (period is load + 1 clocks) */
};

static inline void lptimer_clock_reset(struct lptimer_clock *c)
{
	c->base = 0U;
	c->load = LPTIMER_CLK_FREE_LOAD;
}

/* Clocks since the current load took effect.  `underflowed`: the counter has
 * hit zero and reloaded once since (alarm pending / ISR not yet run).
 *
 * CURRENTVAL can read above `load` for a clock or so after LOADCOUNT is
 * reprogrammed: the new value only lands on the next timer clock, so the
 * register still shows the previous segment's count.  Measured on E8 silicon
 * (E1M-AEN803): without this clamp the value jumped ahead by load + 1 right
 * after set_alarm and then stood still until the alarm fired.  A reading above
 * `load` means the new segment has not started yet, i.e. 0 clocks elapsed. */
static inline uint32_t
lptimer_clock_seg_elapsed(const struct lptimer_clock *c, uint32_t cur, bool underflowed)
{
	uint32_t in_seg = (cur > c->load) ? 0U : (c->load - cur);

	return (underflowed ? c->load + 1U : 0U) + in_seg;
}

/* Virtual free-running down-counter value. */
static inline uint32_t
lptimer_clock_value(const struct lptimer_clock *c, uint32_t cur, bool underflowed)
{
	return LPTIMER_CLK_FREE_LOAD - (c->base + lptimer_clock_seg_elapsed(c, cur, underflowed));
}

/* Fold the elapsed segment into base; the caller then reloads the hardware
 * with `new_load`. */
static inline void
lptimer_clock_fold(struct lptimer_clock *c, uint32_t cur, bool underflowed, uint32_t new_load)
{
	c->base += lptimer_clock_seg_elapsed(c, cur, underflowed);
	c->load = new_load;
}

#endif /* ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_CLOCK_H_ */

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for the alp_delay_* primitives in <alp/peripheral.h>.
 *
 * alp_delay_us routes to k_busy_wait so callers with sub-millisecond
 * hardware-timing requirements (chip power-on hold times, bus
 * deassert intervals, post-write settle delays) get a cycle-accurate
 * spin without yielding the scheduler.
 *
 * alp_delay_ms routes to k_msleep so longer waits release the CPU
 * to other threads.  Both honour the "0 = no-op" contract.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"

void alp_delay_us(uint32_t us)
{
	if (us == 0u) return;
	k_busy_wait(us);
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	k_msleep((int32_t)ms);
}

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- master/slave timer sync.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* Master/slave id (host-side enum) -> GD32 peripheral base address.
 * The protocol-level contract numbers the three advanced timers
 * compactly as 0 = TIMER0, 1 = TIMER7, 2 = TIMER19; this keeps the
 * wire byte small without naming the underlying SoC peripheral. */
static uint32_t timer_sync_periph(uint8_t id)
{
	switch (id) {
	case 0u:
		return TIMER0;
	case 1u:
		return TIMER7;
	case 2u:
		return TIMER19;
	default:
		return 0u;
	}
}

int bridge_hw_timer_sync(uint8_t master, uint8_t slave, uint8_t mode)
{
	if (master == slave) return BRIDGE_HW_ERR_INVAL;
	const uint32_t mp = timer_sync_periph(master);
	const uint32_t sp = timer_sync_periph(slave);
	if (mp == 0u || sp == 0u) return BRIDGE_HW_ERR_RANGE;

	/* Translate the wire `mode` byte (host-defined: 0 disabled,
     * 1 reset, 2 gated, 3 trigger, 4 external-clock, 5 encoder-mode-1)
     * to the vendor's SMC encoding (defined in gd32g5x3_syscfg.h:
     * TIMER_SLAVE_MODE_* + TIMER_QUAD_DECODER_MODE*).  The two
     * encodings differ in numeric value, so an explicit table is
     * needed here; anything outside the documented range returns
     * INVAL rather than silently selecting a default. */
	uint32_t slave_mode;
	switch (mode) {
	case 0u:
		slave_mode = TIMER_SLAVE_MODE_DISABLE;
		break;
	case 1u:
		slave_mode = TIMER_SLAVE_MODE_RESTART;
		break;
	case 2u:
		slave_mode = TIMER_SLAVE_MODE_PAUSE;
		break;
	case 3u:
		slave_mode = TIMER_SLAVE_MODE_EVENT;
		break;
	case 4u:
		slave_mode = TIMER_SLAVE_MODE_EXTERNAL0;
		break;
	case 5u:
		slave_mode = TIMER_QUAD_DECODER_MODE1;
		break;
	default:
		return BRIDGE_HW_ERR_INVAL;
	}

	/* Master side: emit the update event as TRGO0 (the canonical
     * "I just rolled over" pulse that synchronises a downstream
     * slave to the master's period) + flip the master-slave mode
     * bit so the master broadcasts its trigger.  Idempotent. */
	timer_master_output0_trigger_source_select(mp, TIMER_TRI_OUT0_SRC_UPDATE);
	timer_master_slave_mode_config(mp, TIMER_MASTER_SLAVE_MODE_ENABLE);

	/* Slave side: listen to internal trigger 0 (ITI0).  The
     * SYSCFG_TIMERCFG router maps the slave's ITI0 to a physical
     * upstream TIMER's TRGO; the chip-default routing is the
     * reference-manual table for the slave-master pair selected
     * here, and bring-up tests will tune SYSCFG if a non-default
     * routing is needed for a given (master, slave) combination.
     * Following the vendor pattern, configure the slave-mode AFTER
     * the input-trigger selection so the slave doesn't act on a
     * stale TRGI source. */
	timer_input_trigger_source_select(sp, TIMER_SMCFG_TRGSEL_ITI0);
	timer_slave_mode_select(sp, slave_mode);
	return BRIDGE_HW_OK;
}

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- GPIO proxy (v0.4), camera-enable LDOs,
 * and the bridge READY/host-IRQ line.
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  cc3501e_hw_ti.c keeps platform lifecycle + the deferred-reboot
 * latch; see cc3501e_hw_ti_internal.h for the cross-TU seam the split uses.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ti_drivers_config.h" /* GPIO_pinUpperBound (SysConfig board file) */

#include <ti/drivers/GPIO.h>

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "transport.h" /* cc3501e_bridge_busy/ready prototypes (worker's weak hooks) */

/* --------------------------------------------------------------- */
/* GPIO proxy (v0.4) -- real CC3501E pad I/O via TI Drivers GPIO.    */
/*                                                                   */
/* The CC35xx TI Drivers GPIO layer is PIN-INDEXED: gpioPinConfigs[] */
/* (ti_drivers_config.c) is indexed by the GPIO pad number directly  */
/* (GPIO0..GPIO37, GPIO_pinUpperBound=37) and GPIO_setConfig/write/  */
/* read(index,...) take that pad number.  So the protocol's raw      */
/* cc3501e_gpio index drives the pad 1:1 -- NO logical IO11/IO13..    */
/* -> pad map is needed in firmware (the host owns the logical map,  */
/* metadata/e1m_modules/aen/from-cc3501e.tsv).  The guard below       */
/* refuses the pads the bridge itself owns (the CONFIG_SPI_0 lines +  */
/* the CONFIG_UART2_0 console glue) and the pads not bonded on        */
/* CC35X1E, so a stray host GPIO command can never tear down the      */
/* inter-chip link mid-transfer. */
static bool gpio_pad_reserved(uint8_t pad)
{
	switch (pad) {
	/* CONFIG_SPI_0 inter-chip bridge: CSN=16, SCLK=27, POCI=28, PICO=29. */
	case 16u:
	case 27u:
	case 28u:
	case 29u:
	/* GPIO17 (E1M IO16) = the bridge READY/host-IRQ line, firmware-owned (rev-1
	 * host-IRQ wire: CC35 GPIO17 -> Alif P2_6; GPIO17 is not SPI0-CS-capable,
	 * GPIO16 is the CS).  Reserve it so a host GPIO-proxy command can't clobber
	 * the flow-control line. */
	case 17u:
	/* CONFIG_UART2_0 console glue: TX=5, RX=6. */
	case 5u:
	case 6u:
	/* Not bonded on this device (gpioPinConfigs marks 7/8/9 unavailable). */
	case 7u:
	case 8u:
	case 9u:
		return true;
	default:
		return false;
	}
}

static bool gpio_pad_ok(uint8_t pad)
{
	return (pad <= GPIO_pinUpperBound) && !gpio_pad_reserved(pad);
}

/* GPIO interrupt handler.  Async EVT_GPIO_INTERRUPT delivery to the host needs a
 * slave->master attention path: the rev-1 host-IRQ wire (CC35 GPIO17 -> Alif P2_6)
 * is currently dedicated to bridge READY/flow-control, not yet multiplexed for
 * async GPIO-event delivery, so this only clears the pending edge for now; the HW
 * arming (cc3501e_hw_gpio_set_interrupt) itself is real. */
static void gpio_irq_cb(uint_least8_t index)
{
	GPIO_clearInt(index);
}

int cc3501e_hw_gpio_configure(uint8_t pad, uint8_t dir, uint8_t pull)
{
	if (!gpio_pad_ok(pad)) {
		return CC3501E_HW_ERR_INVAL;
	}
	GPIO_PinConfig pull_cfg = (pull == ALP_CC3501E_GPIO_PULL_UP)     ? GPIO_CFG_PULL_UP_INTERNAL
	                          : (pull == ALP_CC3501E_GPIO_PULL_DOWN) ? GPIO_CFG_PULL_DOWN_INTERNAL
	                                                                 : GPIO_CFG_PULL_NONE_INTERNAL;
	GPIO_PinConfig cfg;
	switch (dir) {
	case ALP_CC3501E_GPIO_DIR_OUTPUT:
		/* push-pull, start low; host sets the level with GPIO_WRITE. */
		cfg = GPIO_CFG_OUTPUT_INTERNAL | pull_cfg | GPIO_CFG_OUT_LOW;
		break;
	case ALP_CC3501E_GPIO_DIR_OPEN_DRAIN:
		/* The CC35xx GPIOWFF3 controller has NO true open-drain output
		 * (GPIO_CFG_OUTPUT_OPEN_DRAIN_INTERNAL is NOT_SUPPORTED).  Emulate
		 * with a push-pull output idling HIGH: on a single-driver line --
		 * the M.2 W_DISABLE contract (host drives low to assert; the board
		 * pull-up holds high when released) -- this is electrically
		 * equivalent.  NOT safe on a line with another active driver. */
		cfg = GPIO_CFG_OUTPUT_INTERNAL | pull_cfg | GPIO_CFG_OUT_HIGH;
		break;
	case ALP_CC3501E_GPIO_DIR_INPUT:
	default:
		cfg = GPIO_CFG_INPUT_INTERNAL | pull_cfg | GPIO_CFG_IN_INT_NONE;
		break;
	}
	return (GPIO_setConfig(pad, cfg) == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

int cc3501e_hw_gpio_write(uint8_t pad, uint8_t level)
{
	if (!gpio_pad_ok(pad)) {
		return CC3501E_HW_ERR_INVAL;
	}
	GPIO_write(pad, (level != 0u) ? 1u : 0u);
	return CC3501E_HW_OK;
}

/* ---- Bridge READY/host-IRQ line: CC35 GPIO17 (E1M IO16) -> Alif P2_6 ------- *
 * Strong overrides of the worker's weak cc3501e_bridge_busy/ready() hooks.  The
 * line is flow-control to the host master: LOW = the bridge is BUSY (a radio op
 * is running and the SPI-slave DMA is dead, do not clock), HIGH = the slave is
 * armed and the host may clock a transaction.  Lazily configured push-pull,
 * idling LOW (busy) until the first ready() so the host holds off through boot.
 * GPIO17 = the silicon-legal sibling of the SPI0 CS pair (it is not CS-capable,
 * GPIO16 is the CS); reserved from the host GPIO proxy in gpio_pad_reserved(). */
#define CC3501E_READY_GPIO 17u

static bool ready_inited;

static void ready_ensure_init(void)
{
	if (!ready_inited) {
		(void)GPIO_setConfig(CC3501E_READY_GPIO,
		                     GPIO_CFG_OUTPUT_INTERNAL | GPIO_CFG_PULL_NONE_INTERNAL |
		                         GPIO_CFG_OUT_LOW);
		ready_inited = true;
	}
}

void cc3501e_bridge_busy(void)
{
	ready_ensure_init();
	GPIO_write(CC3501E_READY_GPIO, 0u); /* LOW = busy */
}

void cc3501e_bridge_ready(void)
{
	ready_ensure_init();
	GPIO_write(CC3501E_READY_GPIO, 1u); /* HIGH = ready */
}

int cc3501e_hw_gpio_read(uint8_t pad, uint8_t *level_out)
{
	if (!gpio_pad_ok(pad)) {
		if (level_out != 0) {
			*level_out = 0u;
		}
		return CC3501E_HW_ERR_INVAL;
	}
	uint8_t lvl = (GPIO_read(pad) != 0u) ? 1u : 0u;
	if (level_out != 0) {
		*level_out = lvl;
	}
	return CC3501E_HW_OK;
}

int cc3501e_hw_gpio_set_interrupt(uint8_t pad, uint8_t edge, uint8_t enabled)
{
	if (!gpio_pad_ok(pad)) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (enabled == 0u || edge == ALP_CC3501E_GPIO_EDGE_NONE) {
		/* Disable: back to a plain interrupt-free input. */
		return (GPIO_setConfig(pad, GPIO_CFG_INPUT_INTERNAL | GPIO_CFG_IN_INT_NONE) == 0)
		           ? CC3501E_HW_OK
		           : CC3501E_HW_ERR_IO;
	}
	GPIO_PinConfig edge_cfg;
	switch (edge) {
	case ALP_CC3501E_GPIO_EDGE_RISING:
		edge_cfg = GPIO_CFG_IN_INT_RISING;
		break;
	case ALP_CC3501E_GPIO_EDGE_FALLING:
		edge_cfg = GPIO_CFG_IN_INT_FALLING;
		break;
	case ALP_CC3501E_GPIO_EDGE_BOTH:
		/* GPIOWFF3 has no both-edges trigger (NOT_SUPPORTED) -- reject so
		 * the host arms a single edge instead of getting a silent no-op. */
		return CC3501E_HW_ERR_INVAL;
	default:
		return CC3501E_HW_ERR_INVAL;
	}
	/* Register the latch callback BEFORE enabling the edge (INT_ENABLE in
	 * the config self-enables the line -- no separate GPIO_enableInt). */
	GPIO_setCallback(pad, gpio_irq_cb);
	return (GPIO_setConfig(pad, GPIO_CFG_INPUT_INTERNAL | edge_cfg | GPIO_CFG_INT_ENABLE) == 0)
	           ? CC3501E_HW_OK
	           : CC3501E_HW_ERR_IO;
}

int cc3501e_hw_cam_enable(uint8_t which, uint8_t on)
{
	/* CAM_EN_LDO0 -> GPIO_1, CAM_EN_LDO1 -> GPIO_0 -- per the E1M-AEN (BDE-BW35N
	 * module U4) netlist: pin54 GPIO_1 = R_CAM_EN_LDO0, pin55 GPIO_0 = R_CAM_EN_LDO1.
	 * (Earlier code/header had this REVERSED; the SWRU626-era note was wrong.)
	 * Push-pull, default-off at boot. */
	uint8_t pad;
	switch (which) {
	case 0u:
		pad = 1u; /* CAM_EN_LDO0 -> GPIO_1 */
		break;
	case 1u:
		pad = 0u; /* CAM_EN_LDO1 -> GPIO_0 */
		break;
	default:
		return CC3501E_HW_ERR_INVAL;
	}
	GPIO_PinConfig cfg = GPIO_CFG_OUTPUT_INTERNAL | GPIO_CFG_PULL_NONE_INTERNAL |
	                     ((on != 0u) ? GPIO_CFG_OUT_HIGH : GPIO_CFG_OUT_LOW);
	if (GPIO_setConfig(pad, cfg) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	GPIO_write(pad, (on != 0u) ? 1u : 0u);
	return CC3501E_HW_OK;
}

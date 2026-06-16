/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- chip lifecycle + meta operations.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK (the CC3501E is a SimpleLink Wi-Fi 6 + BLE 5.4
 * wireless MCU; we run this firmware on its application core).  CI
 * builds the stub backend instead, so this file is never on the
 * SDK-free path.
 *
 * API grounding: this uses the stable SimpleLink host API
 * (`sl_Start` / `sl_NetCfgGet`) plus CMSIS `NVIC_SystemReset()` for the
 * MCU reset.  Two board-specific anchors come from the SDK's SysConfig
 * output (`ti_drivers_config.h`) generated for the LP-EM-CC35X1 / the
 * E1M-AEN board file -- they are resolved at bench-build time, not
 * invented here.
 */

#include <stdbool.h>
#include <stdint.h>

/* CMSIS core (for NVIC_SystemReset) -- pulled in via the SDK's device
 * header; ti_drivers_config.h transitively includes the CMSIS core for
 * the CC35xx M33. */
#include "ti_drivers_config.h"

#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>
#include <ti/drivers/net/wifi/simplelink.h>

#include "../cc3501e_hw.h"

/* SimpleLink network-processor lifecycle.  PING / GET_VERSION answer
 * without the NWP running; it is started lazily on the first request
 * that needs the radio subsystem (GET_MAC reads the factory MAC from
 * it) and kept running thereafter. */
static bool sl_running;

/* Deferred-reset latch: CMD_RESET sets this; cc3501e_hw_tick() performs
 * the reboot on the next idle wakeup, after the SPI/SDIO transport has
 * staged + begun clocking the OK ack back to the host. */
static volatile bool reset_pending;

static int ensure_simplelink_started(void)
{
	if (sl_running) {
		return CC3501E_HW_OK;
	}
	/* sl_Start returns the device role (>=0) on success, a negative
     * error otherwise.  NULL args = default interface / no init
     * callback (synchronous start). */
	int16_t role = sl_Start(NULL, NULL, NULL);
	if (role < 0) {
		return CC3501E_HW_ERR_IO;
	}
	sl_running = true;
	return CC3501E_HW_OK;
}

void cc3501e_hw_init(void)
{
	/* TI Drivers core init.  GPIO + SPI are brought up here so the
     * transport's hw-init hook can open its handles.  SimpleLink (the
     * radio) is started lazily -- see ensure_simplelink_started(). */
	GPIO_init();
	SPI_init();
}

void cc3501e_hw_tick(void)
{
	if (reset_pending) {
		/* Bring the NWP down cleanly if it was started, then reset the
         * application core.  By the time the idle loop runs this tick the
         * transport has already staged + started clocking the CMD_RESET
         * ack, so the host sees the ack then the link going quiet. */
		if (sl_running) {
			(void)sl_Stop(200 /* ms */);
			sl_running = false;
		}
		NVIC_SystemReset(); /* CMSIS: M33 system reset -- does not return */
	}
}

int cc3501e_hw_get_mac(uint8_t mac[6])
{
	if (mac == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int rv = ensure_simplelink_started();
	if (rv != CC3501E_HW_OK) {
		return rv;
	}
	/* Read the factory MAC.  MAC config takes no config-option; the
     * length is SL_MAC_ADDR_LEN (6). */
	uint16_t len = 6u;
	int32_t  rc  = sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET, NULL, &len, mac);
	if (rc < 0 || len != 6u) {
		return CC3501E_HW_ERR_IO;
	}
	return CC3501E_HW_OK;
}

void cc3501e_hw_request_reset(void)
{
	reset_pending = true;
}

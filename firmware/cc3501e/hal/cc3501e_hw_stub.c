/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default HAL implementation: hardware-free.  Lifecycle hooks are
 * no-ops and HW-touching ops return CC3501E_HW_ERR_NOTIMPL, so the
 * protocol round-trip (PING / GET_VERSION) is exercisable on the host
 * with no TI SimpleLink SDK on the workspace.
 *
 * The real implementation against TI's SimpleLink CC33xx SDK lives
 * under hal/ti/; the build picks one or the other via
 * CC3501E_HAL_BACKEND in CMakeLists.txt.
 */

#include <stdint.h>

#include "cc3501e_hw.h"

void cc3501e_hw_init(void)
{
	/* no-op on the stub backend */
}

void cc3501e_hw_tick(void)
{
	/* no-op on the stub backend */
}

int cc3501e_hw_get_mac(uint8_t mac[6])
{
	/* No radio on the host stub -- zero the buffer and report NOTIMPL so
     * the protocol layer answers RESP_ERR_NOT_READY rather than handing
     * back a fabricated MAC. */
	if (mac != 0) {
		for (unsigned i = 0u; i < 6u; ++i)
			mac[i] = 0u;
	}
	return CC3501E_HW_ERR_NOTIMPL;
}

void cc3501e_hw_request_reset(void)
{
	/* no-op on the stub backend */
}

void cc3501e_hw_notify_reply_sent(void)
{
	/* no-op on the stub backend */
}

/* --------------------------------------------------------------- */
/* GPIO proxy (v0.4) -- in-memory simulation so the host protocol    */
/* path (configure/write/read/IRQ-arm + camera enables) is fully     */
/* exercisable on the host with no silicon.  Real pad I/O is in       */
/* hal/ti/.                                                          */
/* --------------------------------------------------------------- */
#define STUB_GPIO_MAX 32u
static uint8_t stub_gpio_level[STUB_GPIO_MAX];
static uint8_t stub_cam[2];

int cc3501e_hw_gpio_configure(uint8_t pad, uint8_t dir, uint8_t pull)
{
	(void)dir;
	(void)pull;
	if (pad >= STUB_GPIO_MAX) return CC3501E_HW_ERR_INVAL;
	return CC3501E_HW_OK;
}

int cc3501e_hw_gpio_write(uint8_t pad, uint8_t level)
{
	if (pad >= STUB_GPIO_MAX) return CC3501E_HW_ERR_INVAL;
	stub_gpio_level[pad] = level ? 1u : 0u;
	return CC3501E_HW_OK;
}

int cc3501e_hw_gpio_read(uint8_t pad, uint8_t *level_out)
{
	if (pad >= STUB_GPIO_MAX || level_out == 0) return CC3501E_HW_ERR_INVAL;
	*level_out = stub_gpio_level[pad];
	return CC3501E_HW_OK;
}

int cc3501e_hw_gpio_set_interrupt(uint8_t pad, uint8_t edge, uint8_t enabled)
{
	(void)edge;
	(void)enabled;
	if (pad >= STUB_GPIO_MAX) return CC3501E_HW_ERR_INVAL;
	return CC3501E_HW_OK;
}

int cc3501e_hw_cam_enable(uint8_t which, uint8_t on)
{
	if (which > 1u) return CC3501E_HW_ERR_INVAL;
	stub_cam[which] = on ? 1u : 0u;
	return CC3501E_HW_OK;
}

/* --------------------------------------------------------------- */
/* Wi-Fi (v0.2) -- no radio on the host stub: report NOTIMPL so the  */
/* protocol path stays exercisable (the handlers parse + validate,    */
/* then map NOTIMPL -> RESP_ERR_NOT_READY).                           */
/* --------------------------------------------------------------- */
int cc3501e_hw_wifi_scan_start(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_scan_stop(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_connect_sta(const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk,
                                uint8_t psk_len, uint8_t security)
{
	(void)ssid;
	(void)ssid_len;
	(void)psk;
	(void)psk_len;
	(void)security;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_disconnect(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_ap_start(const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk,
                             uint8_t psk_len, uint8_t security)
{
	(void)ssid;
	(void)ssid_len;
	(void)psk;
	(void)psk_len;
	(void)security;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_ap_stop(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_get_rssi(int8_t *rssi_dbm_out)
{
	if (rssi_dbm_out != 0) *rssi_dbm_out = 0;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_get_ip(uint8_t ip_out[4])
{
	(void)ip_out;
	return CC3501E_HW_ERR_NOTIMPL;
}

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

#include "alp/protocol/cc3501e.h"
#include "cc3501e_hw.h"

void cc3501e_hw_init(void)
{
	/* no-op on the stub backend */
}

void cc3501e_hw_tick(void)
{
	/* no-op on the stub backend */
}

void cc3501e_hw_wifi_boot_start(void)
{
	/* No radio on the host stub -- nothing to bring up at boot. */
}

void cc3501e_hw_net_init(void)
{
	/* No lwIP on the host stub -- nothing to bring up. */
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

int cc3501e_hw_wifi_scan(uint8_t *buf, size_t cap, size_t *out_len)
{
	(void)buf;
	(void)cap;
	if (out_len != 0) *out_len = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_wifi_connect_sta(
    const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk, uint8_t psk_len, uint8_t security)
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

int cc3501e_hw_wifi_ap_start(
    const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk, uint8_t psk_len, uint8_t security)
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

void cc3501e_hw_wifi_mark_connecting(void)
{
	/* No radio on the host stub -- the connect-status latch stays DISCONNECTED. */
}

int cc3501e_hw_wifi_conn_status(uint8_t *state, uint8_t *fail_reason, int8_t *rssi_dbm)
{
	if (state != 0) *state = (uint8_t)ALP_CC3501E_WIFI_DISCONNECTED;
	if (fail_reason != 0) *fail_reason = (uint8_t)ALP_CC3501E_WIFI_FAIL_NONE;
	if (rssi_dbm != 0) *rssi_dbm = 0;
	return CC3501E_HW_OK;
}

/* --------------------------------------------------------------- */
/* BLE 5.4 (v0.3) -- no BLE host on the stub: report NOTIMPL so the   */
/* protocol path stays exercisable (handlers parse + validate, then   */
/* map NOTIMPL -> RESP_ERR_NOT_READY).                                */
/* --------------------------------------------------------------- */
int cc3501e_hw_ble_enable(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_disable(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_adv_start(uint8_t        connectable,
                             uint16_t       interval_min_ms,
                             uint16_t       interval_max_ms,
                             const uint8_t *adv_data,
                             uint8_t        adv_data_len)
{
	(void)connectable;
	(void)interval_min_ms;
	(void)interval_max_ms;
	(void)adv_data;
	(void)adv_data_len;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_adv_stop(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_scan_start(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_scan_stop(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_scan(uint8_t *buf, size_t cap, size_t *out_len)
{
	(void)buf;
	(void)cap;
	if (out_len != 0) {
		*out_len = 0u;
	}
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_connect(uint8_t addr_type, const uint8_t addr[6])
{
	(void)addr_type;
	(void)addr;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_disconnect(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_gatt_register(const uint8_t *desc, uint16_t desc_len)
{
	(void)desc;
	(void)desc_len;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t data_len)
{
	(void)handle;
	(void)data;
	(void)data_len;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len)
{
	(void)handle;
	(void)out;
	(void)cap;
	if (out_len != 0) *out_len = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t data_len)
{
	(void)handle;
	(void)data;
	(void)data_len;
	return CC3501E_HW_ERR_NOTIMPL;
}

/* --------------------------------------------------------------- */
/* OTA -- no PSA-FWU flash on the host stub: report NOTIMPL so the    */
/* protocol layer maps it to RESP_ERR_NOT_READY (the ztests assert    */
/* the handler framing/parse, not a real flash install).             */
/* --------------------------------------------------------------- */
int cc3501e_hw_ota_begin(uint32_t total_len)
{
	(void)total_len;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ota_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
	(void)offset;
	(void)data;
	(void)len;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ota_finish(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ota_abort(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ota_status(uint8_t *state, uint32_t *bytes_written, uint32_t *total_len)
{
	if (state != 0) *state = 0u;
	if (bytes_written != 0) *bytes_written = 0u;
	if (total_len != 0) *total_len = 0u;
	return CC3501E_HW_ERR_NOTIMPL;
}

/* --------------------------------------------------------------- */
/* Power policy + diagnostics -- firmware-side config (no radio), so */
/* the stub accepts config and reports a cold-start diag.            */
/* --------------------------------------------------------------- */
int cc3501e_hw_set_power_policy(uint8_t policy, uint8_t wake_events, uint32_t idle_ms_before_sleep)
{
	(void)policy;
	(void)wake_events;
	(void)idle_ms_before_sleep;
	return CC3501E_HW_OK;
}

int cc3501e_hw_set_log_level(uint8_t level)
{
	(void)level;
	return CC3501E_HW_OK;
}

uint8_t cc3501e_hw_reset_cause(void)
{
	return (uint8_t)ALP_CC3501E_RESET_POWER_ON;
}

uint32_t cc3501e_hw_uptime_ms(void)
{
	return 0u;
}

uint32_t cc3501e_hw_free_heap_bytes(void)
{
	return 0u;
}

uint32_t cc3501e_hw_wifi_last_event_id(void)
{
	return 0u;
}

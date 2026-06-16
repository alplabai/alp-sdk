/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- chip lifecycle + meta operations.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK (the CC3501E is a SimpleLink Wi-Fi 6 + BLE 5.4
 * wireless MCU; we run this firmware on its application core).  CI builds
 * the stub backend instead, so this file is never on the SDK-free path.
 *
 * v0.1 ("bring-up") scope: META group only (PING / GET_VERSION / GET_MAC /
 * RESET).  v0.1 deliberately pulls in NO Wi-Fi/BLE stack -- it links only
 * TI Drivers (SPI/GPIO) + device_cc35xx + the RTOS.  GET_MAC needs the
 * network processor, which v0.1 does not bring up, so it reports
 * RESP_ERR_NOT_READY; the real factory-MAC read (via the CC35xx SimpleLink
 * host API) lands in v0.2 alongside the Wi-Fi group.  This also keeps the
 * v0.1 ti build independent of the SDK's reorganized host-API header path
 * (SDK 10.10 moved it off the classic <ti/drivers/net/wifi/simplelink.h>).
 *
 * API grounding: CMSIS NVIC_SystemReset() for the MCU reset; the
 * board-specific anchors (CONFIG_SPI_0, pin mux) come from the SDK's
 * SysConfig output (ti_drivers_config.h) generated for the E1M-AEN board
 * file -- resolved at bench-build time, not invented here.
 */

#include <stdbool.h>
#include <stdint.h>

/* CMSIS core for NVIC_SystemReset (the CC35xx M33 core).  Pulled in via the
 * device's CMSIS header -- ti_drivers_config.h does NOT bring the core in
 * transitively on this SDK (SDK 10.10). */
#include <ti/devices/cc35xx/cmsis/device.h>

#include "ti_drivers_config.h"

#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>

#include "../cc3501e_hw.h"

/* Deferred-reset latch: CMD_RESET sets this; cc3501e_hw_tick() performs
 * the reboot on the next idle wakeup, but ONLY after reply_drained
 * confirms the OK ack has fully clocked back to the host. */
static volatile bool reset_pending;

/* Set by cc3501e_hw_notify_reply_sent() when the in-flight reply (for
 * CMD_RESET, the ack itself) has fully clocked out; cleared by
 * cc3501e_hw_request_reset() so only that command's own ack arms the
 * reboot.  Gates reset_pending in cc3501e_hw_tick(). */
static volatile bool reply_drained;

void cc3501e_hw_init(void)
{
	/* TI Drivers core init.  GPIO + SPI are brought up here so the
	 * transport's hw-init hook can open its handles.  No network
	 * processor in v0.1 (see file header). */
	GPIO_init();
	SPI_init();
}

void cc3501e_hw_tick(void)
{
	/* Deferred self-reset, gated on reply_drained so the CMD_RESET ack has
	 * FULLY clocked to the host before the chip resets (audit
	 * "reset-fires-before-ack-clocked": the reset previously raced the
	 * in-flight ack and the host never saw it).  The host sees the ack,
	 * then the link goes quiet, then the firmware re-PINGs alive. */
	if (reset_pending && reply_drained) {
		NVIC_SystemReset(); /* CMSIS: M33 system reset -- does not return */
	}
}

int cc3501e_hw_get_mac(uint8_t mac[6])
{
	if (mac == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* v0.1 brings up no network processor, so the factory MAC is not
	 * readable yet -- report NOTIMPL, which the protocol layer maps to
	 * RESP_ERR_NOT_READY.  v0.2 wires the real read through the CC35xx
	 * SimpleLink host API alongside the Wi-Fi group. */
	(void)mac;
	return CC3501E_HW_ERR_NOTIMPL;
}

void cc3501e_hw_request_reset(void)
{
	/* Clear reply_drained FIRST so only this command's own ack (the next
	 * reply to finish clocking) re-arms the reboot in cc3501e_hw_tick();
	 * a stale drained flag from an earlier reply must not reset early. */
	reply_drained = false;
	reset_pending = true;
}

void cc3501e_hw_notify_reply_sent(void)
{
	reply_drained = true;
}

/* --------------------------------------------------------------- */
/* GPIO proxy (v0.4) -- real CC3501E pad I/O.                        */
/*                                                                   */
/* TODO(cc3501e v0.4 bench): map the cc3501e GPIO index -> the       */
/* SysConfig CONFIG_GPIO_* instances the AEN board file declares for */
/* the proxied pads (IO11/IO13/IO15..IO21 + CAM_EN_LDO0/1 per        */
/* metadata/e1m_modules/aen/from-cc3501e.tsv), then drive via TI     */
/* Drivers GPIO_setConfig / GPIO_write / GPIO_read + GPIO_setCallback */
/* for edge IRQs.  Until those pads are declared in the board file,  */
/* report NOTIMPL (-> RESP_ERR_NOT_READY) rather than invent a pad    */
/* map -- the protocol path stays honest on silicon. */
int cc3501e_hw_gpio_configure(uint8_t pad, uint8_t dir, uint8_t pull)
{
	(void)pad;
	(void)dir;
	(void)pull;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_gpio_write(uint8_t pad, uint8_t level)
{
	(void)pad;
	(void)level;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_gpio_read(uint8_t pad, uint8_t *level_out)
{
	(void)pad;
	if (level_out != 0) {
		*level_out = 0u;
	}
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_gpio_set_interrupt(uint8_t pad, uint8_t edge, uint8_t enabled)
{
	(void)pad;
	(void)edge;
	(void)enabled;
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_cam_enable(uint8_t which, uint8_t on)
{
	(void)which;
	(void)on;
	return CC3501E_HW_ERR_NOTIMPL;
}

/* --------------------------------------------------------------- */
/* Wi-Fi (v0.2) -- real CC35xx SimpleLink host integration.          */
/*                                                                   */
/* TODO(cc3501e v0.2): route to the CC35xx SimpleLink Wi-Fi host      */
/* (source/ti/drivers/net/wifi/wifi_host_driver -- sl_Start/sl_Wlan* /*/
/* sl_NetApp* / sl_NetCfgGet) once the host-API surface for this SDK  */
/* is wired (the classic CC32xx <ti/drivers/net/wifi/simplelink.h>    */
/* path moved in SDK 10.10; see hal/cc3501e_hw.h + DESIGN.md).  Until  */
/* then report NOTIMPL (-> RESP_ERR_NOT_READY); the v0.1 image stays   */
/* radio-stack-free and links against TI Drivers + driverlib alone. */
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

/* --------------------------------------------------------------- */
/* BLE 5.4 (v0.3) -- real TI BLE host integration.                   */
/*                                                                   */
/* TODO(cc3501e v0.3): route to the TI BLE 5.4 host (NimBLE,          */
/* source/ti/net/ble_interface + source/third_party/nimble) for GAP  */
/* (advertise/scan/connect) + GATT.  Until the BLE host is brought up */
/* report NOTIMPL (-> RESP_ERR_NOT_READY); v0.1 stays radio-free. */
int cc3501e_hw_ble_enable(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_disable(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

int cc3501e_hw_ble_adv_start(uint8_t connectable, uint16_t interval_min_ms,
                             uint16_t interval_max_ms, const uint8_t *adv_data, uint8_t adv_data_len)
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

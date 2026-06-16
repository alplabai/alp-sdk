/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-abstraction shim consumed by firmware/cc3501e/src/.
 * Each function maps a firmware-level operation onto the CC3501E
 * silicon via TI's SimpleLink CC33xx SDK / driverlib.
 *
 * The default implementations in hal/cc3501e_hw_stub.c keep the
 * protocol path exercisable on the host with no TI SDK (every
 * HW-touching op returns CC3501E_HW_ERR_NOTIMPL).  The real
 * implementations live under hal/ti/ (CC3501E_HAL_BACKEND=ti, linked
 * against TI's SimpleLink SDK on the bench build).
 *
 * v0.1 ("bring-up") surface is intentionally tiny: chip init, the idle
 * housekeeping tick, the factory MAC read, and a deferred self-reset.
 * The Wi-Fi / BLE / GPIO-proxy / camera-enable HAL surfaces land in
 * v0.2+ alongside their protocol handlers (see docs/cc3501e-bridge.md
 * "v0.x roadmap").
 */

#ifndef CC3501E_BRIDGE_HAL_CC3501E_HW_H
#define CC3501E_BRIDGE_HAL_CC3501E_HW_H

#include <stdint.h>

/* Return codes.  0 = success; negatives are operation-independent
 * failure classes (mirrors the gd32-bridge BRIDGE_HW_* convention). */
#define CC3501E_HW_OK          0
#define CC3501E_HW_ERR_NOTIMPL -1 /* op not wired on this backend (stub)   */
#define CC3501E_HW_ERR_IO      -2 /* peripheral / radio access failed       */
#define CC3501E_HW_ERR_INVAL   -3 /* bad argument                           */

/* --------------------------------------------------------------- */
/* Lifecycle                                                         */
/* --------------------------------------------------------------- */

/* One-time chip bring-up: clocks, power, and any board-level pin setup
 * the firmware owns before a transport is started.  No-op on the stub. */
void cc3501e_hw_init(void);

/* Periodic housekeeping, run on each WFI wakeup from main().  No-op on
 * this firmware rev; reserved for watchdog kick / deferred work. */
void cc3501e_hw_tick(void);

/* --------------------------------------------------------------- */
/* Meta operations                                                   */
/* --------------------------------------------------------------- */

/* Read the CC3501E's 6-byte factory MAC into @p mac (TI wire order).
 * Returns CC3501E_HW_OK on success, CC3501E_HW_ERR_NOTIMPL on the stub
 * backend (the protocol layer maps that to RESP_ERR_NOT_READY). */
int cc3501e_hw_get_mac(uint8_t mac[6]);

/* Request a soft self-reset.  The implementation DEFERS the actual
 * reboot until the in-flight reply has been clocked back to the host
 * (so the host sees the CMD_RESET ack), then resets the chip.  No-op on
 * the stub backend. */
void cc3501e_hw_request_reset(void);

/* Transport -> HAL: the in-flight reply frame has been FULLY clocked back
 * to the host.  The slave-side SPI/SDIO transport calls this from its
 * reply-complete path.  The deferred-reset latch (cc3501e_hw_request_reset)
 * only fires the reboot once this has been observed for the CMD_RESET ack,
 * so the host always sees the ack before the link goes quiet.  No-op on
 * the stub backend. */
void cc3501e_hw_notify_reply_sent(void);

/* --------------------------------------------------------------- */
/* GPIO proxy (v0.4) + camera enables                                */
/* --------------------------------------------------------------- */

/* The CC3501E fronts E1M pads IO11 / IO13 / IO15..IO21 (plus mux/wake
 * lines) and the two camera-enable LDOs, per
 * metadata/e1m_modules/aen/from-cc3501e.tsv.  These shims drive the
 * proxied CC3501E GPIOs on the Alif's behalf.  @p pad is the CC3501E
 * GPIO index; @p dir / @p pull / @p edge use the alp_cc3501e_gpio_*
 * enums in <alp/protocol/cc3501e.h>.  Return CC3501E_HW_* (NOTIMPL maps
 * to RESP_ERR_NOT_READY at the protocol layer). */
int cc3501e_hw_gpio_configure(uint8_t pad, uint8_t dir, uint8_t pull);
int cc3501e_hw_gpio_write(uint8_t pad, uint8_t level);
int cc3501e_hw_gpio_read(uint8_t pad, uint8_t *level_out);
int cc3501e_hw_gpio_set_interrupt(uint8_t pad, uint8_t edge, uint8_t enabled);

/* Camera-enable LDOs: @p which 0 -> CAM_EN_LDO0 (GPIO_0), 1 -> CAM_EN_LDO1
 * (GPIO_1); @p on != 0 asserts the enable.  Default OFF at boot. */
int cc3501e_hw_cam_enable(uint8_t which, uint8_t on);

/* --------------------------------------------------------------- */
/* Wi-Fi (v0.2)                                                      */
/* --------------------------------------------------------------- */

/* Route to TI's SimpleLink Wi-Fi host (sl_Wlan* / sl_NetApp*) in the ti
 * backend; the stub + the silicon-free host build report NOTIMPL (->
 * RESP_ERR_NOT_READY).  @p security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE
 * (matches alp_cc3501e_wifi_connect_t.security).  ssid/psk are not NUL-
 * terminated; the lengths are authoritative. */
int cc3501e_hw_wifi_scan_start(void);
int cc3501e_hw_wifi_scan_stop(void);
int cc3501e_hw_wifi_connect_sta(const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk,
                                uint8_t psk_len, uint8_t security);
int cc3501e_hw_wifi_disconnect(void);
int cc3501e_hw_wifi_ap_start(const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk,
                             uint8_t psk_len, uint8_t security);
int cc3501e_hw_wifi_ap_stop(void);
int cc3501e_hw_wifi_get_rssi(int8_t *rssi_dbm_out);
int cc3501e_hw_wifi_get_ip(uint8_t ip_out[4]);

/* --------------------------------------------------------------- */
/* BLE 5.4 (v0.3)                                                    */
/* --------------------------------------------------------------- */

/* Route to TI's BLE host (NimBLE, source/ti/net/ble_interface) in the ti
 * backend; the stub + the silicon-free host build report NOTIMPL.  adv/scan
 * intervals are in ms; addr is a 6-byte BLE device address; GATT ops carry
 * a 16-bit attribute handle. */
int cc3501e_hw_ble_enable(void);
int cc3501e_hw_ble_disable(void);
int cc3501e_hw_ble_adv_start(uint8_t connectable, uint16_t interval_min_ms,
                             uint16_t interval_max_ms, const uint8_t *adv_data, uint8_t adv_data_len);
int cc3501e_hw_ble_adv_stop(void);
int cc3501e_hw_ble_scan_start(void);
int cc3501e_hw_ble_scan_stop(void);
int cc3501e_hw_ble_connect(uint8_t addr_type, const uint8_t addr[6]);
int cc3501e_hw_ble_disconnect(void);
int cc3501e_hw_ble_gatt_register(const uint8_t *desc, uint16_t desc_len);
int cc3501e_hw_ble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t data_len);
int cc3501e_hw_ble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len);
int cc3501e_hw_ble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t data_len);

/* --------------------------------------------------------------- */
/* Power policy + diagnostics (configurability)                      */
/* --------------------------------------------------------------- */

/* Apply the host's power-policy hint (alp_cc3501e_power_policy_t fields:
 * coarse policy preset, wake-event bitmap, idle-before-sleep ms).  These
 * are firmware-side config knobs (no radio needed) -- the backend applies
 * what it can and returns OK. */
int cc3501e_hw_set_power_policy(uint8_t policy, uint8_t wake_events, uint32_t idle_ms_before_sleep);

/* Set firmware log verbosity (0 = off).  Firmware-side config -> OK. */
int cc3501e_hw_set_log_level(uint8_t level);

/* Diagnostics sources for GET_DIAG_INFO (best-effort; 0 / UNKNOWN when the
 * backend has no source yet).  reset_cause is an alp_cc3501e_reset_cause_t. */
uint8_t  cc3501e_hw_reset_cause(void);
uint32_t cc3501e_hw_uptime_ms(void);
uint32_t cc3501e_hw_free_heap_bytes(void);

#endif /* CC3501E_BRIDGE_HAL_CC3501E_HW_H */

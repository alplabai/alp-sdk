/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- BLE 5.4 (v0.3, real TI BLE host
 * integration via Apache NimBLE).
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  BLE shares the HIF with Wi-Fi, so cc3501e_hw_ble_enable()
 * calls cc3501e_hw_wifi_lazy_start() (cc3501e_hw_ti_wifi.c) first -- see
 * cc3501e_hw_ti_internal.h for that cross-TU seam.  cc3501e_hw_ti.c keeps
 * platform lifecycle + the deferred-reboot latch.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdint.h>

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* cc3501e_hw_wifi_lazy_start (BLE shares the HIF) */
#include "transport.h"              /* bridge_transport_spi_hw_reinit */

/* --------------------------------------------------------------- */
/* BLE 5.4 (v0.3) -- real TI BLE host integration (Apache NimBLE).   */
/*                                                                   */
/* Under CC3501E_BLE the enable + advertise bodies route to the      */
/* NimBLE host glue (cc3501e_nimble_host.c -> third_party/nimble +   */
/* ti/net/ble_interface).  BLE shares the HIF with Wi-Fi, so the      */
/* enable path lazy-starts Wi-Fi FIRST (cc3501e_hw_wifi_lazy_start)  */
/* exactly as the Wi-Fi bodies do, then re-opens the bridge SPI after */
/* the radio op.  scan/connect/gatt stay NOTIMPL this rev.  The       */
/* non-BLE build (#else) keeps every op NOTIMPL (-> RESP_ERR_NOT_READY)*/
/* so the stub / silicon-free path is unchanged. */
#ifdef CC3501E_BLE
#ifndef CC3501E_WIFI
#error \
    "CC3501E_BLE requires CC3501E_WIFI (shared HIF -> Wlan_Start first); build with -Ble (which implies -WifiHostDriver)."
#endif

/* CC35xx Wi-Fi host API (SDK 10.10 moved off the classic <.../simplelink.h>):
 * Wlan_Set / WlanPowerManagement_e / WLAN_SET_POWER_MANAGEMENT /
 * POWER_MANAGEMENT_ALWAYS_ACTIVE_MODE for the always-active power mode the
 * enable path forces before BleIf_EnableBLE (see cc3501e_hw_ble_enable). */
#include <wlan_if.h>
#include <string.h> /* memcpy (BLE scan record packing) */
/* App-side NimBLE host adapter (cc3501e_nimble_host.c): the BLE bodies below
 * drive advertising/enable through this seam so the raw NimBLE headers stay
 * out of this TU. */
#include "cc3501e_nimble_host.h"
/* BLE_ENABLE: start Wi-Fi first (shared HIF), then bring up the NimBLE host.
 * Reached ONLY from the async worker's drain (never the SPI ISR): both the
 * Wlan_Start lazy-init and nimble_host_start() block for seconds.  Re-opens
 * the bridge SPI after each radio op, exactly as the Wi-Fi bodies do. */
int cc3501e_hw_ble_enable(void)
{
	/* IDEMPOTENT first: if a prior pass already brought the NimBLE host up, do NOT
	 * re-init it -- just re-sync the bridge for the reply and report OK.  The host
	 * poll-by-repeat re-sends BLE_ENABLE on any transient IO, so without this a
	 * second pass would double-init NimBLE (BleIf_EnableBLE + nimble_port_init
	 * twice) and fail.  THIS is the wifi-scan -> ble-enable -4: the scan glitches
	 * the bridge mid-enable so the (successful) first pass's OK never publishes,
	 * the host retries, and the retry double-inits.  A clean boot never retries
	 * (bridge stays synced), so it never hit this. */
	if (cc3501e_nimble_host_is_enabled()) {
		bridge_transport_spi_hw_reinit();
		return CC3501E_HW_OK;
	}

	/* Wi-Fi must be up before BLE (shared HIF).  lazy_start re-opens the
	 * bridge SPI after the Wlan_Start HIF disruption (its own bench-proven
	 * fix); we re-open again after the NimBLE bring-up below. */
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}

	/* Resync the bridge slave BEFORE the heavy BleIf_EnableBLE.  A prior radio op
	 * (e.g. `wifi scan`) leaves the shared HIF / bridge DMA desynced, and the host
	 * poll-by-repeat does NOT self-recover it -- so without this the enable starts
	 * on a dead link and returns IO (the wifi-scan -> ble-enable -4).  On a clean
	 * boot the bridge is already synced, so this reinit is a harmless no-op.  (When
	 * Wi-Fi was just lazy-started above, lazy_start already reinit'd -- a second
	 * reinit is still safe; the cost is one SPI re-open off the SPI ISR.) */
	bridge_transport_spi_hw_reinit();

	/* nimble_host_start = BleIf_OpenTransport + BleIf_EnableBLE + NimBLE host.
	 * BleIf_EnableBLE sends the enable cmd then BLOCKS on an async 0x2A04
	 * (BLE_INIT_DONE) event -- which the WLAN event thread delivers over the SAME
	 * shared host<->NWP HIF.  So we must NOT suspend the bridge across the enable:
	 * suspending the shared transport STARVES the 0x2A04 signal -> the 1s wait
	 * times out every pass -> EnableBLE returns -1 -> the worker stalls (the -4
	 * hang).  No TI BLE demo suspends across the enable -- this file's old "suspend
	 * fixes BLE" note was WRONG (root-caused via the SDK 2026-06-22).  Two things
	 * the TI ble_controller demo does that we were missing:
	 *   (a) Always-Active power mode BEFORE the enable -- Wlan_Start leaves the NWP
	 *       in ELP, in which it may never complete BLE-controller init / post 0x2A04
	 *       (ble_controller_app.c:395 cmdSetPmModeCallback("-m 0")).
	 *   (b) NO bridge suspend across the enable (above).
	 * BleIf_EnableBLE/nimble still perturb the shared DMA like Wlan_Start, so we
	 * recover the bridge slave with reinit AFTER (the host poll-retries on IO). */
	WlanPowerManagement_e pm = POWER_MANAGEMENT_ALWAYS_ACTIVE_MODE; /* = 0 */
	(void)Wlan_Set(WLAN_SET_POWER_MANAGEMENT, (void *)&pm);

	const int rc = cc3501e_nimble_host_start();
	bridge_transport_spi_hw_reinit();
	if (rc != 0 || !cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_IO;
	}
	return CC3501E_HW_OK;
}

/* BLE_DISABLE: tear down advertising + discovery via the NimBLE glue, then
 * re-open the bridge SPI (the teardown issues HCI over the shared HIF, exactly
 * like adv_stop).  Reached ONLY from the async worker's drain (never the SPI
 * ISR).  Best-effort: the NimBLE host stays up so a later BLE_ENABLE is a cheap
 * no-op (see cc3501e_hw_ble_enable's idempotency). */
int cc3501e_hw_ble_disable(void)
{
	const int rc = cc3501e_nimble_host_disable();
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_ADVERTISE (ext-adv): configure + start the single adv set via the NimBLE
 * glue, then re-open the bridge SPI (the adv config/start path issues HCI over
 * the shared HIF).  adv_data_len==0 -> the glue builds a default (device name
 * "ALP-CC3501E" + flags). */
int cc3501e_hw_ble_adv_start(uint8_t        connectable,
                             uint16_t       interval_min_ms,
                             uint16_t       interval_max_ms,
                             const uint8_t *adv_data,
                             uint8_t        adv_data_len)
{
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL; /* BLE not enabled yet -> NOT_READY */
	}
	const int rc = cc3501e_nimble_adv_config_and_start(
	    connectable, interval_min_ms, interval_max_ms, adv_data, adv_data_len);
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

int cc3501e_hw_ble_adv_stop(void)
{
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_adv_stop();
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_SCAN (worker-routed, record-returning -- the BLE mirror of
 * cc3501e_hw_wifi_scan).  Runs a NimBLE GAP discovery for CC3501E_BLE_SCAN_MS,
 * then packs the discovered advertisers onto the reply: each record =
 * addr[6] | addr_type(1) | rssi(int8) | name_len(1) | name[name_len].  The
 * ble_gap_disc churns the shared HIF DMA like adv, so re-open the bridge SPI
 * after (the host poll-retries on IO while the bridge re-syncs). */
#define CC3501E_BLE_SCAN_MS 4000u
int cc3501e_hw_ble_scan(uint8_t *buf, size_t cap, size_t *out_len)
{
	if (buf == 0 || out_len == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	*out_len = 0u;
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL; /* BLE not enabled yet -> NOT_READY */
	}

	static cc3501e_nimble_scan_rec_t recs[24];
	uint32_t                         n  = 0u;
	const int                        rc = cc3501e_nimble_scan(
	    recs, (uint32_t)(sizeof(recs) / sizeof(recs[0])), &n, CC3501E_BLE_SCAN_MS);
	bridge_transport_spi_hw_reinit();
	if (rc != 0) {
		return CC3501E_HW_ERR_IO;
	}

	size_t off = 0u;
	for (uint32_t i = 0u; i < n; i++) {
		const cc3501e_nimble_scan_rec_t *r        = &recs[i];
		uint8_t                          name_len = r->name_len;
		if (name_len > 31u) {
			name_len = 31u;
		}
		const size_t rec = 9u + (size_t)name_len; /* addr[6]+type+rssi+name_len + name */
		if (off + rec > cap) {
			break; /* stop before overflowing the wire buffer */
		}
		memcpy(&buf[off], r->addr, 6u);
		buf[off + 6u] = r->addr_type;
		buf[off + 7u] = (uint8_t)r->rssi;
		buf[off + 8u] = name_len;
		for (uint32_t s = 0u; s < name_len; s++) {
			buf[off + 9u + s] = (uint8_t)r->name[s];
		}
		off += rec;
	}
	*out_len = off;
	return CC3501E_HW_OK;
}

/* BLE_SCAN_STOP: cancel any in-flight GAP discovery (issues HCI over the shared
 * HIF, so re-sync the bridge after).  Worker-routed off the SPI ISR. */
int cc3501e_hw_ble_scan_stop(void)
{
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL; /* BLE not enabled yet -> NOT_READY */
	}
	const int rc = cc3501e_nimble_scan_stop();
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_CONNECT: central-connect to addr_type|addr.  Blocks on the connection-
 * complete HCI event over the shared HIF (worker-routed off the SPI ISR -- see
 * worker.c ALP_CC3501E_CMD_BLE_CONNECT), then re-syncs the bridge SPI. */
int cc3501e_hw_ble_connect(uint8_t addr_type, const uint8_t addr[6])
{
	if (addr == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL; /* BLE not enabled yet -> NOT_READY */
	}
	const int rc = cc3501e_nimble_connect(addr_type, addr);
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_DISCONNECT: terminate the active connection; blocks on the disconnect HCI
 * event over the shared HIF, then re-syncs the bridge.  Idempotent (OK when no
 * link is up). */
int cc3501e_hw_ble_disconnect(void)
{
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_disconnect();
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_GATT_REGISTER: confirm the fixed demo GATT service is live (the opaque
 * descriptor is not parsed this rev -- see cc3501e_nimble_gatt_register).  Pure
 * host-side attribute-table check: no HCI, so no bridge re-sync needed. */
int cc3501e_hw_ble_gatt_register(const uint8_t *desc, uint16_t desc_len)
{
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_gatt_register(desc, desc_len);
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_GATT_NOTIFY: push a notification to the connected peer.  Blocks on HCI
 * over the shared HIF, so re-sync the bridge after (worker-routed). */
int cc3501e_hw_ble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t data_len)
{
	if (data == 0 && data_len != 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_gatt_notify(handle, data, data_len);
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_GATT_READ: GATT-client read of a peer attribute; blocks on the read-
 * response HCI over the shared HIF, packs the value into out, re-syncs the
 * bridge (worker-routed -- the payload+reply path copies out back to the host). */
int cc3501e_hw_ble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len)
{
	if (out == 0 || out_len == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	*out_len = 0u;
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_gatt_read(handle, out, cap, out_len);
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}

/* BLE_GATT_WRITE: GATT-client acknowledged write to a peer attribute; blocks on
 * the write-response HCI over the shared HIF, then re-syncs the bridge. */
int cc3501e_hw_ble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t data_len)
{
	if (data == 0 && data_len != 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (!cc3501e_nimble_host_is_enabled()) {
		return CC3501E_HW_ERR_NOTIMPL;
	}
	const int rc = cc3501e_nimble_gatt_write(handle, data, data_len);
	bridge_transport_spi_hw_reinit();
	return (rc == 0) ? CC3501E_HW_OK : CC3501E_HW_ERR_IO;
}
#else  /* !CC3501E_BLE -- stub / Wi-Fi-only / silicon-free build */
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

int cc3501e_hw_ble_scan(uint8_t *buf, size_t cap, size_t *out_len)
{
	(void)buf;
	(void)cap;
	if (out_len != 0) {
		*out_len = 0u;
	}
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
#endif /* CC3501E_BLE */

/* BLE_SCAN_START: streaming-scan kickoff has no NimBLE mapping this rev (the
 * record-returning cc3501e_hw_ble_scan is the supported scan path) -- NOTIMPL on
 * every build. */
int cc3501e_hw_ble_scan_start(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
}

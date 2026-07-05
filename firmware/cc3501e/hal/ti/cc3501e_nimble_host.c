/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- Apache NimBLE host adapter (v0.3 BLE).
 *
 * Built ONLY for the CC3501E_BLE bench build (implied by -Ble in build_ti.ps1,
 * which also pulls -WifiHostDriver: the BLE controller shares the HIF with
 * Wi-Fi, so Wlan_Start MUST run first -- see WIFI_BLE_INTEGRATION.md).  This
 * TU is the app-side NimBLE glue the cc3501e_hw_ble_* bodies call: it owns the
 * NimBLE host bring-up, the single GAP event callback, a minimal GATT server,
 * and the extended-advertising (BLE_EXT_ADV=1) configure+start path.
 *
 * It is a TRIMMED adaptation of the SDK demo
 *   examples/rtos/LP_EM_CC35X1/demos/network_terminal/nimble_host.c
 * keeping only the load-bearing seams (nimble_host_start / gap_event_cb /
 * gatt_svr_init / ext-adv configure+enable); the demo's console_printf / UART
 * cmd-parser surface (ExtAdvCfg_t, print_*) is dropped -- the bridge is
 * headless and drives advertising from the host protocol, not a serial CLI.
 * The raw NimBLE GAP/GATT API is called directly (no cmd_parser.h types).
 *
 * Symbol grounding (verified against SDK 10.10.01.08 ticlang libs 2026-06-18):
 *  - BleIf_EnableBLE()            : ti/net/ble_interface (ble_interface.a)
 *  - nimble_port_init / _run, ble_transport_ll_init, ble_svc_{gap,gatt,dis}_init,
 *    ble_gatts_count_cfg / _add_svcs, ble_gap_ext_adv_{configure,set_data,start,
 *    remove}, ble_hs_adv_set_fields_mbuf, ble_svc_gap_device_name_set,
 *    ble_hs_is_enabled, ble_hs_id_*, ble_npl_task_init, ble_store_*_init,
 *    os_msys_get_pkthdr  : third_party/nimble (nimble.a)
 *  - osi_SyncObj* / OSI_WAIT_FOR_SECOND : wifi_platform_cc35xx.a via osi_dpl.c
 */

#include <stdint.h>
#include <string.h>

#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/dis/ble_svc_dis.h"

#include "ble_if.h"     /* BleIf_EnableBLE */
#include "osi_kernel.h" /* OsiSyncObj_t / osi_SyncObj* / OSI_WAIT_FOR_SECOND / OSI_OK */

#include "cc3501e_nimble_host.h"

/* ------------------------------------------------------------------ */
/* Tunables (mirror the demo's NimBLE thread + naming).                 */
/* ------------------------------------------------------------------ */
/* OSI priority is INVERTED (0 = highest, 31 = lowest).  Use 8 -- this matches the
 * reference vendor app's CC35XX branch (network_terminal/nimble_host.c is
 * #ifdef CC33XX -> 3, #else -> 8; our build defines -DCC35XX, so 8 is the correct
 * value for this part).  The old value 3 was the CC33XX-only branch: at 3 the
 * NimBLE host task PREEMPTS the prio-7 HIF transport thread and the prio-8 shared
 * WLAN/BLE event + CME threads, which is simply wrong for CC35XX -- the host must
 * sit at or below the servicers it depends on, not above them.
 *
 * NOTE: this is a build-correctness fix, NOT a Wi-Fi+BLE concurrency fix.  Running
 * a Wlan_Scan while BLE is up is still rejected by the closed NWP firmware
 * (FW 1.8.0.42, SoftGemini coexistence arbiter -- TI OSPREY_MX-1518 "degraded scan
 * in coex"); the priority value does not change that.  See docs/cc3501e-production.md
 * "Wi-Fi + BLE concurrency".  Use the two radios one-at-a-time. */
#define CC3501E_NIMBLE_THRD_PRIORITY   (8)
#define CC3501E_NIMBLE_THRD_STACK_SIZE (4096)
#define CC3501E_BLE_ADV_INSTANCE       (0) /* the single ext-adv set we drive */
#define CC3501E_BLE_DEFAULT_NAME       "ALP-CC3501E"

/* ------------------------------------------------------------------ */
/* Module state.                                                        */
/* ------------------------------------------------------------------ */
static struct ble_npl_task s_task_host;
static uint8_t             s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static OsiSyncObj_t        s_host_init_sync; /* signalled by ble_sync_cb */

/* nimble.a brings these in depending on BLE_STORE_CONFIG_PERSIST (syscfg.h
 * default = 1 -> persistent store).  Both symbols are in nimble.a; pick the
 * configured one exactly as the demo does so the store init matches the lib. */
#if MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST)
void ble_store_config_init(void);
#else
void ble_store_ram_init(void);
#endif

/* ------------------------------------------------------------------ */
/* GATT server -- one minimal primary service so the peripheral is not  */
/* attribute-empty (host can still add richer tables via GATT_REGISTER  */
/* in a later rev).  Read/write/notify characteristic, like the demo's  */
/* TI Simple Peripheral but pared to a single characteristic.           */
/* ------------------------------------------------------------------ */
#define CC3501E_SVC_UUID16 0xFFF0u
#define CC3501E_CHR_UUID16 0xFFF1u

static uint16_t s_chr_val_handle;
static uint8_t  s_chr_value = 0x00u;

static int cc3501e_gatt_chr_access(uint16_t                     conn_handle,
                                   uint16_t                     attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt,
                                   void                        *arg)
{
	(void)conn_handle;
	(void)attr_handle;
	(void)arg;
	switch (ctxt->op) {
	case BLE_GATT_ACCESS_OP_READ_CHR: {
		int rc = os_mbuf_append(ctxt->om, &s_chr_value, sizeof(s_chr_value));
		return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	case BLE_GATT_ACCESS_OP_WRITE_CHR: {
		uint16_t len = 0u;
		int      rc  = ble_hs_mbuf_to_flat(ctxt->om, &s_chr_value, sizeof(s_chr_value), &len);
		return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
	}
	default:
		return BLE_ATT_ERR_UNLIKELY;
	}
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
	{
	    .type = BLE_GATT_SVC_TYPE_PRIMARY,
	    .uuid = BLE_UUID16_DECLARE(CC3501E_SVC_UUID16),
	    .characteristics =
	        (struct ble_gatt_chr_def[]){
	            {
	                .uuid       = BLE_UUID16_DECLARE(CC3501E_CHR_UUID16),
	                .access_cb  = cc3501e_gatt_chr_access,
	                .val_handle = &s_chr_val_handle,
	                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
	            },
	            { 0 }, /* no more characteristics */
	        },
	},
	{ 0 }, /* no more services */
};

static int cc3501e_gatt_svr_init(void)
{
	int rc = ble_gatts_count_cfg(s_gatt_svcs);
	if (rc != 0) {
		return rc;
	}
	return ble_gatts_add_svcs(s_gatt_svcs);
}

/* ------------------------------------------------------------------ */
/* GAP event callback -- single sink for the adv set + any connection.  */
/* Headless: no console output; the bridge surfaces events to the host  */
/* via async protocol frames in a later rev (see DESIGN.md).            */
/* ------------------------------------------------------------------ */
static int cc3501e_gap_event_cb(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	switch (event->type) {
	case BLE_GAP_EVENT_ADV_COMPLETE:
		/* A finite-duration adv set completed; re-arm so the device keeps
		 * advertising (matches the demo's adv_enable() re-trigger). */
		(void)ble_gap_ext_adv_start(CC3501E_BLE_ADV_INSTANCE, 0, 0);
		break;
	case BLE_GAP_EVENT_CONNECT:
	case BLE_GAP_EVENT_DISCONNECT:
	default:
		break;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* NimBLE host task + sync rendezvous (host<->controller).             */
/* ------------------------------------------------------------------ */
static void cc3501e_ble_sync_cb(void)
{
	/* Ensure we have an identity address, infer its type, then release the
	 * waiter in cc3501e_nimble_host_start().  Mirrors the demo's ble_sync_cb. */
	(void)ble_hs_util_ensure_addr(0);
	(void)ble_hs_id_infer_auto(0, &s_own_addr_type);
	osi_SyncObjSignal(&s_host_init_sync);
}

static void cc3501e_ble_reset_cb(int reason)
{
	(void)reason; /* controller reset -- headless, nothing to log */
}

static void cc3501e_nimble_host_task(void *param)
{
	(void)param;
	ble_hs_cfg.reset_cb = cc3501e_ble_reset_cb;
	ble_hs_cfg.sync_cb  = cc3501e_ble_sync_cb;

	ble_svc_gap_device_name_set(CC3501E_BLE_DEFAULT_NAME);

	nimble_port_run(); /* blocks: services the host event queue forever */
}

/* ------------------------------------------------------------------ */
/* Public seam (cc3501e_nimble_host.h).                                 */
/* ------------------------------------------------------------------ */

int cc3501e_nimble_host_start(void)
{
	int rc;

	/* Open the BLE HIF transport BEFORE enabling the controller.  THE missing
	 * step (root cause of the BLE_ENABLE hang): the TI demo does
	 * BleIf_OpenTransport() then nimble_host_start (network_terminal
	 * ble_cmd.c:671 cmdBleStartCallback); without it the NWP has no open BLE
	 * transport to command-complete on, so BleIf_EnableBLE() never returns ->
	 * the worker stalls and the host times out (-4).  Mirrors the WiFi
	 * WLAN_SET_STA_WIFI_BAND fix: a TI setup call we were skipping. */
	BleIf_OpenTransport();

	/* Enable the NWP BLE controller over the (already-up) shared HIF.  Gated
	 * by the device conf_bin EnableBle -- times out if BLE is OFF in the conf
	 * (see the conf_bin regen note in WIFI_BLE_INTEGRATION.md).
	 *
	 * RETRY the enable: BleIf_EnableBLE blocks only 1 s (ble_if.c osi_SyncObjWait,
	 * OSI_WAIT_FOR_SECOND) for the async 0x2A04 (BLE_INIT_DONE) event.  Right
	 * after a Wi-Fi scan the NWP is busy and that first 1 s wait can expire even
	 * though the controller comes up fine -- the wifi-scan -> ble-enable -4.  Each
	 * call re-creates its own bleInitEventSyncObj + re-sends the enable cmd
	 * (ble_if.c:194/211/230), so a retry is self-contained; loop until the
	 * init-done lands.  This is BEFORE nimble_port_init so the one-time host init
	 * below still runs exactly once. */
	for (int attempt = 0; attempt < 8; attempt++) {
		rc = BleIf_EnableBLE();
		if (rc == OSI_OK) {
			break;
		}
	}
	if (rc != OSI_OK) {
		return rc;
	}

	if (osi_SyncObjCreate(&s_host_init_sync) != OSI_OK) {
		return -1;
	}

	nimble_port_init();
	ble_transport_ll_init();

	ble_svc_gap_init();
	ble_svc_gatt_init();
	ble_svc_dis_init();

	rc = cc3501e_gatt_svr_init();
	if (rc != 0) {
		return rc;
	}

#if MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST)
	ble_store_config_init();
#else
	ble_store_ram_init();
#endif

	rc = ble_npl_task_init(&s_task_host,
	                       "nimble_host",
	                       cc3501e_nimble_host_task,
	                       NULL,
	                       CC3501E_NIMBLE_THRD_PRIORITY,
	                       BLE_NPL_TIME_FOREVER,
	                       NULL,
	                       CC3501E_NIMBLE_THRD_STACK_SIZE);
	if (rc != OSI_OK) {
		return rc;
	}

	/* Block ~1 s for host<->controller sync (the demo waits OSI_WAIT_FOR_SECOND). */
	rc = osi_SyncObjWait(&s_host_init_sync, OSI_WAIT_FOR_SECOND);
	(void)osi_SyncObjDelete(&s_host_init_sync);
	if (rc != OSI_OK) {
		return -1; /* never synced -> not enabled */
	}
	return ble_hs_is_enabled() ? 0 : -1;
}

int cc3501e_nimble_host_is_enabled(void)
{
	return ble_hs_is_enabled();
}

int cc3501e_nimble_adv_config_and_start(uint8_t        connectable,
                                        uint16_t       interval_min_ms,
                                        uint16_t       interval_max_ms,
                                        const uint8_t *adv_data,
                                        uint8_t        adv_data_len)
{
	struct ble_gap_ext_adv_params params;
	struct os_mbuf               *adv_mbuf;
	ble_addr_t                    addr;
	int                           rc;

	if (!ble_hs_is_enabled()) {
		return -1;
	}

	/* --- Extended-advertising parameters (BLE_EXT_ADV=1). --- */
	memset(&params, 0, sizeof(params));
	params.connectable   = connectable ? 1u : 0u;
	params.scannable     = 0u; /* non-scannable ext-adv */
	params.legacy_pdu    = 0u;
	params.own_addr_type = s_own_addr_type;
	params.primary_phy   = BLE_HCI_LE_PHY_1M;
	params.secondary_phy = BLE_HCI_LE_PHY_1M;
	params.tx_power      = 127; /* "use the controller's default" */
	params.sid           = 0u;
	params.itvl_min      = BLE_GAP_ADV_ITVL_MS(interval_min_ms ? interval_min_ms : 100u);
	params.itvl_max      = BLE_GAP_ADV_ITVL_MS(interval_max_ms ? interval_max_ms : 100u);
	params.channel_map   = 0x07u; /* all 3 primary channels */

	/* Remove any stale set on this instance, then (re)configure it. */
	rc = ble_gap_ext_adv_remove(CC3501E_BLE_ADV_INSTANCE);
	if ((rc != 0) && (rc != BLE_HS_EALREADY)) {
		return rc;
	}
	rc = ble_gap_ext_adv_configure(
	    CC3501E_BLE_ADV_INSTANCE, &params, NULL, cc3501e_gap_event_cb, NULL);
	if (rc != 0) {
		return rc;
	}

	/* A RANDOM identity needs the address pushed to the adv set explicitly. */
	if (s_own_addr_type != BLE_OWN_ADDR_PUBLIC) {
		addr.type = BLE_ADDR_RANDOM;
		rc        = ble_hs_id_copy_addr(addr.type, addr.val, NULL);
		if (rc == 0) {
			rc = ble_gap_ext_adv_set_addr(CC3501E_BLE_ADV_INSTANCE, &addr);
		}
		if (rc != 0) {
			return rc;
		}
	}

	/* --- Advertising data. --- */
	adv_mbuf = os_msys_get_pkthdr(BLE_HCI_MAX_ADV_DATA_LEN, 0);
	if (adv_mbuf == NULL) {
		return -1;
	}

	if (adv_data_len == 0u) {
		/* Sane default: flags + the device name (recipe-specified fallback). */
		struct ble_hs_adv_fields fields;
		const char              *name = ble_svc_gap_device_name();
		memset(&fields, 0, sizeof(fields));
		fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
		fields.name             = (uint8_t *)name;
		fields.name_len         = (uint8_t)strlen(name);
		fields.name_is_complete = 1u;
		rc                      = ble_hs_adv_set_fields_mbuf(&fields, adv_mbuf);
	} else {
		/* Host-supplied raw AD bytes -> straight into the adv payload. */
		rc = os_mbuf_append(adv_mbuf, adv_data, adv_data_len);
	}
	if (rc != 0) {
		os_mbuf_free_chain(adv_mbuf);
		return rc;
	}

	rc = ble_gap_ext_adv_set_data(CC3501E_BLE_ADV_INSTANCE, adv_mbuf);
	if (rc != 0) {
		os_mbuf_free_chain(adv_mbuf);
		return rc;
	}

	/* duration=0 (forever), max_events=0 (unbounded). */
	return ble_gap_ext_adv_start(CC3501E_BLE_ADV_INSTANCE, 0, 0);
}

int cc3501e_nimble_adv_stop(void)
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	return ble_gap_ext_adv_stop(CC3501E_BLE_ADV_INSTANCE);
}

int cc3501e_nimble_host_disable(void)
{
	/* Nothing to tear down if the host never came up -- report success so a
	 * BLE_DISABLE on an already-off stack is idempotent. */
	if (!ble_hs_is_enabled()) {
		return 0;
	}
	/* Stop the two GAP roles that hold the shared HIF: the ext-adv set and any
	 * in-flight discovery.  Both are idempotent (EALREADY when not active), so
	 * their return codes are swallowed -- disable is best-effort teardown. */
	(void)ble_gap_ext_adv_stop(CC3501E_BLE_ADV_INSTANCE);
	(void)ble_gap_disc_cancel();
	return 0;
}

/* ------------------------------------------------------------------ */
/* GAP discovery (BLE scan) -- collect advertisers + their names.       */
/*                                                                      */
/* ble_gap_disc runs for a fixed window; each BLE_GAP_EVENT_DISC report  */
/* is folded into a per-address cache (dedup, keep the latest RSSI +    */
/* first non-empty name).  DISC_COMPLETE signals the waiter.  Headless: */
/* the cache is drained by cc3501e_nimble_scan() into the caller's      */
/* records, which cc3501e_hw_ble_scan() then packs onto the wire.       */
/* ------------------------------------------------------------------ */
#define CC3501E_BLE_SCAN_CACHE_MAX 24u

static cc3501e_nimble_scan_rec_t s_scan_cache[CC3501E_BLE_SCAN_CACHE_MAX];
static volatile uint32_t         s_scan_count;
static OsiSyncObj_t              s_scan_done;

/* Locate a cached advertiser by address (dedup); -1 if not yet seen. */
static int cc3501e_scan_cache_find(const uint8_t addr[6])
{
	for (uint32_t i = 0u; i < s_scan_count; i++) {
		if (memcmp(s_scan_cache[i].addr, addr, 6) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int cc3501e_scan_gap_event_cb(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	switch (event->type) {
	case BLE_GAP_EVENT_DISC: {
		int idx = cc3501e_scan_cache_find(event->disc.addr.val);
		if (idx < 0) {
			if (s_scan_count >= CC3501E_BLE_SCAN_CACHE_MAX) {
				break; /* cache full -- ignore further new advertisers */
			}
			idx = (int)s_scan_count++;
			memcpy(s_scan_cache[idx].addr, event->disc.addr.val, 6);
			s_scan_cache[idx].addr_type = event->disc.addr.type;
			s_scan_cache[idx].name_len  = 0u;
			s_scan_cache[idx].name[0]   = '\0';
		}
		/* Latest RSSI wins (a scan-response often arrives stronger/closer). */
		s_scan_cache[idx].rssi = event->disc.rssi;
		/* Parse a device name out of the AD payload once -- the ADV_IND may
		 * carry none and the scan-response (active scan) the complete name. */
		if (s_scan_cache[idx].name_len == 0u) {
			struct ble_hs_adv_fields fields;
			if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0 &&
			    fields.name != NULL && fields.name_len > 0u) {
				uint8_t n = fields.name_len;
				if (n > sizeof(s_scan_cache[idx].name) - 1u) {
					n = (uint8_t)(sizeof(s_scan_cache[idx].name) - 1u);
				}
				memcpy(s_scan_cache[idx].name, fields.name, n);
				s_scan_cache[idx].name[n]  = '\0';
				s_scan_cache[idx].name_len = n;
			}
		}
		break;
	}
	case BLE_GAP_EVENT_DISC_COMPLETE:
		osi_SyncObjSignal(&s_scan_done);
		break;
	default:
		break;
	}
	return 0;
}

int cc3501e_nimble_scan(cc3501e_nimble_scan_rec_t *out,
                        uint32_t                   cap,
                        uint32_t                  *out_count,
                        uint32_t                   duration_ms)
{
	if (out_count != NULL) {
		*out_count = 0u;
	}
	if (!ble_hs_is_enabled()) {
		return -1; /* controller/host not up -- caller maps to NOT_READY */
	}

	s_scan_count = 0u;
	if (osi_SyncObjCreate(&s_scan_done) != OSI_OK) {
		return -1;
	}

	struct ble_gap_disc_params dp;
	memset(&dp, 0, sizeof(dp));
	dp.passive           = 0u; /* active scan: request scan-response (names) */
	dp.filter_duplicates = 0u; /* dedup host-side + refresh RSSI per report */
	dp.itvl              = 0u; /* 0 -> controller default interval/window */
	dp.window            = 0u;
	dp.limited           = 0u;

	int rc = ble_gap_disc(s_own_addr_type, duration_ms, &dp, cc3501e_scan_gap_event_cb, NULL);
	if (rc != 0) {
		(void)osi_SyncObjDelete(&s_scan_done);
		return rc;
	}

	/* Wait for DISC_COMPLETE: the scan window plus a margin.  osi_SyncObjWait
	 * blocks in 1-second ticks, so poll it (duration + 2) times and stop early
	 * once the completion event signals. */
	uint32_t wait_s = (duration_ms / 1000u) + 2u;
	for (uint32_t i = 0u; i < wait_s; i++) {
		if (osi_SyncObjWait(&s_scan_done, OSI_WAIT_FOR_SECOND) == OSI_OK) {
			break;
		}
	}
	(void)ble_gap_disc_cancel(); /* ensure the scan is stopped (idempotent) */
	(void)osi_SyncObjDelete(&s_scan_done);

	uint32_t n = s_scan_count;
	if (n > cap) {
		n = cap;
	}
	for (uint32_t i = 0u; i < n; i++) {
		out[i] = s_scan_cache[i];
	}
	if (out_count != NULL) {
		*out_count = n;
	}
	return 0;
}

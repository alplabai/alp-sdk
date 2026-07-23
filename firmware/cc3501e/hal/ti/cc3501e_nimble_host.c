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
 *    ble_gatts_count_cfg / _add_svcs / _reset / _start, ble_gap_ext_adv_{configure,
 *    set_data,start,remove}, ble_hs_adv_set_fields_mbuf, ble_svc_gap_device_name_set,
 *    ble_hs_is_enabled, ble_hs_id_*, ble_npl_task_init, ble_store_*_init,
 *    os_msys_get_pkthdr  : third_party/nimble (nimble.a)
 *  - osi_SyncObj* / OSI_WAIT_FOR_SECOND : wifi_platform_cc35xx.a via osi_dpl.c
 */

#include <stdint.h>
#include <string.h>

#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/ble.h" /* BLE_ERR_REM_USER_CONN_TERM (nimble/ble.h:216) */
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

/* ------------------------------------------------------------------ */
/* Connection + GATT-client rendezvous state (worker-thread ownership).  */
/*                                                                      */
/* One active link at a time (v0.3): s_conn_handle mirrors the single    */
/* GAP connection -- set on BLE_GAP_EVENT_CONNECT (either role: our      */
/* central connect OR a central connecting to our advertiser) and        */
/* cleared on BLE_GAP_EVENT_DISCONNECT.  The connect/disconnect/GATT-     */
/* client bodies run on the bridge worker task (never the SPI ISR), so    */
/* they may block on these OSI sync objects while the NimBLE host task     */
/* delivers the matching GAP/GATT completion callback and signals them.    */
/* The sync objects are created once (lazy) and never deleted so an async  */
/* peer-initiated DISCONNECT can always signal s_disc_sync.               */
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int               s_syncs_ready;
static OsiSyncObj_t      s_conn_sync;     /* signalled on BLE_GAP_EVENT_CONNECT    */
static OsiSyncObj_t      s_disc_sync;     /* signalled on BLE_GAP_EVENT_DISCONNECT */
static OsiSyncObj_t      s_gattc_op_sync; /* signalled on a GATT read/write completion */
static volatile int      s_conn_status;   /* status field of the CONNECT event    */
static volatile int      s_gattc_op_status;
static uint8_t          *s_gattc_read_buf; /* caller buffer for the pending read   */
static uint16_t          s_gattc_read_cap;
static volatile uint16_t s_gattc_read_len;

/* Create the connect/disconnect/GATT sync objects exactly once.  Called from the
 * worker before it initiates a blocking GAP/GATT op; idempotent. */
static int cc3501e_ensure_syncs(void)
{
	if (s_syncs_ready) {
		return 0;
	}
	if (osi_SyncObjCreate(&s_conn_sync) != OSI_OK) {
		return -1;
	}
	if (osi_SyncObjCreate(&s_disc_sync) != OSI_OK) {
		return -1;
	}
	if (osi_SyncObjCreate(&s_gattc_op_sync) != OSI_OK) {
		return -1;
	}
	s_syncs_ready = 1;
	return 0;
}

/* Poll an OSI sync object in 1-second ticks up to @max_seconds (osi_SyncObjWait
 * blocks in whole-second units on this port -- see the scan path).  0 -> signalled,
 * -1 -> timed out. */
static int cc3501e_wait_signaled(OsiSyncObj_t *sync, uint32_t max_seconds)
{
	for (uint32_t i = 0u; i < max_seconds; i++) {
		if (osi_SyncObjWait(sync, OSI_WAIT_FOR_SECOND) == OSI_OK) {
			return 0;
		}
	}
	return -1;
}

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
/* attribute-empty even before any GATT_REGISTER call.  Read/write/notify */
/* characteristic, like the demo's TI Simple Peripheral but pared to a   */
/* single characteristic.  cc3501e_nimble_gatt_register() (below) ADDS a */
/* second, caller-defined service alongside this one -- it does not     */
/* replace it.                                                          */
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
/* GATT server -- dynamic service (BLE_GATT_REGISTER, 0x38).            */
/*                                                                      */
/* Static storage: ble_gatts_add_svcs() does not copy the svc/chr/uuid   */
/* tables it is given -- the ATT server keeps referencing them for the   */
/* service's whole lifetime -- so a stack-local table would dangle the   */
/* moment cc3501e_nimble_gatt_register() returned.  v1 supports exactly  */
/* ONE dynamic service (one register call); a second call is rejected    */
/* rather than silently leaking/replacing the first -- multi-service      */
/* stacking is a follow-up if a customer needs it.                       */
/* ------------------------------------------------------------------ */

/* HAL-side storage cap, independent of the wire-format cap
 * (ALP_CC3501E_BLE_GATT_MAX_CHARS in <alp/protocol/cc3501e.h>) -- this TU
 * stays wire-protocol-agnostic (see cc3501e_hw.h), so the two are not
 * #include-linked; cc3501e_hw_ble_gatt_register's handles_cap argument is
 * the actual cross-check (worker.c sizes it from the wire header). Must be
 * >= the wire cap or a legal descriptor could exceed this table. */
#define CC3501E_GATT_DYN_MAX_CHARS 8u

/* Per-characteristic value storage cap.  A GATT_REGISTER initial_value
 * longer than this is rejected outright (not silently truncated) -- 64
 * bytes comfortably covers a small telemetry/config value at the default
 * ATT MTU (23 bytes) and several MTU-negotiated steps beyond it. */
#define CC3501E_GATT_DYN_CHR_VAL_MAX 64u

typedef struct {
	uint16_t len;
	uint8_t  value[CC3501E_GATT_DYN_CHR_VAL_MAX];
} cc3501e_gatt_dyn_val_t;

static int                     s_dyn_svc_registered;
static ble_uuid128_t           s_dyn_svc_uuid;
static ble_uuid128_t           s_dyn_chr_uuid[CC3501E_GATT_DYN_MAX_CHARS];
static cc3501e_gatt_dyn_val_t  s_dyn_chr_val[CC3501E_GATT_DYN_MAX_CHARS];
static uint16_t                s_dyn_chr_val_handle[CC3501E_GATT_DYN_MAX_CHARS];
static struct ble_gatt_chr_def s_dyn_chrs[CC3501E_GATT_DYN_MAX_CHARS + 1u]; /* +1 terminator */
static struct ble_gatt_svc_def s_dyn_svcs[2];                               /* +1 terminator */

/* Single access callback shared by every dynamic characteristic; @p arg is
 * the per-characteristic cc3501e_gatt_dyn_val_t* (set as chr_def.arg below),
 * same read/write shape as the fixed demo's cc3501e_gatt_chr_access. */
static int cc3501e_gatt_dyn_chr_access(uint16_t                     conn_handle,
                                       uint16_t                     attr_handle,
                                       struct ble_gatt_access_ctxt *ctxt,
                                       void                        *arg)
{
	(void)conn_handle;
	(void)attr_handle;
	cc3501e_gatt_dyn_val_t *v = (cc3501e_gatt_dyn_val_t *)arg;

	switch (ctxt->op) {
	case BLE_GATT_ACCESS_OP_READ_CHR: {
		int rc = os_mbuf_append(ctxt->om, v->value, v->len);
		return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	case BLE_GATT_ACCESS_OP_WRITE_CHR: {
		uint16_t len = 0u;
		int      rc  = ble_hs_mbuf_to_flat(ctxt->om, v->value, CC3501E_GATT_DYN_CHR_VAL_MAX, &len);
		if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
		v->len = len;
		return 0;
	}
	default:
		return BLE_ATT_ERR_UNLIKELY;
	}
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
		/* A central connected to our advertiser (peripheral role): latch the
		 * handle so GATT notify/read/write target it.  (status != 0 => the
		 * attempt failed; leave the handle cleared.) */
		if (event->connect.status == 0) {
			s_conn_handle = event->connect.conn_handle;
		}
		break;
	case BLE_GAP_EVENT_DISCONNECT:
		s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		if (s_syncs_ready) {
			osi_SyncObjSignal(&s_disc_sync);
		}
		break;
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

/* ------------------------------------------------------------------ */
/* GAP central connect + connection lifecycle.                          */
/*                                                                      */
/* cc3501e_nimble_connect() issues ble_gap_connect() and blocks (bounded) */
/* on the BLE_GAP_EVENT_CONNECT delivered to cc3501e_conn_gap_event_cb.   */
/* Once connected, that callback becomes the connection's event sink and  */
/* also fields the eventual BLE_GAP_EVENT_DISCONNECT.                     */
/* ------------------------------------------------------------------ */
#define CC3501E_BLE_CONNECT_TIMEOUT_MS 8000

/* Single event sink for a central-initiated connection (ble_gap.h:2116 says a
 * successful connection inherits the connect callback for later events, incl.
 * DISCONNECT). */
static int cc3501e_conn_gap_event_cb(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT: /* ble_gap.h:169; event.connect{status,conn_handle} */
		s_conn_status = event->connect.status;
		if (event->connect.status == 0) {
			s_conn_handle = event->connect.conn_handle;
		} else {
			s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		}
		osi_SyncObjSignal(&s_conn_sync);
		break;
	case BLE_GAP_EVENT_DISCONNECT: /* ble_gap.h:172; event.disconnect.reason */
		s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		osi_SyncObjSignal(&s_disc_sync);
		break;
	default:
		break;
	}
	return 0;
}

int cc3501e_nimble_connect(uint8_t addr_type, const uint8_t addr[6])
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
		return -1; /* one active link at a time (v0.3) */
	}
	if (cc3501e_ensure_syncs() != 0) {
		return -1;
	}

	/* ble.h:295 ble_addr_t {type,val[6]}: the peer to connect to. */
	ble_addr_t peer;
	peer.type = addr_type;
	memcpy(peer.val, addr, 6);

	s_conn_status = -1;
	(void)osi_SyncObjClear(&s_conn_sync); /* drop any stale signal */

	/* ble_gap.h:2133 ble_gap_connect(own_addr_type, peer, duration_ms, params,
	 * cb, cb_arg); NULL params => stack defaults. */
	int rc = ble_gap_connect(s_own_addr_type,
	                         &peer,
	                         CC3501E_BLE_CONNECT_TIMEOUT_MS,
	                         NULL,
	                         cc3501e_conn_gap_event_cb,
	                         NULL);
	if (rc != 0) {
		return rc;
	}

	/* Wait for the CONNECT event (timeout budget + a margin). */
	if (cc3501e_wait_signaled(&s_conn_sync, (CC3501E_BLE_CONNECT_TIMEOUT_MS / 1000u) + 2u) != 0) {
		(void)ble_gap_conn_cancel(); /* ble_gap.h:2204 -- abort the in-flight attempt */
		return -1;
	}
	if (s_conn_status != 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
		return -1; /* controller reported a failed connection */
	}
	return 0;
}

int cc3501e_nimble_disconnect(void)
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	uint16_t h = s_conn_handle;
	if (h == BLE_HS_CONN_HANDLE_NONE) {
		return 0; /* already disconnected -- idempotent */
	}
	if (cc3501e_ensure_syncs() != 0) {
		return -1;
	}
	(void)osi_SyncObjClear(&s_disc_sync);

	/* ble_gap.h:2227 ble_gap_terminate(conn_handle, hci_reason); the standard
	 * local-initiated reason is REMOTE USER TERMINATED (ble.h:216 = 0x13). */
	int rc = ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
	if (rc != 0) {
		/* Already gone (EALREADY/ENOTCONN): reflect the cleared state, report OK. */
		s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		return 0;
	}
	if (cc3501e_wait_signaled(&s_disc_sync, 4u) != 0) {
		return -1;
	}
	return 0;
}

int cc3501e_nimble_scan_stop(void)
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	/* ble_gap.h:2086 ble_gap_disc_cancel(); BLE_HS_EALREADY when no scan is
	 * running -> idempotent success (matches the disable teardown path). */
	int rc = ble_gap_disc_cancel();
	return (rc == 0 || rc == BLE_HS_EALREADY) ? 0 : rc;
}

/* ------------------------------------------------------------------ */
/* GATT client (against the connected peer).                            */
/*                                                                      */
/* ble_gattc_read / ble_gattc_write_flat both complete asynchronously via */
/* a ble_gatt_attr_fn callback (ble_gatt.h:275): error->status carries the */
/* result and, for a read, attr->om the value mbuf (host frees it after    */
/* the callback -- we copy it out flat).                                  */
/* ------------------------------------------------------------------ */
static int cc3501e_gattc_read_cb(uint16_t                     conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr        *attr,
                                 void                        *arg)
{
	(void)conn_handle;
	(void)arg;
	s_gattc_op_status = (error != NULL) ? (int)error->status : 0;
	if (s_gattc_op_status == 0 && attr != NULL && attr->om != NULL) {
		uint16_t n  = 0u;
		int      rc = ble_hs_mbuf_to_flat(attr->om, s_gattc_read_buf, s_gattc_read_cap, &n);
		if (rc != 0) {
			s_gattc_op_status = rc;
		} else {
			s_gattc_read_len = n;
		}
	}
	osi_SyncObjSignal(&s_gattc_op_sync);
	return 0;
}

static int cc3501e_gattc_write_cb(uint16_t                     conn_handle,
                                  const struct ble_gatt_error *error,
                                  struct ble_gatt_attr        *attr,
                                  void                        *arg)
{
	(void)conn_handle;
	(void)attr;
	(void)arg;
	s_gattc_op_status = (error != NULL) ? (int)error->status : 0;
	osi_SyncObjSignal(&s_gattc_op_sync);
	return 0;
}

int cc3501e_nimble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len)
{
	if (out_len != NULL) {
		*out_len = 0u;
	}
	if (!ble_hs_is_enabled() || out == NULL) {
		return -1;
	}
	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
		return -1; /* no peer connected */
	}
	if (cc3501e_ensure_syncs() != 0) {
		return -1;
	}
	(void)osi_SyncObjClear(&s_gattc_op_sync);
	s_gattc_op_status = -1;
	s_gattc_read_buf  = out;
	s_gattc_read_cap  = cap;
	s_gattc_read_len  = 0u;

	/* ble_gatt.h:452 ble_gattc_read(conn_handle, attr_handle, cb, cb_arg). */
	int rc = ble_gattc_read(s_conn_handle, handle, cc3501e_gattc_read_cb, NULL);
	if (rc != 0) {
		return rc;
	}
	if (cc3501e_wait_signaled(&s_gattc_op_sync, 4u) != 0) {
		return -1;
	}
	if (s_gattc_op_status != 0) {
		return -1;
	}
	if (out_len != NULL) {
		*out_len = s_gattc_read_len;
	}
	return 0;
}

int cc3501e_nimble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
		return -1;
	}
	if (cc3501e_ensure_syncs() != 0) {
		return -1;
	}
	(void)osi_SyncObjClear(&s_gattc_op_sync);
	s_gattc_op_status = -1;

	/* ble_gatt.h:597 ble_gattc_write_flat(conn_handle, attr_handle, data,
	 * data_len, cb, cb_arg) -- acknowledged write, completes via the callback. */
	int rc = ble_gattc_write_flat(s_conn_handle, handle, data, len, cc3501e_gattc_write_cb, NULL);
	if (rc != 0) {
		return rc;
	}
	if (cc3501e_wait_signaled(&s_gattc_op_sync, 4u) != 0) {
		return -1;
	}
	return (s_gattc_op_status == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* GATT server -- dynamic registration (BLE_GATT_REGISTER, 0x38).       */
/*                                                                      */
/* Descriptor layout: version(1) | service_uuid(16) | num_chars(1) then, */
/* per characteristic: char_uuid(16) | properties(1) | initial_len(LE16) */
/* | initial_value[initial_len] -- see the wire-format doc block in      */
/* <alp/protocol/cc3501e.h>.  protocol_ble.c has already length/shape-    */
/* validated @p desc (version, num_chars bound, every record fits          */
/* exactly) before this runs off the worker drain, so this parse trusts    */
/* the shape and only re-checks the two HAL-local storage caps            */
/* (CC3501E_GATT_DYN_MAX_CHARS, CC3501E_GATT_DYN_CHR_VAL_MAX) that the      */
/* wire layer does not know about.                                        */
/*                                                                        */
/* NimBLE lifecycle -- exactly ONE ble_gatts_start() against a live server: */
/*                                                                          */
/* The host's automatic first ble_gatts_start() (BLE_HS_AUTO_START, fired   */
/* by nimble_port_run() before cc3501e_nimble_host_start() returns) has      */
/* ALREADY registered the fixed demo service and populated ble_att_svr_list   */
/* with its ble_att_svr_entry nodes.  A naive second ble_gatts_start() is a    */
/* USE-AFTER-FREE: ble_gatts_start() -> ble_att_svr_start() (ble_att_svr.c)     */
/* unconditionally ble_att_svr_free_start_mem()s the attribute-entry pool and   */
/* re-mallocs it, but ble_att_svr_list is untouched by that path (only            */
/* ble_att_svr_init() -- called once, at ble_hs init, never again --              */
/* STAILQ_INITs it) -- so the demo's entries dangle into freed heap and every      */
/* subsequent STAILQ_FOREACH walk over ble_att_svr_list (ATT read/write/MTU/        */
/* discovery) touches freed memory.                                                 */
/*                                                                                    */
/* Fix: ble_gatts_reset() (ble_gatts.c) first.  It calls ble_att_svr_reset()           */
/* (ble_att_svr.c), which STAILQ_REMOVE_HEADs every entry off ble_att_svr_list          */
/* and ble_att_svr_hidden_list and frees each one back into the pool -- this             */
/* happens BEFORE ble_att_svr_start()'s free+remalloc, so the list is properly            */
/* emptied while the pool it points into is still live; nothing dangles.  Both             */
/* calls share ble_gatts_mutable()'s guard (no active connection/adv/discover/              */
/* connect) -- if that is not met they return BLE_HS_EBUSY rather than touch                 */
/* anything, so a register attempt during an active link errors, it does not                 */
/* corrupt.  After the reset we re-queue BOTH service tables -- the demo's                     */
/* s_gatt_svcs (its ble_gatts_svc_defs entry was freed by the first                              */
/* ble_gatts_start()'s ble_gatts_free_svc_defs(), but s_gatt_svcs itself is a                      */
/* static const table, still valid to re-add) and our new s_dyn_svcs -- then                       */
/* call ble_gatts_start() exactly once.  ble_gatts_count_cfg() is NOT re-run for                     */
/* the demo table: its doc comment (ble_gatts.c) says counts accumulate without                       */
/* being cleared first, and ble_hs_max_attrs/services/client_configs already                           */
/* carry the demo's counts from cc3501e_gatt_svr_init()'s one-time call -- re-                           */
/* counting it here would double it.  ble_att_svr_id resets to 0 in                                       */
/* ble_att_svr_reset(), and ble_gatts_register_svcs() (run inside                                          */
/* ble_gatts_start()) walks ble_gatts_svc_defs in order, so re-registering the                              */
/* demo table FIRST reproduces its original attribute handles (s_chr_val_handle                              */
/* comes out identical) before the dynamic table's handles are assigned after                                  */
/* it -- the demo characteristic (0xFFF0/0xFFF1) keeps working post-register.                                   */
int cc3501e_nimble_gatt_register(const uint8_t *desc,
                                 uint16_t       desc_len,
                                 uint16_t      *handles_out,
                                 uint16_t       handles_cap,
                                 uint16_t      *num_handles_out)
{
	if (num_handles_out != 0) {
		*num_handles_out = 0u;
	}
	if (desc == 0 || handles_out == 0 || num_handles_out == 0) {
		return -1;
	}
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	if (s_dyn_svc_registered) {
		return -1; /* v1: one dynamic service per boot -- see the block comment above */
	}

	/* Header: version(1) | service_uuid(16) | num_chars(1) -- shape already
	 * validated by protocol_ble.c; desc_len is re-checked here anyway as an
	 * independent bounds guard, so this HAL cannot over-read desc even if a
	 * future caller ever reaches it without going through that validator. */
	if (desc_len < 18u) {
		return -1;
	}
	const uint8_t num_chars = desc[17];
	if (num_chars == 0u || num_chars > CC3501E_GATT_DYN_MAX_CHARS) {
		return -1; /* exceeds THIS HAL's storage, even if the wire cap allowed it */
	}

	memcpy(s_dyn_svc_uuid.value, &desc[1], 16u); /* verbatim -- alp_ble_uuid_t.b is
	                                              * already little-endian, matching
	                                              * ble_uuid128_t.value's order
	                                              * (host/ble_uuid.h BLE_UUID128_INIT
	                                              * doc: "little-endian order"). */
	s_dyn_svc_uuid.u.type = BLE_UUID_TYPE_128;

	size_t pos = 18u; /* BLE_GATT_REG_HDR in protocol_ble.c */
	for (uint8_t i = 0u; i < num_chars; i++) {
		if (pos + 16u + 1u + 2u > desc_len) {
			return -1; /* record header would run past desc_len */
		}
		memcpy(s_dyn_chr_uuid[i].value, &desc[pos], 16u);
		s_dyn_chr_uuid[i].u.type   = BLE_UUID_TYPE_128;
		const uint8_t  properties  = desc[pos + 16u];
		const uint16_t initial_len = (uint16_t)desc[pos + 17u] | ((uint16_t)desc[pos + 18u] << 8);
		pos += 16u + 1u + 2u;

		if (initial_len > CC3501E_GATT_DYN_CHR_VAL_MAX) {
			return -1; /* HAL storage cap, not a wire-format limit */
		}
		if (pos + initial_len > desc_len) {
			return -1; /* initial_value would run past desc_len */
		}
		memcpy(s_dyn_chr_val[i].value, &desc[pos], initial_len);
		s_dyn_chr_val[i].len = initial_len;
		pos += initial_len;

		s_dyn_chr_val_handle[i]    = 0u;
		s_dyn_chrs[i].uuid         = &s_dyn_chr_uuid[i].u;
		s_dyn_chrs[i].access_cb    = cc3501e_gatt_dyn_chr_access;
		s_dyn_chrs[i].arg          = &s_dyn_chr_val[i];
		s_dyn_chrs[i].descriptors  = NULL;
		s_dyn_chrs[i].flags        = (ble_gatt_chr_flags)properties; /* ALP_BLE_GATT_PROP_* ==
		                                                              * BLE_GATT_CHR_F_* bits,
		                                                              * verified against
		                                                              * host/ble_gatt.h */
		s_dyn_chrs[i].min_key_size = 0u;
		s_dyn_chrs[i].val_handle   = &s_dyn_chr_val_handle[i];
	}
	memset(&s_dyn_chrs[num_chars], 0, sizeof(s_dyn_chrs[num_chars])); /* terminator */

	s_dyn_svcs[0].type            = BLE_GATT_SVC_TYPE_PRIMARY;
	s_dyn_svcs[0].uuid            = &s_dyn_svc_uuid.u;
	s_dyn_svcs[0].includes        = NULL;
	s_dyn_svcs[0].characteristics = s_dyn_chrs;
	memset(&s_dyn_svcs[1], 0, sizeof(s_dyn_svcs[1])); /* terminator */

	/* ble_gatts_reset() empties ble_att_svr_list/hidden_list into the still-
	 * live pool (see the block comment above) -- fails BLE_HS_EBUSY, not
	 * corruption, if a connection/adv/discover/connect is active. */
	int rc = ble_gatts_reset();
	if (rc != 0) {
		return rc;
	}
	/* Re-queue the demo table first (no re-count -- see block comment),
	 * then the new dynamic table, then start exactly once. */
	rc = ble_gatts_add_svcs(s_gatt_svcs);
	if (rc != 0) {
		return rc;
	}
	rc = ble_gatts_count_cfg(s_dyn_svcs);
	if (rc != 0) {
		return rc;
	}
	rc = ble_gatts_add_svcs(s_dyn_svcs);
	if (rc != 0) {
		return rc;
	}
	rc = ble_gatts_start();
	if (rc != 0) {
		return rc;
	}

	const uint16_t n = (handles_cap < num_chars) ? handles_cap : num_chars;
	for (uint16_t i = 0u; i < n; i++) {
		handles_out[i] = s_dyn_chr_val_handle[i];
	}
	*num_handles_out     = n;
	s_dyn_svc_registered = 1;
	return 0;
}

int cc3501e_nimble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
	if (!ble_hs_is_enabled()) {
		return -1;
	}
	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
		return -1; /* nobody to notify */
	}
	/* handle==0 -> notify the fixed demo characteristic value handle. */
	uint16_t att = (handle != 0u) ? handle : s_chr_val_handle;

	/* ble_hs_mbuf.h:57 ble_hs_mbuf_from_flat(buf,len) builds the value mbuf;
	 * ble_gatts_notify_custom (ble_gatt.h:658) consumes/frees it. */
	struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
	if (om == NULL) {
		return -1;
	}
	return ble_gatts_notify_custom(s_conn_handle, att, om);
}

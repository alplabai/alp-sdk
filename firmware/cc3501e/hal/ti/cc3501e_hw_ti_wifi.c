/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- Wi-Fi (v0.2, real CC35xx SimpleLink
 * host integration) + the factory-MAC read + the boot-time radio/lwIP
 * bring-up (radio<->SPI coexistence fix; see cc3501e_hw.h).
 *
 * Split by hardware subsystem out of cc3501e_hw_ti.c (issue #703, #461
 * Phase B).  cc3501e_hw_wifi_lazy_start() below is shared with
 * cc3501e_hw_ti_ble.c (BLE shares the HIF, so BLE_ENABLE lazy-starts Wi-Fi
 * first) -- see cc3501e_hw_ti_internal.h for that cross-TU seam.
 * cc3501e_hw_ti.c keeps platform lifecycle + the deferred-reboot latch.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (the bench build), against TI's
 * SimpleLink CC35xx SDK.  CI builds the stub backend instead, so this file
 * is never on the SDK-free path.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h> /* memcpy (scan-result cache / AP SSID staging) */

/* Async-event ring (src/event_ring.h, on the firmware CMake include path): the
 * Wi-Fi connect/disconnect path pushes EVT_WIFI_* here for the host to drain via
 * CMD_GET_PENDING_EVENTS.  Silicon-free; always linked. */
#include "event_ring.h"

#ifdef CC3501E_WIFI
/* CC35xx Wi-Fi host API (SDK 10.10 moved off the classic <.../simplelink.h>):
 * Wlan_Get / WlanMacAddress_t / WLAN_GET_MACADDRESS / WLAN_ROLE_STA / Wlan_Start /
 * Wlan_RoleUp / RoleUpStaCmd_t.  Impl in wifi_stack.a (pulls the OSI port ->
 * osi_dpl.c); the Wlan_* refs here prove the P0-5 link. */
#include <wlan_if.h>
/* OSI sync-obj rendezvous (osi_SyncObj* / OSI_WAIT_FOREVER): the worker thread
 * Clear()s + waits, the Wi-Fi event cb (host-driver thread) Signal()s.  Impl in
 * wifi_platform_cc35xx.a via osi_dpl.c.  See WIFI_BLE_INTEGRATION.md rendezvous. */
#include <osi_kernel.h>
/* lwIP bring-up app-source (compiled from the demo into this image -- see
 * build_ti.ps1 $WifiHostDriver $sources): network_set_up / network_stack_add_if_ap
 * / network_stack_get_if_ip for the DHCP + AP + GET_IP bodies.  WIFI_GET_IP has
 * NO async event (per the integration recipe) -- it polls lwIP for a non-zero ip. */
#include <network_lwip.h>
/* The demo's network_lwip.c references the app control block app_CB (status bits
 * + the connect/dhcp OSI sync objs) which the demo defines in network_terminal.c
 * -- a TU we do NOT link (it carries the console main()).  Provide app_CB here,
 * zero-initialised: network_lwip.c only reads app_CB.Status and guards every
 * app_CB.CON_CB.*SyncObj use against NULL, so a zeroed block is safe (those
 * optional signals are simply skipped on this headless bridge). */
#include <network_terminal.h>
appControlBlock app_CB;
/* uptime source for cc3501e_hw_wifi_connect_sta's bounded DHCP-lease poll
 * (ClockP_usleep between tries so the tcpip thread runs DHCP). */
#include <ti/drivers/dpl/ClockP.h>
#endif

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h" /* cc3501e_hw_wifi_lazy_start (shared with cc3501e_hw_ti_ble.c) */
#include "transport.h" /* bridge_transport_spi_hw_reinit/suspend, cc3501e_bridge_busy/ready */

#ifdef CC3501E_WIFI
/* --------------------------------------------------------------- *
 * Lazy Wi-Fi bring-up (P0-6) -- the CC35xx host stack is started ONCE,    *
 * on first use, from the async worker's drain (NOT the SPI ISR).  Per     *
 * WIFI_BLE_INTEGRATION.md "Init order": Wlan_Start(eventCB) ->            *
 * Wlan_RoleUp(WLAN_ROLE_STA, ...).  Both block (RoleUp can take seconds), *
 * which is exactly why this is gated behind the worker.                   *
 * --------------------------------------------------------------- */

/* One-time guard: the stack is started exactly once for the process
 * lifetime; subsequent GET_MAC / Wi-Fi ops skip straight to the op. */
static bool wifi_started;

/* One-time STA role-up guard.  Wlan_Start (lazy_start) is enough for the factory
 * MAC read, but SCAN and CONNECT need the STA ROLE up.  RoleUp is brought up once
 * (bounded timeout, NOT WLAN_WAIT_FOREVER -- a stuck role-up must never hang the
 * worker / the bridge SPI re-open).  Shared by scan + connect so RoleUp runs once. */
static bool wifi_sta_role_up;

/* ---- Worker <-> Wi-Fi-event rendezvous (WIFI_BLE_INTEGRATION.md) ----------
 * Async Wlan_* ops (Scan / Connect) complete on the host-driver thread via the
 * event cb, NOT on the worker thread that issued them.  The worker Clear()s the
 * sync obj, issues the op, then Wait()s; the cb copies the result into the cache
 * below and Signal()s.  Created once at first lazy-start (osi services are up by
 * then).  A single rendezvous is sufficient: the worker is single-in-flight
 * (one Wi-Fi op at a time), matching src/worker.c's single-job seam.
 *
 * Scope note (architecture, not a TODO): src/worker.c routes only argument-free
 * getters one at a time, and the WIFI protocol handlers in src/protocol.c are
 * direct synchronous calls (NOT poll-by-repeat like GET_MAC) -- the native_sim
 * suite locks single-frame NOT_READY for SCAN/CONNECT.  These bodies therefore
 * block in their caller's context; on silicon the seconds-long ones must be
 * reached from the worker drain (as GET_MAC is).  Wiring SCAN/CONNECT through
 * the worker needs an arg-carrying multi-job worker + poll-by-repeat handlers
 * (a wire/seam redesign) -- out of scope here (see RETURN notes). */
static OsiSyncObj_t wifi_event_sync;  /* signalled by the event cb        */
static volatile int wifi_last_status; /* WLAN event Status (<0 = fail)    */

/* Scan-result cache: the whole AP list arrives in ONE WLAN_EVENT_SCAN_RESULT.
 * The cb snapshots it here; a SCAN body copies/packs it after the wait. */
#define WIFI_SCAN_CACHE_MAX WLAN_MAX_SCAN_COUNT
static volatile uint32_t  wifi_scan_count;
static WlanNetworkEntry_t wifi_scan_cache[WIFI_SCAN_CACHE_MAX];

/* Wi-Fi event callback -- runs on the host-driver task (NOT the SPI ISR, NOT
 * the worker).  Snapshots the result the issuing op is waiting on, then signals
 * the rendezvous.  GET_MAC takes no event (blocking get) so it is not handled
 * here.  osi_SyncObjSignal (not *FromISR): the cb is thread context. */
/* DIAG: count EVERY Wi-Fi event the host-driver task delivers + record the last
 * Id, so a scan that never returns SCAN_RESULT can be split: NWP fired NO event
 * at all (RX/scan-engine stall -> antenna/HW) vs fired OTHER events (cb gap). */
static volatile uint32_t wifi_cb_event_count;
static volatile uint32_t wifi_cb_last_id;

static void wifi_event_cb(WlanEvent_t *event)
{
	if (event == NULL) {
		return;
	}
	wifi_cb_event_count++;
	wifi_cb_last_id = (uint32_t)event->Id;
	switch (event->Id) {
	case WLAN_EVENT_SCAN_RESULT: {
		uint32_t n = event->Data.ScanResult.NetworkListResultLen;
		if (n > WIFI_SCAN_CACHE_MAX) {
			n = WIFI_SCAN_CACHE_MAX;
		}
		for (uint32_t i = 0; i < n; i++) {
			wifi_scan_cache[i] = event->Data.ScanResult.NetworkListResult[i];
		}
		wifi_scan_count  = n;
		wifi_last_status = 0;
		osi_SyncObjSignal(&wifi_event_sync);
		break;
	}
	case WLAN_EVENT_EXTENDED_SCAN_RESULT: {
		/* The NWP delivers scan results in the EXTENDED format -- a pointer to a
		 * separate list -- not WLAN_EVENT_SCAN_RESULT.  Its entries share the
		 * same leading fields as the normal WlanNetworkEntry_t, so copy those
		 * into the same cache the wire-packer reads.  WITHOUT this case the cb
		 * dropped the only event the scan fired and the host saw -4 (proven by
		 * the event-count diag + the TI SDK two-event model). */
		WlanEventExtendedScanResult_t *ext = event->Data.pExtendedScanResult;
		const int ext_null = (ext == NULL || ext->NetworkListResult == NULL) ? 1 : 0;
		uint32_t  n        = ext_null ? 0u : ext->NetworkListResultLen;
		if (n > WIFI_SCAN_CACHE_MAX) {
			n = WIFI_SCAN_CACHE_MAX;
		}
		for (uint32_t i = 0; i < n; i++) {
			const WlanNetworkEntryExtended_t *e = &ext->NetworkListResult[i];
			memcpy(wifi_scan_cache[i].Ssid, e->Ssid, WLAN_SSID_MAX_LENGTH);
			memcpy(wifi_scan_cache[i].Bssid, e->Bssid, WLAN_BSSID_LENGTH);
			wifi_scan_cache[i].SsidLen      = e->SsidLen;
			wifi_scan_cache[i].Rssi         = e->Rssi;
			wifi_scan_cache[i].SecurityInfo = e->SecurityInfo;
			wifi_scan_cache[i].Channel      = e->Channel;
		}
		wifi_scan_count = n;
		wifi_last_status =
		    ext_null ? -77 : 0; /* DIAG: -77 = EXTENDED event had a NULL data pointer */
		osi_SyncObjSignal(&wifi_event_sync);
		break;
	}
	case WLAN_EVENT_CONNECT:
		wifi_last_status = (int)event->Data.Connect.Status;
		osi_SyncObjSignal(&wifi_event_sync);
		break;
	case WLAN_EVENT_DISCONNECT:
	case WLAN_EVENT_ASSOCIATION_REJECTED:
	case WLAN_EVENT_AUTHENTICATION_REJECTED:
		/* A connect attempt that the FW rejects arrives as one of these
		 * rather than CONNECT(Status<0); surface it as a failure. */
		wifi_last_status = -1;
		osi_SyncObjSignal(&wifi_event_sync);
		break;
	default:
		break; /* CONNECTING / peer / P2P / etc. -- not awaited here */
	}
}

/* DIAG: last Wi-Fi event Id the cb saw (any type) + the running count, surfaced
 * via GET_DIAG_INFO so the host can identify which event the scan actually fires
 * when neither SCAN_RESULT nor EXTENDED_SCAN_RESULT signals. */
uint32_t cc3501e_hw_wifi_last_event_id(void)
{
	return wifi_cb_last_id;
}

/* Defined below; boot_start pre-caches the STA role through it. */
static int cc3501e_hw_wifi_ensure_sta_role(void);

/* Bring the CC35xx Wi-Fi host up to STA role once.  Returns CC3501E_HW_OK
 * when the stack is up (already-started is OK), CC3501E_HW_ERR_IO on a
 * start/role failure.  Called from the worker drain before the first
 * Wlan_* data op.  Non-static (declared in cc3501e_hw_ti_internal.h):
 * cc3501e_hw_ti_ble.c's BLE_ENABLE also calls it (BLE shares the HIF, so
 * Wlan_Start must run first). */
int cc3501e_hw_wifi_lazy_start(void)
{
	if (wifi_started) {
		return CC3501E_HW_OK;
	}
	/* Create the event rendezvous before Wlan_Start so the cb can signal it. */
	if (osi_SyncObjCreate(&wifi_event_sync) != OSI_OK) {
		return CC3501E_HW_ERR_IO;
	}
	/* Wlan_Start internally runs InitHostDriver -> HIF/NWP bring-up + FW
	 * download (wlan_if.c) and reads the factory MAC into the CME cache; it is
	 * the ONLY radio init GET_MAC needs.  Do NOT Wlan_RoleUp(STA) here: RoleUp is
	 * for the connect/scan/AP roles, NOT for the factory-MAC read.
	 *
	 * === bridge-SPI recovery (SHIP-CRITICAL, proven on silicon) ===
	 * Wlan_Start's HIFInit re-points the shared host-DMA + the NWP IRQ to bring up
	 * the host<->NWP link, which DISRUPTS the bridge SPI slave (the host then reads
	 * 0x00000000 from a dead link).  This happens as a SIDE EFFECT of HIFInit
	 * REGARDLESS of whether Wlan_Start ultimately succeeds, so the bridge MUST be
	 * re-opened after the attempt EVEN ON FAILURE -- bench-proven: with reinit only
	 * on the success path, a failing/slow Wlan_Start left the SPI dead forever
	 * (ping_ok stuck 0, reqhdr_rx=0x00000000).  bridge_transport_spi_hw_reinit()
	 * does the real SPI_close + SPI_open + re-arm (transport owns the handle). */
	const int start_rv = Wlan_Start(wifi_event_cb);
	bridge_transport_spi_hw_reinit();
	if (start_rv != 0) {
		return CC3501E_HW_ERR_IO; /* radio not up; link is back, GET_MAC reports IO */
	}

	wifi_started = true;
	return CC3501E_HW_OK;
}

/* Boot-time radio bring-up (radio<->SPI coexistence fix; see cc3501e_hw.h).
 * Runs the SAME one-time lazy-start, but EARLY -- the bring-up task calls this
 * once after the SPI poll task is up and before it services any host command,
 * so the seconds-long Wlan_Start/RoleUp (during which the bridge is down) runs
 * with no host traffic to disrupt.  The wifi_started one-time guard inside
 * makes this idempotent: a later GET_MAC sees wifi_started==true and skips
 * straight to the short Wlan_Get.  cc3501e_hw_wifi_lazy_start() already re-syncs
 * the bridge slave (bridge_transport_spi_hw_reinit) after the radio is up, so the
 * link is re-armed at a clean header boundary before the first host command. */
void cc3501e_hw_wifi_boot_start(void)
{
	cc3501e_bridge_busy(); /* configure GPIO17 + hold the host off through the boot radio init */
	(void)cc3501e_hw_wifi_lazy_start();
	/* Bring the STA role up ONCE here at boot, before the host polls.  RoleUp is a
	 * HEAVY radio op that needs the bridge quiesced (suspend) to kick; doing it in
	 * the scan hot path made the scan's bridge-down window ~30s and churned the
	 * link (the suspend's SPI_close/open) past re-sync.  Pre-cached here, each later scan is just a LIGHT
	 * Wlan_Scan (role already up) that kicks with NO suspend -- like GET_MAC's
	 * Wlan_Get.  Suspend for the RoleUp, then reinit the bridge clean. */
	bridge_transport_spi_hw_suspend();
	(void)cc3501e_hw_wifi_ensure_sta_role();
	bridge_transport_spi_hw_reinit();
	cc3501e_bridge_ready(); /* boot radio init done, slave armed -> host may clock */
}

/* Bring up the lwIP TCP/IP core ONCE, EARLY -- called from bringup_task BEFORE
 * transport_spi_init() spawns the busy-poll bridge slave task AND before the radio
 * is lazy-started.  WHY this exact spot (root-caused on silicon 2026-06-23): tcpip_init
 * spawns the lwIP tcpip thread then sys_sem_wait()s for it to start.  Run from the
 * worker drain AFTER Wlan_Start, that wait HUNG the worker -- the bridge slave poll
 * task busy-waits the RX FIFO at priority 8 (one below bringup), which starves the
 * lwIP thread (TCPIP_THREAD_PRIO clamps to <=9) so it never signals, and the radio
 * had already consumed the FreeRTOS heap the 16 KB tcpip stack needs.  Calling it
 * here -- before that poll task exists and before any radio allocation -- lets the
 * tcpip thread start cleanly.  Once the core mutex exists, the later
 * network_stack_add_if_sta() (ensure_sta_role) is a synchronous LOCK_TCPIP_CORE +
 * netif_add (no thread wait), so it is unaffected by the busy-poll thereafter. */
void cc3501e_hw_net_init(void)
{
	network_stack_init();
	/* Register the STA netif HERE at boot too -- before transport_spi_init() spawns
	 * the busy-poll bridge task.  network_stack_add_if_sta() does LOCK_TCPIP_CORE +
	 * netif_add; done LATER from the radio path (ensure_sta_role) it DEADLOCKED the
	 * worker -- the busy-poll task (prio 8, one below bringup) starves the lwIP tcpip
	 * thread, which holds/needs the core lock, so LOCK_TCPIP_CORE never returns
	 * (silicon 2026-06-23: scan -4 / READY stuck low after adding the netif there).
	 * At boot, before that poll task exists, the tcpip thread runs and netif_add
	 * completes.  The registration is persistent (a static netif); the later
	 * Wlan_RoleUp(STA) binds it.  The STA netif's linkoutput (the WLAN tx) is what the
	 * connect EAPOL-4way / WPA3-SAE handshake flows over -- without it the NWP raises
	 * no connect event (the no-connect-event root cause). */
	network_stack_add_if_sta();
}
#else  /* !CC3501E_WIFI -- default v0.1 ti build brings up no radio */
void cc3501e_hw_wifi_boot_start(void)
{
	/* No radio linked in this build -- nothing to bring up. */
}
void cc3501e_hw_net_init(void)
{
	/* No lwIP linked in this build -- nothing to bring up. */
}
uint32_t cc3501e_hw_wifi_last_event_id(void)
{
	/* No radio linked in this build -- no Wi-Fi events to report.  protocol.c's
	 * GET_DIAG_INFO reads this unconditionally, so the non-Wi-Fi ti build must
	 * define it too (matches cc3501e_hw_stub.c). */
	return 0u;
}
#endif /* CC3501E_WIFI */

/* Read the CC3501E's 6-byte factory MAC (see ../cc3501e_hw.h).  Reached ONLY
 * from the async worker's drain, like the rest of this file's Wlan_* ops. */
int cc3501e_hw_get_mac(uint8_t mac[6])
{
	if (mac == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
#ifdef CC3501E_WIFI
	/* Real factory-MAC read via the CC35xx Wi-Fi host API.  WLAN_GET_MACADDRESS
	 * is a blocking get (no async event).  This is reached ONLY from the
	 * async worker's drain (worker_run_pending, on bringup_task), never from
	 * the SPI ISR (P0-4/P0-6): the ISR submits the job and polls the cached
	 * result, so the seconds-long lazy Wi-Fi init + Wlan_Get below run off
	 * the ISR.  cc3501e_hw_wifi_lazy_start() brings the stack up once (per
	 * WIFI_BLE_INTEGRATION.md init order) before the first Wlan_* op. */
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	WlanMacAddress_t p = { .roleType = WLAN_ROLE_STA };
	if (Wlan_Get(WLAN_GET_MACADDRESS, &p) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)p.pMacAddress[i];
	}
	return CC3501E_HW_OK;
#else
	/* v0.1 brings up no network processor, so the factory MAC is not
	 * readable yet -- report NOTIMPL, which the protocol layer maps to
	 * RESP_ERR_NOT_READY.  The real read lands under CC3501E_WIFI (above). */
	(void)mac;
	return CC3501E_HW_ERR_NOTIMPL;
#endif
}

/* ---- async-connect status latch (CMD_WIFI_STATUS) ------------------------- *
 * The worker-routed connect body BLOCKS for seconds on the association event, so
 * the host no longer polls it to completion (which clocked the bridge while the
 * radio op held it down -- the -4/desync wall).  Instead the firmware mirrors the
 * outcome into this latch, which the NON-blocking CMD_WIFI_STATUS reads off the
 * SPI ISR (no radio op).  Defined unconditionally (no SDK needed) so it links in
 * BOTH ti sub-builds; the connect body (CC3501E_WIFI) is the only terminal writer.
 *
 * Concurrency: written by mark_connecting() in the SPI-ISR/protocol context at
 * SUBMIT, then by the connect body on the drain thread at completion -- never both
 * at once (the worker is single-in-flight).  Read by handle_wifi_status (SPI ISR).
 * volatile byte fields; state is published LAST so a reader seeing CONNECTED also
 * sees the fresh rssi (release-style, mirrors the worker's publish discipline). */
static volatile struct {
	uint8_t state;       /* alp_cc3501e_wifi_conn_state_t */
	uint8_t fail_reason; /* alp_cc3501e_wifi_fail_t        */
	int8_t  rssi;        /* dBm, valid on CONNECTED        */
} g_wifi_conn = { (uint8_t)ALP_CC3501E_WIFI_DISCONNECTED, (uint8_t)ALP_CC3501E_WIFI_FAIL_NONE, 0 };

void cc3501e_hw_wifi_mark_connecting(void)
{
	g_wifi_conn.fail_reason = (uint8_t)ALP_CC3501E_WIFI_FAIL_NONE;
	g_wifi_conn.rssi        = 0;
	g_wifi_conn.state       = (uint8_t)ALP_CC3501E_WIFI_CONNECTING; /* publish state last */
}

int cc3501e_hw_wifi_conn_status(uint8_t *state, uint8_t *fail_reason, int8_t *rssi_dbm)
{
	if (state != 0) *state = g_wifi_conn.state;
	if (fail_reason != 0) *fail_reason = g_wifi_conn.fail_reason;
	if (rssi_dbm != 0) *rssi_dbm = g_wifi_conn.rssi;
	return CC3501E_HW_OK;
}

/* --------------------------------------------------------------- */
/* Wi-Fi (v0.2) -- real CC35xx SimpleLink host integration.          */
/*                                                                   */
/* Under CC3501E_WIFI these route to the CC35xx Wi-Fi host (Wlan_* /  */
/* network_*), each lazy-starting the STA stack first and using the   */
/* osi_SyncObj rendezvous for the async (Scan / Connect) completions. */
/* The stub / silicon-free host build (#else) keeps every op NOTIMPL  */
/* (-> RESP_ERR_NOT_READY) so the native_sim suite stays unchanged.   */
/*                                                                   */
/* Map the host wire security byte (alp_cc3501e_wifi_connect_t.security:*/
/* 0=open, 1=WPA2-PSK, 2=WPA3-SAE) onto the SDK WLAN_SEC_TYPE_* enum. */
#ifdef CC3501E_WIFI
static char cc3501e_wifi_sec(uint8_t security)
{
	switch (security) {
	case 0u:
		return (char)WLAN_SEC_TYPE_OPEN;
	case 2u:
		/* WPA2_WPA3(16) = transition mode (WPA2 / WPA2+PMF / WPA3), NOT WPA3-only(12).
		 * Our scan labels both a pure-WPA3 and a WPA2/WPA3-transition AP as "wpa3", so
		 * the transition type is the robust choice -- it associates to either. */
		return (char)WLAN_SEC_TYPE_WPA2_WPA3;
	case 1u:
	default:
		return (char)WLAN_SEC_TYPE_WPA_WPA2; /* WPA/WPA2-PSK */
	}
}

/* Bring the STA role up once (after Wlan_Start), with a bounded timeout.  Needed
 * by SCAN + CONNECT (not GET_MAC).  Wlan_RoleUp returns the role id (>=0) on
 * success, <0 on error/timeout. */
static int cc3501e_hw_wifi_ensure_sta_role(void)
{
	if (wifi_sta_role_up) {
		return CC3501E_HW_OK;
	}
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	/* Program the STA PHY band BEFORE RoleUp.  THE missing step (root cause of the
	 * 0-AP scan): WLAN_SET_STA_WIFI_BAND is the ONLY call that runs
	 * l2_StorePhyConfig() to set the band the radio RX uses + inits the scan-DB
	 * wifi_band_cfg (static init leaves it 0 -> the survey captured 0 frames).
	 * BOTH TI references do this on every STA role-up (network_terminal
	 * wlan_cmd.c:676, at_commands atcmd_wlan.c:805); ours omitted it. */
	uint8_t sta_wifi_band = (uint8_t)
	    BAND_SEL_ONLY_2_4GHZ; /* our antenna/AP are 2.4 GHz; BOTH made the kick fail (5G) */
	(void)Wlan_Set(WLAN_SET_STA_WIFI_BAND, &sta_wifi_band);
	/* The STA netif is registered ONCE at boot (cc3501e_hw_net_init ->
	 * network_stack_add_if_sta) -- it must NOT be done here: from this radio-path
	 * context the busy-poll bridge task starves the lwIP tcpip thread and
	 * LOCK_TCPIP_CORE deadlocks the worker.  Wlan_RoleUp(STA) binds the already-
	 * registered netif, which is what the connect EAPOL/SAE handshake flows over. */
	RoleUpStaCmd_t staParams = { 0 };
	if (Wlan_RoleUp(WLAN_ROLE_STA, &staParams, 10000u) < 0) {
		return CC3501E_HW_ERR_IO;
	}
	wifi_sta_role_up = true;
	return CC3501E_HW_OK;
}

/* Shared scan core: lazy-start the radio, kick a STA scan of both bands, then
 * wait the single WLAN_EVENT_SCAN_RESULT (whole list in one event) so the
 * result list is cached (wifi_scan_cache[] / wifi_scan_count) for the caller
 * to read/pack.  WLAN_MAX_SCAN_COUNT(20) records cached; MAX_PAYLOAD=512 caps
 * the eventual wire pack to ~17 records (noted upstream).  Returns CC3501E_HW_*. */
static int cc3501e_hw_wifi_scan_run(void)
{
	/* Wlan_Scan runs after just Wlan_Start (no RoleUp) -- the scan completes + the
	 * WLAN_EVENT_SCAN_RESULT fires either way; adding a 10s RoleUp here only
	 * blocked the worker + disrupted the bridge (scan timed out, bench-proven). */
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	/* The STA role is pre-cached at boot (cc3501e_hw_wifi_boot_start), so this
	 * ensure_sta_role returns INSTANTLY with no RoleUp radio op.  The scan is then a
	 * LIGHT Wlan_Scan -- like GET_MAC's Wlan_Get it kicks WITHOUT a bridge suspend.
	 * No suspend == no SPI_close/open churn (the suspend's close/open is what wedged
	 * the link past re-sync; GET_MAC, reinit-only, never churns).  reinit
	 * AFTER recovers the slave's DMA that Wlan_Scan tore down. */
	const int role_rv = cc3501e_hw_wifi_ensure_sta_role();

	scanCommon_t sc = { 0 };
	sc.Band         = BAND_SEL_ONLY_2_4GHZ; /* 2.4 GHz antenna; matches the role-up band-set */
	wifi_scan_count = 0u;
	osi_SyncObjClear(&wifi_event_sync);
	const uint32_t cb_before = wifi_cb_event_count; /* DIAG: did ANY wifi event fire? */
	int            scan_rv   = -1;
	if (role_rv == CC3501E_HW_OK) {
		scan_rv = Wlan_Scan(WLAN_ROLE_STA, &sc, (unsigned char)WLAN_MAX_SCAN_COUNT);
	}
	int wait_rv = OSI_OK;
	if (scan_rv == 0) {
		/* 6s (was 20s) so RoleUp(<=10s)+wait fits the host's 30s poll budget --
		 * the host then reads the REAL final code instead of a -4 timeout mask. */
		wait_rv = osi_SyncObjWait(&wifi_event_sync, 6u * OSI_WAIT_FOR_SECOND);
	}
	bridge_transport_spi_hw_reinit(); /* recover the bridge slave after the radio ops */

	if (role_rv != CC3501E_HW_OK) {
		return CC3501E_HW_ERR_INVAL; /* STA RoleUp failed -> host ALP_ERR_INVAL (-1) */
	}
	if (scan_rv != 0) {
		return CC3501E_HW_ERR_INVAL; /* Wlan_Scan kick failed -> host ALP_ERR_INVAL (-1) */
	}
	if (wait_rv != OSI_OK) {
		/* No WLAN_EVENT_SCAN_RESULT signalled.  DIAG split via the cb event count:
		 *   NO event at all   -> NOTIMPL -> host ALP_ERR_NOT_READY (-2): the NWP/scan
		 *                        engine produced nothing (scan stall / RF).
		 *   other events only -> IO      -> host ALP_ERR_IO (-5): events fired but the
		 *                        cb never saw SCAN_RESULT (a cb/event-routing gap). */
		if (wifi_cb_event_count == cb_before) {
			return CC3501E_HW_ERR_NOTIMPL;
		}
		return CC3501E_HW_ERR_IO;
	}
	return CC3501E_HW_OK;
}

/* WIFI_SCAN_START: run a scan + cache the result list.  Kept for the
 * scan-then-stop control surface; cc3501e_hw_wifi_scan() (below) runs the
 * SAME core and additionally packs the cached list onto the wire. */
int cc3501e_hw_wifi_scan_start(void)
{
	return cc3501e_hw_wifi_scan_run();
}

/* cc3501e_hw_wifi_scan: run the scan (shared core), then PACK the cached AP
 * list into @p buf in the host's wire format -- per record:
 *   bssid[6] | rssi(1) | channel(1) | security(1) | ssid_len(1) then ssid[ssid_len]
 * (the cc3501e_wifi_scan parser's CC3501E_SCAN_REC_HDR=10 layout).  Security and
 * channel are passed through RAW (no translation); rssi is the SDK's signed beacon
 * RSSI.  Records are packed until @p cap would be exceeded; *out_len = total bytes. */
int cc3501e_hw_wifi_scan(uint8_t *buf, size_t cap, size_t *out_len)
{
	if (buf == 0 || out_len == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	*out_len = 0u;

	const int rv = cc3501e_hw_wifi_scan_run();
	if (rv != CC3501E_HW_OK) {
		return rv;
	}

	const uint32_t count = wifi_scan_count;
	size_t         off   = 0u;
	for (uint32_t i = 0u; i < count; i++) {
		const WlanNetworkEntry_t *e        = &wifi_scan_cache[i];
		uint8_t                   ssid_len = (uint8_t)e->SsidLen;
		if (ssid_len > sizeof(e->Ssid)) {
			ssid_len = (uint8_t)sizeof(e->Ssid);
		}
		const size_t rec = 11u + (size_t)ssid_len; /* CC3501E_SCAN_REC_HDR + SSID */
		if (off + rec > cap) {
			break; /* stop before overflowing the wire buffer */
		}
		for (uint32_t b = 0u; b < 6u; b++) {
			buf[off + b] = (uint8_t)e->Bssid[b];
		}
		buf[off + 6u] = (uint8_t)e->Rssi;    /* signed beacon RSSI, raw */
		buf[off + 7u] = (uint8_t)e->Channel; /* raw */
		/* Raw 16-bit SecurityInfo, little-endian.  The host needs BOTH bytes:
		 * the sec-type that distinguishes open / WPA2 / WPA3 lives in the HIGH
		 * byte (WLAN_SCAN_RESULT_SEC_TYPE_BITMAP = (SecurityInfo >> 8) & 0x3f) --
		 * the old 1-byte pack truncated to the low byte (group cipher) so the
		 * host could only ever print "?". */
		buf[off + 8u]  = (uint8_t)(e->SecurityInfo & 0xFFu);
		buf[off + 9u]  = (uint8_t)((e->SecurityInfo >> 8) & 0xFFu);
		buf[off + 10u] = ssid_len;
		for (uint32_t s = 0u; s < ssid_len; s++) {
			buf[off + 11u + s] = (uint8_t)e->Ssid[s];
		}
		off += rec;
	}
	*out_len = off;
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_scan_stop(void)
{
	/* The CC35xx scan is a one-shot that self-completes via the event; there
	 * is no standing scan to cancel.  Treat stop as a successful no-op so the
	 * host's stop after a completed scan does not error. */
	return CC3501E_HW_OK;
}

/* Publish a terminal connect outcome to the status latch (rssi first, state last --
 * a reader that observes the terminal state also observes the matching detail).
 *
 * ALSO enqueue the matching async EVT_* so a host that registered an event
 * callback (via CMD_GET_PENDING_EVENTS polling) is notified: CONNECTED ->
 * EVT_WIFI_CONNECTED, a terminal FAILED/DISCONNECTED -> EVT_WIFI_DISCONNECTED.
 * Both carry no payload -- the host reads the detail (rssi / fail_reason) via
 * CMD_WIFI_STATUS.  wifi_conn_set is the single terminal-transition chokepoint
 * (mark_connecting writes the CONNECTING latch directly and is NOT terminal), so
 * exactly one event is queued per terminal outcome. */
static void wifi_conn_set(uint8_t state, uint8_t fail_reason, int8_t rssi)
{
	g_wifi_conn.fail_reason = fail_reason;
	g_wifi_conn.rssi        = rssi;
	g_wifi_conn.state       = state;

	if (state == (uint8_t)ALP_CC3501E_WIFI_CONNECTED) {
		(void)event_ring_push((uint8_t)ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	} else if (state == (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED ||
	           state == (uint8_t)ALP_CC3501E_WIFI_DISCONNECTED) {
		(void)event_ring_push((uint8_t)ALP_CC3501E_EVT_WIFI_DISCONNECTED, NULL, 0u);
	}
}

/* STA L3 bring-up: bounded DHCP-lease poll after the L2 connect event.
 * CC3501E_STA_DHCP_TRIES * CC3501E_STA_DHCP_POLL_US = 50 * 200 ms = 10 s budget
 * (the worker drain sleeps between tries so the tcpip thread runs DHCP). */
#define CC3501E_STA_DHCP_TRIES   50u
#define CC3501E_STA_DHCP_POLL_US 200000u

int cc3501e_hw_wifi_connect_sta(const uint8_t *ssid,
                                uint8_t        ssid_len,
                                const uint8_t *psk,
                                uint8_t        psk_len,
                                uint8_t        security)
{
	if (ssid == 0 || ssid_len == 0u) {
		/* The latch was armed CONNECTING at submit (mark_connecting); a bad arg must
		 * still publish a TERMINAL outcome, else the latch stays stuck CONNECTING and
		 * the host's status poll never resolves (spins to a misleading timeout). */
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_KICK, 0);
		return CC3501E_HW_ERR_INVAL;
	}
	const int wifi_rv =
	    cc3501e_hw_wifi_ensure_sta_role(); /* lazy-start + bounded STA role-up (shared) */
	if (wifi_rv != CC3501E_HW_OK) {
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_KICK, 0);
		return wifi_rv;
	}
	osi_SyncObjClear(&wifi_event_sync);
	wifi_last_status = 0;
	/* Wlan_Connect(ssid,len,bssid=NULL,secType,pass,passlen,flags=0).  Open
	 * networks pass a NULL/zero-length password. */
	if (Wlan_Connect((const signed char *)ssid,
	                 (int)ssid_len,
	                 NULL,
	                 cc3501e_wifi_sec(security),
	                 (const char *)psk,
	                 (char)psk_len,
	                 0) != 0) {
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_KICK, 0);
		return CC3501E_HW_ERR_IO;
	}
	/* BOUNDED wait for the connect event.  This op is WORKER-ROUTED (see protocol.c
	 * wifi_join -> worker), so the wait pends off the SPI ISR; the READY/host-IRQ
	 * line (CC35 GPIO17 -> Alif P2_6, a rev-1 wire) is held BUSY for the duration --
	 * the SPI framing itself is hardware SS0 -- so the host never clocks into the
	 * dead SPI-slave DMA.  The L2 association completes on
	 * silicon.  WPA2 associates ~15s in, but WPA3-SAE is SLOWER (the extra SAE
	 * commit/confirm exchange + PMF), so a 15s wait raced the WLAN_EVENT_CONNECT and
	 * timed out on a WPA3 AP even though the association was in progress -- bench-seen
	 * on "Alp Electronix" (wpa3), 2026-07-05.  30s covers WPA2 and WPA3-SAE with margin
	 * (the bridge is BUSY for the wait, but a connect is a deliberate, infrequent op).
	 * The OUTCOME is mirrored into the status latch below; the host collects it
	 * NON-blocking via CMD_WIFI_STATUS.  DHCP/IP is brought up right after (L3). */
	if (osi_SyncObjWait(&wifi_event_sync, 30u * OSI_WAIT_FOR_SECOND) != OSI_OK) {
		/* No connect event within the wait -- TERMINAL timeout (was masked as a
		 * retryable IO that looped the host's poll-by-repeat -> -4). */
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_TIMEOUT, 0);
		return CC3501E_HW_ERR_IO;
	}
	if (wifi_last_status < 0) {
		/* FW rejected the association/auth (WLAN_EVENT_CONNECT Status<0, or a
		 * DISCONNECT/ASSOCIATION_REJECTED/AUTHENTICATION_REJECTED event) -- TERMINAL. */
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_REJECTED, 0);
		return CC3501E_HW_ERR_IO;
	}
	/* L2 ASSOCIATED.  Bring the STA netif UP at L3 + start DHCP, MIRRORING the AP path
	 * (cc3501e_hw_wifi_ap_start -> network_set_up(network_get_ap_if())).  Without this
	 * the netif stays link-down, lwIP has no route, and lwip_socket/connect fail -- the
	 * L3 gap the bench hit.  network_set_up() does netif_set_up + netif_set_link_up under
	 * LOCK_TCPIP_CORE; the STA netif's link_callback (registered at boot by
	 * network_stack_add_if_sta -> _role_sta_up) then runs dhcp_start FROM THE TCPIP
	 * CONTEXT (sta_ip_mode defaults to IP_DHCP, STATIC_IP undefined).  The old
	 * "network_set_up()/DHCP would hang the worker" caveat here PREDATED the busy-poll
	 * transport that starved the tcpip thread; the slave is now DMA-callback driven, so
	 * LOCK_TCPIP_CORE no longer deadlocks the drain (confirmed 2026-07-05).
	 *
	 * Do NOT read the RSSI here: a Wlan_Get(WLAN_GET_RSSI) immediately after associate
	 * BLOCKS on this NWP (the link is not yet settled for a beacon measurement) and hangs
	 * the worker body -- the host fetches it separately via WIFI_GET_RSSI once settled. */
	network_set_up(network_get_sta_if());

	/* GATE "CONNECTED" ON L3-UP: bounded, NON-blocking poll of the netif IP (a light read,
	 * no radio op) with a short task-sleep between tries so DHCP proceeds on the tcpip
	 * thread -- NOT a blocking semaphore wait in the drain.  Publishing CONNECTED only
	 * once a nonzero IP is leased means a host that observes CONNECTED can open sockets
	 * immediately (the netif has a route). */
	uint32_t ip = 0u, mask = 0u, gw = 0u, dhcp = 0u;
	for (unsigned i = 0u; i < CC3501E_STA_DHCP_TRIES; ++i) {
		if (network_stack_get_if_ip(WLAN_ROLE_STA, &ip, &mask, &gw, &dhcp) == 0 && ip != 0u) {
			break;
		}
		ip = 0u;
		ClockP_usleep(CC3501E_STA_DHCP_POLL_US);
	}
	if (ip == 0u) {
		/* Associated at L2 but no DHCP lease within the budget -- TERMINAL (there is no
		 * usable IP, so a "connected" report would mislead the host into failing socket
		 * ops).  The host reads this as CONN_FAILED/TIMEOUT via CMD_WIFI_STATUS. */
		wifi_conn_set(
		    (uint8_t)ALP_CC3501E_WIFI_CONN_FAILED, (uint8_t)ALP_CC3501E_WIFI_FAIL_TIMEOUT, 0);
		return CC3501E_HW_ERR_IO;
	}
	wifi_conn_set((uint8_t)ALP_CC3501E_WIFI_CONNECTED, (uint8_t)ALP_CC3501E_WIFI_FAIL_NONE, 0);
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_disconnect(void)
{
	if (!wifi_started) {
		return CC3501E_HW_OK; /* nothing to disconnect */
	}
	if (Wlan_Disconnect(WLAN_ROLE_STA, NULL) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	/* Host-requested teardown succeeded: mirror the state into the latch and
	 * queue an async EVT_WIFI_DISCONNECTED (wifi_conn_set does both). */
	wifi_conn_set((uint8_t)ALP_CC3501E_WIFI_DISCONNECTED, (uint8_t)ALP_CC3501E_WIFI_FAIL_NONE, 0);
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_ap_start(const uint8_t *ssid,
                             uint8_t        ssid_len,
                             const uint8_t *psk,
                             uint8_t        psk_len,
                             uint8_t        security)
{
	if (ssid == 0 || ssid_len == 0u || ssid_len >= WLAN_SSID_MAX_LENGTH) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* The stack must be started (Wlan_Start) before any RoleUp; lazy-start
	 * brings it up as STA first, then we role-up the AP interface. */
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	network_stack_add_if_ap();

	/* RoleUpApCmd_t.ssid is NUL-terminated in the SDK; copy + terminate. */
	uint8_t ssid_buf[WLAN_SSID_MAX_LENGTH];
	memcpy(ssid_buf, ssid, ssid_len);
	ssid_buf[ssid_len] = 0u;

	RoleUpApCmd_t ap    = { 0 };
	ap.ssid             = ssid_buf;
	ap.channel          = 6u; /* common 2.4 GHz default */
	ap.secParams.Type   = (uint8_t)cc3501e_wifi_sec(security);
	ap.secParams.Key    = (int8_t *)psk;
	ap.secParams.KeyLen = psk_len;
	if (Wlan_RoleUp(WLAN_ROLE_AP, &ap, WLAN_WAIT_FOREVER) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	network_set_up(network_get_ap_if());
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_ap_stop(void)
{
	if (!wifi_started) {
		return CC3501E_HW_OK;
	}
	if (Wlan_RoleDown(WLAN_ROLE_AP, WLAN_WAIT_FOREVER) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_get_rssi(int8_t *rssi_dbm_out)
{
	if (rssi_dbm_out == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	WlanBeaconRssi_t r = { 0 }; /* role_id 0 = STA */
	if (Wlan_Get(WLAN_GET_RSSI, &r) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	*rssi_dbm_out = r.rssi_beacon;
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_get_ip(uint8_t ip_out[4])
{
	if (ip_out == 0) {
		return CC3501E_HW_ERR_INVAL;
	}
	if (!wifi_started) {
		return CC3501E_HW_ERR_IO; /* no STA up -> no lease */
	}
	uint32_t ip = 0u, mask = 0u, gw = 0u, dhcp = 0u;
	if (network_stack_get_if_ip(WLAN_ROLE_STA, &ip, &mask, &gw, &dhcp) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	if (ip == 0u) {
		return CC3501E_HW_ERR_IO; /* DHCP not yet acquired -> NOT-ready-ish (RADIO) */
	}
	/* network_stack_get_if_ip returns the lwIP host-order u32; emit MSB-first
	 * (a.b.c.d) to match the host's 4-byte IPv4 wire order. */
	ip_out[0] = (uint8_t)((ip >> 24) & 0xFFu);
	ip_out[1] = (uint8_t)((ip >> 16) & 0xFFu);
	ip_out[2] = (uint8_t)((ip >> 8) & 0xFFu);
	ip_out[3] = (uint8_t)(ip & 0xFFu);
	return CC3501E_HW_OK;
}
#else  /* !CC3501E_WIFI -- stub / silicon-free host build */
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

int cc3501e_hw_wifi_connect_sta(const uint8_t *ssid,
                                uint8_t        ssid_len,
                                const uint8_t *psk,
                                uint8_t        psk_len,
                                uint8_t        security)
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

int cc3501e_hw_wifi_ap_start(const uint8_t *ssid,
                             uint8_t        ssid_len,
                             const uint8_t *psk,
                             uint8_t        psk_len,
                             uint8_t        security)
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
#endif /* CC3501E_WIFI */

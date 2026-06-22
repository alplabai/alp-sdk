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
#include <string.h> /* memcpy (OTA manifest buffering) */

/* CMSIS core for NVIC_SystemReset (the CC35xx M33 core).  Pulled in via the
 * device's CMSIS header -- ti_drivers_config.h does NOT bring the core in
 * transitively on this SDK (SDK 10.10). */
#include <ti/devices/cc35xx/cmsis/device.h>

#include "ti_drivers_config.h"

#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>
#include <ti/drivers/dpl/ClockP.h> /* uptime source for GET_DIAG_INFO (no radio needed) */
#include <ti/utils/FWU/psa_fwu.h> /* PSA Firmware Update: accept the MCUboot TRIAL image (cold-boot fix) */

#ifdef CC3501E_WIFI
/* CC35xx Wi-Fi host API (SDK 10.10 moved off the classic <.../simplelink.h>):
 * Wlan_Get / WlanMacAddress_t / WLAN_GET_MACADDRESS / WLAN_ROLE_STA / Wlan_Start /
 * Wlan_RoleUp / RoleUpStaCmd_t.  Impl in wifi_stack.a (pulls the OSI port ->
 * osi_dpl.c); the Wlan_* refs here prove the P0-5 link. */
#include <wlan_if.h>
#include <string.h>
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
#endif

#ifdef CC3501E_BLE
/* App-side NimBLE host adapter (cc3501e_nimble_host.c): the BLE bodies below
 * drive advertising/enable through this seam so the raw NimBLE headers stay out
 * of this TU.  CC3501E_BLE implies CC3501E_WIFI (the BLE controller shares the
 * HIF, so Wlan_Start runs first via cc3501e_hw_wifi_lazy_start). */
#include "cc3501e_nimble_host.h"
#endif

#include "alp/protocol/cc3501e.h"

#include "../cc3501e_hw.h"
#include "transport.h" /* bridge_transport_spi_hw_reinit (Wlan_Start DMA-coexistence fix) */

/* Bridge SPI desync counter (transport_hw_ti_spi.c): increments each time the
 * slave re-arms the header phase on a reserved-range/0xA5 header (a misframe).
 * A burst in one housekeeping tick means the link is stuck desynced + the RX
 * FIFO likely holds residue -- cc3501e_hw_tick() flushes via a full SPI re-open. */
extern volatile uint32_t g_resync_count;

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
		wifi_scan_count  = n;
		wifi_last_status = ext_null ? -77 : 0; /* DIAG: -77 = EXTENDED event had a NULL data pointer */
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
 * Wlan_* data op. */
static int cc3501e_hw_wifi_lazy_start(void)
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
	(void)cc3501e_hw_wifi_lazy_start();
	/* Bring the STA role up ONCE here at boot, before the host polls.  RoleUp is a
	 * HEAVY radio op that needs the bridge quiesced (suspend) to kick; doing it in
	 * the scan hot path made the scan's bridge-down window ~30s and churned the
	 * CS-less link past re-sync.  Pre-cached here, each later scan is just a LIGHT
	 * Wlan_Scan (role already up) that kicks with NO suspend -- like GET_MAC's
	 * Wlan_Get.  Suspend for the RoleUp, then reinit the bridge clean. */
	bridge_transport_spi_hw_suspend();
	(void)cc3501e_hw_wifi_ensure_sta_role();
	bridge_transport_spi_hw_reinit();
}
#else  /* !CC3501E_WIFI -- default v0.1 ti build brings up no radio */
void cc3501e_hw_wifi_boot_start(void)
{
	/* No radio linked in this build -- nothing to bring up. */
}
#endif /* CC3501E_WIFI */

/* Deferred-reset latch: CMD_RESET sets this; cc3501e_hw_tick() performs
 * the reboot on the next idle wakeup, but ONLY after reply_drained
 * confirms the OK ack has fully clocked back to the host. */
static volatile bool reset_pending;

/* Set by cc3501e_hw_notify_reply_sent() when the in-flight reply (for
 * CMD_RESET, the ack itself) has fully clocked out; cleared by
 * cc3501e_hw_request_reset() so only that command's own ack arms the
 * reboot.  Gates reset_pending in cc3501e_hw_tick(). */
static volatile bool reply_drained;

/* Deferred OTA swap-reboot latch: cc3501e_hw_ota_finish() sets it; cc3501e_hw_tick()
 * calls psa_fwu_request_reboot() once reply_drained confirms the FINISH ack has
 * clocked back (same ack-before-reboot race fix as reset_pending). */
static volatile bool ota_reboot_pending;

/* Runs a queued OTA op's flash work off the SPI ISR (defined with the OTA
 * section below); called from cc3501e_hw_tick on the bring-up task. */
void cc3501e_hw_ota_pump(void);

/* Minimal OSI sleep shim for the PSA-FWU lib (FWU.a references osi_uSleep -- the Wi-Fi host
 * driver's OSI microsecond sleep -- for a flash-commit settle).  The full OSI layer lives in
 * the Wi-Fi platform we do NOT link in v0.1, and psa_fwu_accept() runs PRE-scheduler (from
 * cc3501e_hw_init, before vTaskStartScheduler) where a yielding sleep can't run, so use a
 * busy-wait.  Signature matches <ti/drivers/net/wifi/wifi_host_driver/inc_adapt/osi_kernel.h>
 * (OsiReturnVal_e osi_uSleep(OsiTime_t); OSI_OK == 0).  Over-delay is harmless.
 * Compiled out under CC3501E_WIFI: the real OSI in wifi_platform_cc35xx.a then provides
 * osi_uSleep, and defining it here too would be a multiple-definition at link (P0-5). */
#ifndef CC3501E_WIFI
int osi_uSleep(unsigned int usec)
{
	volatile unsigned int spins = usec * 100u; /* generous busy-wait; commit settle only */
	while (spins--) {
		__asm__ volatile("nop");
	}
	return 0; /* OSI_OK */
}
#endif /* !CC3501E_WIFI */

/* ---- async-worker ISR/drain mutual exclusion (P0-4) -------------- *
 * The silicon-free worker (src/worker.c) publishes a job result with a
 * multi-field write that the SPI ISR (which polls that result) must never
 * observe half-done.  worker.c declares these as WEAK no-ops (correct for
 * the single-threaded native build); on real silicon the SPI driver's
 * transfer callback runs in HWI/SwiP context, so we mask interrupts for
 * the short publish/read window.  PRIMASK save/restore (not a bare
 * enable) so a nested critical section can't prematurely re-enable. */
unsigned long worker_critical_enter(void)
{
	const unsigned long key = __get_PRIMASK();
	__disable_irq();
	return key;
}

void worker_critical_exit(unsigned long key)
{
	__set_PRIMASK(key);
}

void cc3501e_hw_init(void)
{
	/* TI Drivers core init.  GPIO + SPI are brought up here so the
	 * transport's hw-init hook can open its handles.  No network
	 * processor in v0.1 (see file header). */
	/* The SPI-slave driver is SPIWFF3DMA: it is DMA-based and its SPI_open() calls
	 * Power_setDependency(), so the Power AND DMA drivers MUST be initialized before
	 * any driver open.  Board_init() (generated ti_drivers_config.c) =
	 * Power_init() + GPIO_init() + DMAWFF3_init(); every CC35X1 SDK example calls it
	 * first.  Calling only GPIO_init()+SPI_init() (the prior bug) left Power/DMA
	 * uninitialized -> SPI_open faulted -> the slave never armed and POCI was never
	 * driven (root-caused 2026-06-17 via deep analysis). */
	Board_init();
	SPI_init();
	/* The MCUboot/PSA-FWU TRIAL-image accept is done on the first cc3501e_hw_tick()
	 * (POST-scheduler), NOT here -- the HSM/FWU services that finalize the OTA commit
	 * only run once the FreeRTOS scheduler is up.  See cc3501e_hw_tick(). */
}

#ifdef CC3501E_OTA_SELFTEST
/* ===================================================================== */
/* OTA update cycle (PSA-FWU) -- the supported path to install a CONFIRMED,
 * cold-launchable PRIMARY vendor image on an ALREADY-ACTIVATED unit.
 *
 * A directly-`programming`-written primary is NOT cold-launchable (proven on
 * silicon 2026-06-17); only `factory_programming`'s atomic activation-context
 * write (fresh unit) or an OTA SWAP produces a cold-launchable primary.  This
 * runs the real OTA cycle: write a candidate to the non-primary vendor slot ->
 * install -> reboot, so the cold BL2/MCUboot SWAPS secondary->primary and boots
 * it as a TRIAL; the swapped image's first boot then psa_fwu_accept()s it
 * (cc3501e_hw_tick below, non-SELFTEST build) -> permanent + cold-launchable.
 *
 * SELFTEST drives it from an EMBEDDED candidate (the plain v0.0.4.0 signed
 * vendor image) to validate the mechanism without a host transport; the SAME
 * cc3501e_ota_install() will be driven by OTA-over-bridge commands as the v0.2
 * OTA feature (Mender contract).  The candidate is the GPE-format signed vendor
 * image: first TI_FWU_MANIFEST_SIZE bytes are the manifest (psa_fwu_start), the
 * rest is streamed via psa_fwu_write (mirrors examples/.../ota_example). */
extern const unsigned char cc3501e_ota_candidate[];
extern const unsigned int  cc3501e_ota_candidate_len;

/* Install a GPE-format signed vendor image into the alternate (non-primary)
 * vendor slot and request the reboot that performs the swap.  Returns 0 on
 * "install staged + reboot requested" (does not return if reboot is immediate),
 * negative on any failure (caller falls through harmlessly). */
static int cc3501e_ota_install(const unsigned char *img, unsigned int len)
{
	psa_fwu_component_info_t i1, i2, ti;
	psa_fwu_component_t      target;
	unsigned int             off;

	if (img == 0 || len <= TI_FWU_MANIFEST_SIZE) {
		return -1;
	}
	/* Pick the non-primary vendor slot as the update target (slot 4 <-> 5). */
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_1, &i1) != PSA_SUCCESS) {
		return -2;
	}
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_2, &i2) != PSA_SUCCESS) {
		return -2;
	}
	if (i1.impl.Primary && !i2.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_2;
	} else if (i2.impl.Primary && !i1.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_1;
	} else {
		return -3; /* can't identify a single non-primary target */
	}

	/* Bring the target slot to READY (clean/cancel a stale candidate). */
	if (psa_fwu_query(target, &ti) != PSA_SUCCESS) {
		return -4;
	}
	if (ti.state != PSA_FWU_READY) {
		if (ti.state == PSA_FWU_WRITING || ti.state == PSA_FWU_CANDIDATE) {
			psa_fwu_cancel(target);
		} else {
			psa_fwu_clean(target);
		}
	}

	/* Manifest = first TI_FWU_MANIFEST_SIZE bytes; image = the remainder. */
	if (psa_fwu_start(target, img, TI_FWU_MANIFEST_SIZE) != PSA_SUCCESS) {
		return -5;
	}
	for (off = TI_FWU_MANIFEST_SIZE; off < len;) {
		unsigned int n = len - off;
		if (n > CC3501E_OTA_WRITE_CHUNK) {
			n = CC3501E_OTA_WRITE_CHUNK;
		}
		if (psa_fwu_write(target, off, img + off, n) != PSA_SUCCESS) {
			return -6;
		}
		off += n;
	}
	if (psa_fwu_finish(target) != PSA_SUCCESS) {
		return -7;
	}
	if (psa_fwu_install() != PSA_SUCCESS) { /* CANDIDATE -> STAGED */
		return -8;
	}
	psa_fwu_request_reboot(); /* reboot -> BL2 swaps secondary->primary -> TRIAL boot */
	return 0;
}
#endif /* CC3501E_OTA_SELFTEST */

void cc3501e_hw_tick(void)
{
	/* === COLD-BOOT FIX (2026-06-17): accept the MCUboot / PSA-FWU TRIAL image (one-shot). ===
	 * TRM SWRU626 §10.3.2: CC35xx vendor images are MCUboot images.  On this OTA device a
	 * freshly-programmed vendor image boots in PSA_FWU_TRIAL and is REVERTED by the cold
	 * BL2/MCUboot Chain-of-Trust on the next boot UNLESS the RUNNING app accepts (commits) it.
	 * A warm/programmer/debug launch bypasses the trailer check and runs the trial (so PING
	 * worked), but it was never confirmed -> every cold power-on reverted the unconfirmed
	 * trial and the vendor image never launched (host saw reqhdr_rx=0xFFFFFFFF).  Accept on
	 * the FIRST tick -- this runs POST-scheduler (from bringup_task), where the HSM/FWU
	 * services that finalize the commit are up (accept from cc3501e_hw_init, pre-scheduler,
	 * does not finalize).  Mirrors examples/.../ota_example: init -> accept -> reboot-to-commit.
	 * After commit the image is permanent -> subsequent COLD boots launch it. */
	static bool fwu_accept_done = false;
	if (!fwu_accept_done) {
		fwu_accept_done = true;
		psa_fwu_init();
#ifdef CC3501E_OTA_SELFTEST
		/* One-shot bench validation: OTA-install the embedded candidate to the
		 * alternate vendor slot and reboot so BL2 swaps it to primary.  If it
		 * succeeds the device reboots here (does not return); the swapped image
		 * is a PLAIN build (no SELFTEST) that accept()s itself below on its
		 * TRIAL boot -> permanent.  If install fails, fall through to accept. */
		(void)cc3501e_ota_install(cc3501e_ota_candidate, cc3501e_ota_candidate_len);
#endif
		if (psa_fwu_accept() == PSA_SUCCESS_REBOOT) {
			psa_fwu_request_reboot(); /* finalize commit; device reboots, image now permanent */
		}
		/* PSA_ERROR_BAD_STATE = nothing in trial (already permanent) -> continue. */
	}

	/* === Bridge SPI FIFO-flush recovery (cold-framing self-heal) ===
	 * On the CS-less fixed-count link a 1-byte frame offset (RX FIFO residue from
	 * the master over-clocking during a desync) cannot self-correct by more
	 * clocking -- it persists until the FIFO is flushed.  g_resync_count bursts
	 * while the slave is stuck re-arming garbage (each misframed header reads the
	 * 0xA5 idle, which is in the reserved cmd range -> re-arm++).  A burst within
	 * one ~10 ms housekeeping tick = the link is stuck: do a full SPI re-open
	 * (SPI_close/open) which FLUSHES the RX FIFO + DMA and re-arms a fresh header.
	 * If this lands in the host's inter-PING gap the slave's fresh RX then aligns
	 * with the host's next 4-byte transfer.  Healthy traffic uses valid (cmd<0x80)
	 * headers and does not bump g_resync_count, so this never fires when aligned. */
	static uint32_t last_resync;
	const uint32_t  rc = g_resync_count;
	if ((uint32_t)(rc - last_resync) >= 3u) {
		bridge_transport_spi_hw_reinit(); /* flush FIFO+DMA, re-arm clean */
		last_resync = 0u;                 /* reinit zeroes g_resync_count */
	} else {
		last_resync = rc;
	}

	/* Deferred self-reset, gated on reply_drained so the CMD_RESET ack has
	 * FULLY clocked to the host before the chip resets (audit
	 * "reset-fires-before-ack-clocked": the reset previously raced the
	 * in-flight ack and the host never saw it).  The host sees the ack,
	 * then the link goes quiet, then the firmware re-PINGs alive. */
	/* Drain any queued OTA op: the slow psa_fwu flash work runs HERE (bring-up
	 * task), never in the SPI ISR.  The flash op still stalls the CC35, so the
	 * pump stands the bridge slave down for its duration (suspend/reinit, like a
	 * radio op) -- see cc3501e_hw_ota_pump(). */
	cc3501e_hw_ota_pump();

	if (reset_pending && reply_drained) {
		NVIC_SystemReset(); /* CMSIS: M33 system reset -- does not return */
	}

	/* Deferred OTA reboot: once the FINISH ack has clocked back (reply_drained),
	 * request the PSA-FWU reboot so the cold BL2/MCUboot swaps the STAGED slot to
	 * primary (TRIAL).  Same ack-before-reboot race fix as CMD_RESET above. */
	if (ota_reboot_pending && reply_drained) {
		psa_fwu_request_reboot(); /* swap-reboot -- does not return */
	}
}

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

/* ===================================================================== */
/* OTA firmware update (over-the-bridge PSA-FWU streaming) -- v0.2.       */
/*                                                                       */
/* The Alif host streams a signed GPE vendor image into the non-primary  */
/* vendor slot (BEGIN -> WRITE* -> FINISH), then FINISH installs + arms a */
/* deferred reboot so the cold BL2/MCUboot swaps the slot to primary.     */
/* This is the streamed sibling of the SELFTEST cc3501e_ota_install()     */
/* (which feeds the same psa_fwu_* sequence from an embedded array).      */
/* Single session; bytes arrive sequentially (offset == cursor).         */

/* RAM-STAGED OTA (silicon-critical, r1 no-CS/no-IRQ bridge): the psa_fwu_* flash
 * ops share the CC35 HIF/DMA with the bridge SPI slave, so EVERY flash op tears
 * the bridge DMA down (like a radio op) -- doing one per 256 B WRITE desynced the
 * lockstep + churned the link across the ~135-chunk stream, no reinit dance made
 * it reliable (silicon 2026-06-19).  So WRITES never touch flash: each chunk is a
 * synchronous RAM memcpy into image_buf (ISR-safe, no DMA disruption -> the bulk
 * transfer stays clean).  ALL the flash happens ONCE at FINISH (psa_fwu_start +
 * write the whole staged image + install), deferred to cc3501e_hw_ota_pump() on
 * the bring-up task with a single bridge re-arm after.  (r2 adds CS + a host-IRQ;
 * then per-chunk flash + a smaller buffer become viable.) */
#define CC3501E_OTA_IMAGE_MAX (64u * 1024u) /* max staged image; begin rejects larger */
#define CC3501E_OTA_WRITE_CHUNK                                                                    \
	4096u /* finish flash block: big => few psa_fwu_write calls (each tears the bridge DMA), short burst */

#define OTA_OP_IDLE     0u
#define OTA_OP_BEGIN    1u
#define OTA_OP_FINISH   3u /* WRITE is synchronous (RAM memcpy) -- not a deferred op */
#define OTA_OP_INFLIGHT 2  /* op_rc sentinel: queued, not yet executed (!= any CC3501E_HW_*) */

static struct {
	uint8_t             state; /* alp_cc3501e_ota_state_t */
	psa_fwu_component_t target;
	uint32_t            total_len;
	uint32_t            cursor; /* bytes staged into image_buf so far */
	/* Deferred BEGIN/FINISH queue (ISR enqueues; ota_pump runs the flash). */
	volatile uint8_t op;       /* OTA_OP_* currently queued/running */
	volatile int8_t  op_rc;    /* OTA_OP_INFLIGHT while pending; else result */
	uint32_t         op_total; /* BEGIN arg */
	uint8_t          image_buf[CC3501E_OTA_IMAGE_MAX]; /* full image staged in RAM */
} ota;

/* Enqueue op @o (args already staged) and return BUSY: an op is in flight while
 * op_rc == OTA_OP_INFLIGHT.  The pump publishes the result + frees the slot
 * (auto-resets op to IDLE); the host observes completion through OTA_STATUS
 * (state / cursor), NOT by re-collecting -- so a WRITE poll never has to re-send
 * its 256 B payload while the device is mid-flash (which would desync the CS-less
 * lockstep).  Fast + ISR-safe (no flash here). */
static int ota_submit(uint8_t o)
{
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY; /* an op is running */
	ota.op    = o;
	ota.op_rc = OTA_OP_INFLIGHT;
	return CC3501E_HW_BUSY;
}

/* ---- slow bodies (run ONLY from ota_pump, off the SPI ISR) ----------------- */

static int ota_do_begin(void)
{
	psa_fwu_component_info_t i1 = { 0 }, i2 = { 0 }, ti = { 0 };
	psa_fwu_component_t      target;

	psa_fwu_init(); /* idempotent */
	if (psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_1, &i1) != PSA_SUCCESS ||
	    psa_fwu_query((psa_fwu_component_t)Vendor_Image_Slot_2, &i2) != PSA_SUCCESS) {
		return CC3501E_HW_ERR_IO;
	}
	if (i1.impl.Primary && !i2.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_2;
	} else if (i2.impl.Primary && !i1.impl.Primary) {
		target = (psa_fwu_component_t)Vendor_Image_Slot_1;
	} else {
		return CC3501E_HW_ERR_IO;
	}
	if (psa_fwu_query(target, &ti) != PSA_SUCCESS) return CC3501E_HW_ERR_IO;
	if (ti.state != PSA_FWU_READY) {
		if (ti.state == PSA_FWU_WRITING || ti.state == PSA_FWU_CANDIDATE) {
			psa_fwu_cancel(target); /* slow: erase */
		} else {
			psa_fwu_clean(target);
		}
	}
	ota.target    = target;
	ota.total_len = ota.op_total;
	ota.cursor    = 0u;
	ota.state     = ALP_CC3501E_OTA_STATE_WRITING;
	return CC3501E_HW_OK;
}

/* FINISH: commit the whole RAM-staged image to the target slot in ONE flash burst
 * (manifest = first TI_FWU_MANIFEST_SIZE bytes -> psa_fwu_start; the remainder in
 * CC3501E_OTA_WRITE_CHUNK pages -> psa_fwu_write), finalize + install, then arm
 * the swap-reboot.  All the OTA flash (hence all bridge-DMA disruption) is here. */
static int ota_do_finish(void)
{
	if (ota.cursor != ota.total_len || ota.total_len <= (uint32_t)TI_FWU_MANIFEST_SIZE) {
		return CC3501E_HW_ERR_INVAL;
	}
	/* Force the target component's persistent flash flow-state to READY before
	 * psa_fwu_start.  A prior failed/partial OTA leaves the flash flow-state stuck
	 * (set inside psa_fwu_start / _install), and psa_fwu_start's own flow_check then
	 * returns PSA_ERROR_BAD_STATE(-137) forever -- the RAM ComponentInfo.state can
	 * still read READY, so this must NOT be gated on it (silicon 2026-06-19).  Walk
	 * every stuck state back to READY (ignore each rc -- they no-op when N/A):
	 *   cancel  WRITING/CANDIDATE -> FAILED
	 *   reject  STAGED            -> FAILED   (an install that never swap-booted)
	 *   clean   FAILED/UPDATED    -> READY
	 * (STAGED is the common stuck case here: a finish reached psa_fwu_install but
	 * the cold swap-reboot could not complete -- see project-cc3501e-firmware-bringup.) */
	(void)psa_fwu_cancel(ota.target);
	(void)psa_fwu_reject(PSA_ERROR_GENERIC_ERROR);
	(void)psa_fwu_clean(ota.target);

	if (psa_fwu_start(ota.target, ota.image_buf, TI_FWU_MANIFEST_SIZE) != PSA_SUCCESS) {
		return CC3501E_HW_ERR_IO;
	}
	uint32_t since_rearm = 0u;
	for (uint32_t off = (uint32_t)TI_FWU_MANIFEST_SIZE; off < ota.total_len;) {
		uint32_t n = ota.total_len - off;
		if (n > CC3501E_OTA_WRITE_CHUNK) {
			n = CC3501E_OTA_WRITE_CHUNK;
		}
		if (psa_fwu_write(ota.target, off, &ota.image_buf[off], n) != PSA_SUCCESS) {
			return CC3501E_HW_ERR_IO;
		}
		off += n;
		/* Re-arm the bridge slave periodically across the flash burst so the host's
		 * header-only FINISH poll keeps getting serviced (BUSY) instead of a long IO
		 * blackout that would time out the host (silicon 2026-06-19). */
		if (++since_rearm >= 2u) {
			since_rearm = 0u;
			bridge_transport_spi_hw_reinit();
		}
	}
	psa_status_t pf = psa_fwu_finish(ota.target);
	if (pf != PSA_SUCCESS && pf != PSA_SUCCESS_REBOOT) {
		return CC3501E_HW_ERR_IO;
	}
	/* psa_fwu_install stages the swap and returns PSA_SUCCESS_REBOOT(1) -- a SUCCESS
	 * code meaning "reboot to complete the swap", NOT an error. */
	psa_status_t pi = psa_fwu_install(); /* CANDIDATE -> STAGED */
	if (pi != PSA_SUCCESS && pi != PSA_SUCCESS_REBOOT) {
		return CC3501E_HW_ERR_IO;
	}
	ota.state = ALP_CC3501E_OTA_STATE_STAGED;
	/* Arm the standard swap-reboot: the tick calls psa_fwu_request_reboot once the
	 * FINISH ack has drained -> the device reboots, BL2 swaps the STAGED slot to
	 * primary (TRIAL), the new image boots and self-accepts (cc3501e_hw_tick).  This
	 * is the production OTA contract.  (On the current mis-activated bench unit the
	 * swap-boot is gated by the vendor-SBL cold-boot issue -- see
	 * project-cc3501e-ota-bridge-rootcause -- but the receive/stage/install pipeline
	 * up to STAGED is silicon-validated.) */
	reply_drained      = false;
	ota_reboot_pending = true;
	return CC3501E_HW_OK;
}

/* Run a queued OTA op (bring-up task, NOT the SPI ISR).  Called from hw_tick.
 * The slow psa_fwu flash work runs HERE, never in the SPI ISR.
 *
 * The psa_fwu flash op writes the external xSPI image store, which shares the
 * CC35 HIF/DMA controller with the bridge SPI -- exactly like a radio op (see
 * transport_hw_ti_spi.c header), it leaves the bridge slave's DMA torn down, so
 * the link goes silent until the slave is re-opened.  Recover with the SAME
 * recover-AFTER reinit the radio path uses: run the op, THEN re-open + re-arm the
 * slave at a clean boundary.  This is recover-AFTER only -- NO suspend BEFORE
 * (SPI_transferCancel/close before the op raced the live SPI callback and locked
 * the core up; bench-proven 2026-06-19).  The host poll-retries on ALP_ERR_IO
 * across the down-window (its OTA_WRITE pushes the payload once then polls
 * header-only STATUS, so nothing is half-served across the flash). */
void cc3501e_hw_ota_pump(void)
{
	if (ota.op_rc != OTA_OP_INFLIGHT) return; /* nothing queued */
	int rc;
	switch (ota.op) {
	case OTA_OP_BEGIN:
		rc = ota_do_begin();
		break;
	case OTA_OP_FINISH:
		rc = ota_do_finish();
		break;
	default:
		rc = CC3501E_HW_ERR_INVAL;
		break;
	}
	if (rc != CC3501E_HW_OK && rc != CC3501E_HW_BUSY) {
		ota.state = ALP_CC3501E_OTA_STATE_ERROR;
	}
	bridge_transport_spi_hw_reinit(); /* flash tore the bridge DMA down -- re-open + re-arm */
	ota.op    = OTA_OP_IDLE;          /* free the slot -- result is observable via STATUS */
	ota.op_rc = (int8_t)rc;           /* publish LAST: clears INFLIGHT so a new op can queue */
}

int cc3501e_hw_ota_begin(uint32_t total_len)
{
	if (total_len <= (uint32_t)TI_FWU_MANIFEST_SIZE || total_len > CC3501E_OTA_IMAGE_MAX) {
		return CC3501E_HW_ERR_INVAL; /* too small to hold a manifest, or larger than the RAM buffer */
	}
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;             /* op running */
	if (ota.state == ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_OK; /* already begun */
	ota.op_total = total_len; /* stage before the queue slot opens */
	return ota_submit(OTA_OP_BEGIN);
}

/* OTA_WRITE: SYNCHRONOUS -- just stage the chunk into RAM (image_buf).  No flash
 * here (that all happens at FINISH), so this is ISR-safe + causes no bridge-DMA
 * disruption: the bulk transfer stays clean across all ~135 chunks.  Idempotent
 * on the cursor so a host re-send of an already-staged chunk is harmless. */
int cc3501e_hw_ota_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (data == 0 || len == 0u || len > (uint32_t)ALP_CC3501E_OTA_MAX_CHUNK) {
		return CC3501E_HW_ERR_INVAL;
	}
	if ((uint64_t)offset + len <= ota.cursor) return CC3501E_HW_OK; /* chunk already staged */
	if (offset != ota.cursor) return CC3501E_HW_ERR_INVAL;          /* out of order */
	if ((uint64_t)offset + len > ota.total_len || (uint64_t)offset + len > CC3501E_OTA_IMAGE_MAX) {
		return CC3501E_HW_ERR_INVAL; /* overruns the declared image / the RAM buffer */
	}
	memcpy(&ota.image_buf[offset], data, len);
	ota.cursor += len;
	return CC3501E_HW_OK;
}

int cc3501e_hw_ota_finish(void)
{
	if (ota.state == ALP_CC3501E_OTA_STATE_STAGED) return CC3501E_HW_OK; /* already finished */
	if (ota.state != ALP_CC3501E_OTA_STATE_WRITING) return CC3501E_HW_ERR_INVAL;
	if (ota.op_rc == OTA_OP_INFLIGHT) return CC3501E_HW_BUSY;
	return ota_submit(OTA_OP_FINISH);
}

int cc3501e_hw_ota_abort(void)
{
	/* Discard the RAM-staged image.  No psa_fwu_cancel needed: FINISH is the only
	 * thing that touches the slot, so an aborted session never opened one. */
	ota.state     = ALP_CC3501E_OTA_STATE_IDLE;
	ota.cursor    = 0u;
	ota.total_len = 0u;
	ota.op        = OTA_OP_IDLE;
	ota.op_rc     = 0;
	return CC3501E_HW_OK;
}

int cc3501e_hw_ota_status(uint8_t *state, uint32_t *bytes_written, uint32_t *total_len)
{
	if (state != 0) *state = ota.state;
	if (bytes_written != 0) *bytes_written = ota.cursor;
	if (total_len != 0) *total_len = ota.total_len;
	return CC3501E_HW_OK;
}

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

/* GPIO interrupt handler.  Async EVT_GPIO_INTERRUPT delivery to the host needs
 * the next-rev host-IRQ line (the CS-less 3-wire link has no slave->master
 * attention path), so this only clears the pending edge for now; the HW arming
 * (cc3501e_hw_gpio_set_interrupt) itself is real. */
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
		return (char)WLAN_SEC_TYPE_WPA3;
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
	uint8_t sta_wifi_band = (uint8_t)BAND_SEL_ONLY_2_4GHZ; /* our antenna/AP are 2.4 GHz; BOTH made the kick fail (5G) */
	(void)Wlan_Set(WLAN_SET_STA_WIFI_BAND, &sta_wifi_band);
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
	 * the CS-less link past re-sync; GET_MAC, reinit-only, never churns).  reinit
	 * AFTER recovers the slave's DMA that Wlan_Scan tore down. */
	const int role_rv = cc3501e_hw_wifi_ensure_sta_role();

	scanCommon_t sc = { 0 };
	sc.Band         = BAND_SEL_ONLY_2_4GHZ; /* 2.4 GHz antenna; matches the role-up band-set */
	wifi_scan_count = 0u;
	osi_SyncObjClear(&wifi_event_sync);
	const uint32_t cb_before = wifi_cb_event_count; /* DIAG: did ANY wifi event fire? */
	int scan_rv = -1;
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

int cc3501e_hw_wifi_connect_sta(
    const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk, uint8_t psk_len, uint8_t security)
{
	if (ssid == 0 || ssid_len == 0u) {
		return CC3501E_HW_ERR_INVAL;
	}
	const int wifi_rv =
	    cc3501e_hw_wifi_ensure_sta_role(); /* lazy-start + bounded STA role-up (shared) */
	if (wifi_rv != CC3501E_HW_OK) {
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
		return CC3501E_HW_ERR_IO;
	}
	if (osi_SyncObjWait(&wifi_event_sync, OSI_WAIT_FOREVER) != OSI_OK) {
		return CC3501E_HW_ERR_IO;
	}
	if (wifi_last_status < 0) {
		return CC3501E_HW_ERR_IO; /* FW rejected the association/auth */
	}
	/* Associated -> start DHCP on the STA interface (no IP-acquired event;
	 * the host reads the lease later via WIFI_GET_IP). */
	network_set_up(network_get_sta_if());
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
	return CC3501E_HW_OK;
}

int cc3501e_hw_wifi_ap_start(
    const uint8_t *ssid, uint8_t ssid_len, const uint8_t *psk, uint8_t psk_len, uint8_t security)
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
#endif /* CC3501E_WIFI */

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
#error                                                                                             \
    "CC3501E_BLE requires CC3501E_WIFI (shared HIF -> Wlan_Start first); build with -Ble (which implies -WifiHostDriver)."
#endif
/* BLE_ENABLE: start Wi-Fi first (shared HIF), then bring up the NimBLE host.
 * Reached ONLY from the async worker's drain (never the SPI ISR): both the
 * Wlan_Start lazy-init and nimble_host_start() block for seconds.  Re-opens
 * the bridge SPI after each radio op, exactly as the Wi-Fi bodies do. */
int cc3501e_hw_ble_enable(void)
{
	/* Wi-Fi must be up before BLE (shared HIF).  lazy_start re-opens the
	 * bridge SPI after the Wlan_Start HIF disruption (its own bench-proven
	 * fix); we re-open again after the NimBLE bring-up below. */
	const int wifi_rv = cc3501e_hw_wifi_lazy_start();
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}

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

int cc3501e_hw_ble_disable(void)
{
	return CC3501E_HW_ERR_NOTIMPL;
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
#endif /* CC3501E_BLE */

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

/* --------------------------------------------------------------- */
/* Power policy + diagnostics.                                       */
/* TODO(cc3501e): apply the policy via the CC35xx power driver, route */
/* the log level, source reset_cause from the boot/reset-cause        */
/* register, and uptime from ClockP.  v0.1 accepts the config and     */
/* reports best-effort diag. */
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
	return (uint8_t)ALP_CC3501E_RESET_UNKNOWN;
}

uint32_t cc3501e_hw_uptime_ms(void)
{
	/* Real uptime from the DPL clock (TI Drivers, RTOS-backed -- no radio
	 * needed).  getSystemTicks() is a 32-bit tick count; getSystemTickPeriod()
	 * is microseconds-per-tick.  Compute in 64-bit to avoid the ticks*us
	 * overflow, then return milliseconds (wraps after ~49 days, documented). */
	const uint64_t ticks     = (uint64_t)ClockP_getSystemTicks();
	const uint64_t period_us = (uint64_t)ClockP_getSystemTickPeriod();
	return (uint32_t)((ticks * period_us) / 1000u);
}

/* FreeRTOS heap-accounting API (configurability diag source).  Declared extern
 * so this TU does not pull in the kernel headers; it resolves at link time iff
 * the SysConfig FreeRTOS aggregate links a heap_N implementation (dynamic
 * allocation enabled, which this image uses for the scheduler + SwiP path). */
extern size_t xPortGetFreeHeapSize(void);

uint32_t cc3501e_hw_free_heap_bytes(void)
{
	return (uint32_t)xPortGetFreeHeapSize();
}

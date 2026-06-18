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
#include <ti/drivers/dpl/ClockP.h> /* uptime source for GET_DIAG_INFO (no radio needed) */
#include <ti/utils/FWU/psa_fwu.h>  /* PSA Firmware Update: accept the MCUboot TRIAL image (cold-boot fix) */

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
static OsiSyncObj_t wifi_event_sync;        /* signalled by the event cb        */
static volatile int wifi_last_status;       /* WLAN event Status (<0 = fail)    */

/* Scan-result cache: the whole AP list arrives in ONE WLAN_EVENT_SCAN_RESULT.
 * The cb snapshots it here; a SCAN body copies/packs it after the wait. */
#define WIFI_SCAN_CACHE_MAX WLAN_MAX_SCAN_COUNT
static volatile uint32_t   wifi_scan_count;
static WlanNetworkEntry_t  wifi_scan_cache[WIFI_SCAN_CACHE_MAX];

/* Wi-Fi event callback -- runs on the host-driver task (NOT the SPI ISR, NOT
 * the worker).  Snapshots the result the issuing op is waiting on, then signals
 * the rendezvous.  GET_MAC takes no event (blocking get) so it is not handled
 * here.  osi_SyncObjSignal (not *FromISR): the cb is thread context. */
static void wifi_event_cb(WlanEvent_t *event)
{
	if (event == NULL) {
		return;
	}
	switch (event->Id) {
	case WLAN_EVENT_SCAN_RESULT: {
		uint32_t n = event->Data.ScanResult.NetworkListResultLen;
		if (n > WIFI_SCAN_CACHE_MAX) {
			n = WIFI_SCAN_CACHE_MAX;
		}
		for (uint32_t i = 0; i < n; i++) {
			wifi_scan_cache[i] = event->Data.ScanResult.NetworkListResult[i];
		}
		wifi_scan_count   = n;
		wifi_last_status  = 0;
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

#define CC3501E_OTA_WRITE_CHUNK 256u /* 4-byte-aligned flash write block */

/* Install a GPE-format signed vendor image into the alternate (non-primary)
 * vendor slot and request the reboot that performs the swap.  Returns 0 on
 * "install staged + reboot requested" (does not return if reboot is immediate),
 * negative on any failure (caller falls through harmlessly). */
static int cc3501e_ota_install(const unsigned char *img, unsigned int len)
{
	psa_fwu_component_info_t i1, i2, ti;
	psa_fwu_component_t target;
	unsigned int off;

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
	for (off = TI_FWU_MANIFEST_SIZE; off < len; ) {
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
	const uint32_t rc = g_resync_count;
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
	if (reset_pending && reply_drained) {
		NVIC_SystemReset(); /* CMSIS: M33 system reset -- does not return */
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
	WlanMacAddress_t p = {.roleType = WLAN_ROLE_STA};
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
	scanCommon_t sc = { 0 };
	sc.Band         = BAND_SEL_BOTH;
	wifi_scan_count = 0u;
	osi_SyncObjClear(&wifi_event_sync);
	if (Wlan_Scan(WLAN_ROLE_STA, &sc, WLAN_MAX_SCAN_COUNT) != 0) {
		return CC3501E_HW_ERR_IO;
	}
	if (osi_SyncObjWait(&wifi_event_sync, OSI_WAIT_FOREVER) != OSI_OK) {
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
		const size_t rec = 10u + (size_t)ssid_len; /* CC3501E_SCAN_REC_HDR + SSID */
		if (off + rec > cap) {
			break; /* stop before overflowing the wire buffer */
		}
		for (uint32_t b = 0u; b < 6u; b++) {
			buf[off + b] = (uint8_t)e->Bssid[b];
		}
		buf[off + 6u] = (uint8_t)e->Rssi;         /* signed beacon RSSI, raw */
		buf[off + 7u] = (uint8_t)e->Channel;      /* raw */
		buf[off + 8u] = (uint8_t)e->SecurityInfo; /* raw */
		buf[off + 9u] = ssid_len;
		for (uint32_t s = 0u; s < ssid_len; s++) {
			buf[off + 10u + s] = (uint8_t)e->Ssid[s];
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
	const int wifi_rv = cc3501e_hw_wifi_ensure_sta_role(); /* lazy-start + bounded STA role-up (shared) */
	if (wifi_rv != CC3501E_HW_OK) {
		return wifi_rv;
	}
	osi_SyncObjClear(&wifi_event_sync);
	wifi_last_status = 0;
	/* Wlan_Connect(ssid,len,bssid=NULL,secType,pass,passlen,flags=0).  Open
	 * networks pass a NULL/zero-length password. */
	if (Wlan_Connect((const signed char *)ssid, (int)ssid_len, NULL, cc3501e_wifi_sec(security),
	                 (const char *)psk, (char)psk_len, 0) != 0) {
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

	RoleUpApCmd_t ap = { 0 };
	ap.ssid          = ssid_buf;
	ap.channel       = 6u; /* common 2.4 GHz default */
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

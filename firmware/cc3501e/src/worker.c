/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: async-job worker (submit/poll/drain).
 *
 * The de-risked seam that keeps the SPI ISR fast (see worker.h): the ISR
 * SUBMITS a job + POLLS its cached result; the slow, possibly-blocking HAL
 * body runs OUTSIDE the ISR in worker_run_pending() (the drain), called
 * from main()'s loop / bringup_task.  A single in-flight job suffices for
 * v0.2.
 *
 * This TU is SILICON-FREE -- it pulls in NO TI SDK; the blocking bodies
 * are the cc3501e_hw_* HAL shims.  Concurrency with the SPI ISR is handled
 * two ways:
 *   1. The shared `job` struct is `volatile`, and `state` is the SINGLE
 *      synchronisation variable.  The drain and the ISR are ordered around
 *      it: result[]/result_len/err are only WRITTEN while state is QUEUED/
 *      RUNNING (the ISR never reads them then -- it sees < DONE and replies
 *      BUSY), and only READ once state == DONE/ERR (the drain never writes
 *      them then).  `state` is the last field written when publishing a
 *      result and the first checked when polling -- a release/acquire-style
 *      ordering that the short critical section below makes atomic.
 *   2. The multi-field publish (write result THEN flip state to DONE) and
 *      the poll's read-then-reset are wrapped in worker_critical_enter/
 *      exit -- weak no-ops here (correct for the single-threaded native
 *      build) that the ti backend overrides with __disable_irq/restore so
 *      the SPI ISR cannot observe a half-updated job on real silicon.
 */

#include <string.h>

#include "worker.h"
#include "transport.h" /* bridge_transport_spi_hw_reinit -- post-radio-op SPI re-sync */
#include "../hal/cc3501e_hw.h"

/* ---- ISR-vs-drain mutual exclusion (weak; ti backend overrides) ---- *
 * Default no-ops are correct for the host/native build (single-threaded,
 * no ISR).  hal/ti/cc3501e_hw_ti.c provides strong versions that mask
 * interrupts (__disable_irq / restore PRIMASK) so the SPI ISR cannot read
 * a half-published result on silicon.  Kept here (not in transport.h) so
 * the worker owns its own synchronisation contract. */
__attribute__((weak)) unsigned long worker_critical_enter(void)
{
	return 0u;
}

__attribute__((weak)) void worker_critical_exit(unsigned long key)
{
	(void)key;
}

/* The single in-flight job + its cached result.  `volatile`: written by
 * the drain (worker_run_pending / synchronous worker_submit), read by the
 * SPI ISR (worker_poll). */
static struct {
	volatile enum worker_state state;
	volatile uint8_t           job_cmd;
	volatile uint8_t           result[ALP_CC3501E_MAX_PAYLOAD];
	volatile uint16_t          result_len;
	volatile int8_t            err;
	/* Request payload for jobs that carry one (WIFI_CONNECT_STA / WIFI_AP_START).
	 * Parameterless jobs (GET_MAC / scan / ble) leave req_len 0.  Written by the
	 * ISR in worker_submit_payload while state goes IDLE->QUEUED; read by the drain
	 * while RUNNING -- the same state-ordered hand-off as result[]. */
	volatile uint8_t  req[ALP_CC3501E_MAX_PAYLOAD];
	volatile uint16_t req_len;
} job;

/* Run the blocking HAL body for @p cmd and publish DONE/ERR.  Called from
 * the drain (RUNNING) and from the synchronous stub path in worker_submit.
 * NOT ISR context -- may block.  v0.2 services only GET_MAC; future
 * opcodes add cases here (each a fast-failing default keeps it honest). */
static void worker_execute(uint8_t cmd)
{
	uint8_t buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t  len = 0u;
	int     rv  = CC3501E_HW_ERR_NOTIMPL;

	switch (cmd) {
	case ALP_CC3501E_CMD_GET_MAC: {
		uint8_t mac[6];
		rv = cc3501e_hw_get_mac(mac); /* WiFi build: lazy-inits, then Wlan_Get (blocks) */
		if (rv == CC3501E_HW_OK) {
			memcpy(buf, mac, 6u);
			len = 6u;
		}
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_RSSI: {
		int8_t r = 0;
		rv       = cc3501e_hw_wifi_get_rssi(
		    &r); /* WiFi build: lazy-starts radio, then Wlan_Get (blocks) */
		if (rv == CC3501E_HW_OK) {
			buf[0] = (uint8_t)r;
			len    = 1u;
		}
		break;
	}
	case ALP_CC3501E_CMD_WIFI_SCAN_START:
		/* Packs the AP-record list into buf in the host's wire format (see
		 * cc3501e_hw_wifi_scan); blocks on the scan + event rendezvous. */
		rv = cc3501e_hw_wifi_scan(buf, ALP_CC3501E_MAX_PAYLOAD, &len);
		break;
	case ALP_CC3501E_CMD_BLE_ENABLE:
		/* Wi-Fi-first (shared HIF) then nimble_host_start -- blocks ~2s, so it
		 * is worker-routed off the SPI ISR exactly like GET_MAC.  Argless: an
		 * OK result carries no payload (len stays 0). */
		rv = cc3501e_hw_ble_enable();
		break;
	case ALP_CC3501E_CMD_BLE_SCAN_START:
		/* Packs the discovered-advertiser list into buf (see cc3501e_hw_ble_scan);
		 * runs a NimBLE GAP discovery that blocks for the scan window, so it is
		 * worker-routed off the SPI ISR exactly like WIFI_SCAN_START. */
		rv = cc3501e_hw_ble_scan(buf, ALP_CC3501E_MAX_PAYLOAD, &len);
		break;
	case ALP_CC3501E_CMD_WIFI_CONNECT_STA:
	case ALP_CC3501E_CMD_WIFI_AP_START: {
		/* Association BLOCKS until the connect/IP event (seconds), so -- unlike the
		 * other Wlan_* ops which were already worker-routed -- this MUST run here in
		 * the drain, not in protocol_dispatch's SPI-ISR context (a blocking wait in
		 * the ISR either hung the bridge or could not pend at all -> the -4/-2
		 * connect failures).  The request payload (alp_cc3501e_wifi_connect_t header
		 * + inline ssid + psk) was stashed in job.req by worker_submit_payload; its
		 * length was already validated by the protocol handler before submit. */
		const alp_cc3501e_wifi_connect_t *c =
		    (const alp_cc3501e_wifi_connect_t *)(const void *)job.req;
		const uint8_t *ssid = (const uint8_t *)job.req + sizeof(*c);
		const uint8_t *psk  = ssid + c->ssid_len;
		rv = (cmd == ALP_CC3501E_CMD_WIFI_AP_START)
		         ? cc3501e_hw_wifi_ap_start(ssid, c->ssid_len, psk, c->psk_len, c->security)
		         : cc3501e_hw_wifi_connect_sta(ssid, c->ssid_len, psk, c->psk_len, c->security);
		break;
	}
	default:
		rv = CC3501E_HW_ERR_NOTIMPL;
		break;
	}

	/* Publish the result atomically wrt the SPI ISR: fill result[] first,
	 * then flip state LAST so a poller never sees DONE with stale bytes. */
	const unsigned long key = worker_critical_enter();
	if (rv == CC3501E_HW_OK) {
		memcpy((void *)job.result, buf, len);
		job.result_len = (uint16_t)len;
		job.err        = 0;
		job.state      = WORKER_DONE;
	} else {
		job.result_len = 0u;
		job.err        = (int8_t)rv;
		job.state      = WORKER_ERR;
	}
	worker_critical_exit(key);
}

void worker_init(void)
{
	const unsigned long key = worker_critical_enter();
	job.state               = WORKER_IDLE;
	job.job_cmd             = 0u;
	job.result_len          = 0u;
	job.err                 = 0;
	worker_critical_exit(key);
}

int worker_submit(uint8_t cmd)
{
	const unsigned long key = worker_critical_enter();
	if (job.state != WORKER_IDLE) {
		worker_critical_exit(key);
		return 0; /* a job is already in flight (single in-flight, v0.2) */
	}
	job.job_cmd    = cmd;
	job.result_len = 0u;
	job.err        = 0;
	job.state      = WORKER_QUEUED;
	worker_critical_exit(key);

#ifndef CC3501E_WIFI
	/* SILICON-FREE / stub / native backend: no main-loop drain runs in the
	 * host ztests, and the HAL body is NOTIMPL (non-blocking).  Run the job
	 * SYNCHRONOUSLY here so the result is ready on the caller's next poll --
	 * the host re-issues GET_MAC once and gets the (NOT_READY) answer.  The
	 * REAL (CC3501E_WIFI) build leaves the job QUEUED for worker_run_pending,
	 * so the SPI ISR is never blocked by the seconds-long Wlan_* body. */
	job.state = WORKER_RUNNING;
	worker_execute(cmd);
#endif
	return 1;
}

int worker_submit_payload(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
	if (len > ALP_CC3501E_MAX_PAYLOAD) {
		return 0; /* would overflow job.req -- caller validated, but stay defensive */
	}
	const unsigned long key = worker_critical_enter();
	if (job.state != WORKER_IDLE) {
		worker_critical_exit(key);
		return 0; /* a job is already in flight (single in-flight, v0.2) */
	}
	job.job_cmd    = cmd;
	job.result_len = 0u;
	job.err        = 0;
	if (len > 0u && payload != NULL) {
		memcpy((void *)job.req, payload, len);
	}
	job.req_len = len;
	job.state   = WORKER_QUEUED;
	worker_critical_exit(key);

#ifndef CC3501E_WIFI
	/* Same synchronous stub path as worker_submit (see there). */
	job.state = WORKER_RUNNING;
	worker_execute(cmd);
#endif
	return 1;
}

enum worker_state
worker_poll(uint8_t cmd, uint8_t *out, size_t out_cap, size_t *out_len, int8_t *err)
{
	if (out_len != NULL) *out_len = 0u;
	if (err != NULL) *err = 0;

	const unsigned long     key = worker_critical_enter();
	const enum worker_state st  = job.state;

	/* No job, or a job for a DIFFERENT cmd: report IDLE so the caller can
	 * submit (or, if a different job is mid-flight, treat it as busy). */
	if (st == WORKER_IDLE || job.job_cmd != cmd) {
		const enum worker_state ret = (job.job_cmd != cmd && st != WORKER_IDLE)
		                                  ? WORKER_RUNNING /* other cmd busy */
		                                  : WORKER_IDLE;
		worker_critical_exit(key);
		return ret;
	}

	if (st == WORKER_DONE) {
		const size_t n = (out_cap < job.result_len) ? out_cap : job.result_len;
		if (out != NULL && n > 0u) memcpy(out, (const void *)job.result, n);
		if (out_len != NULL) *out_len = n;
		worker_critical_exit(key);
		return WORKER_DONE;
	}
	if (st == WORKER_ERR) {
		if (err != NULL) *err = job.err;
		worker_critical_exit(key);
		return WORKER_ERR;
	}

	/* QUEUED or RUNNING: still in flight. */
	worker_critical_exit(key);
	return st;
}

void worker_reset(void)
{
	const unsigned long key = worker_critical_enter();
	job.state               = WORKER_IDLE;
	job.job_cmd             = 0u;
	job.result_len          = 0u;
	job.err                 = 0;
	worker_critical_exit(key);
}

void worker_run_pending(void)
{
	/* Promote QUEUED -> RUNNING atomically so the ISR can't double-submit
	 * while we execute; the long HAL body then runs OUTSIDE the lock. */
	const unsigned long key = worker_critical_enter();
	uint8_t             cmd = 0u;
	int                 go  = 0;
	if (job.state == WORKER_QUEUED) {
		job.state = WORKER_RUNNING;
		cmd       = job.job_cmd;
		go        = 1;
	}
	worker_critical_exit(key);

	if (go) {
		worker_execute(cmd); /* may block for seconds (Wlan_* init + get) */

		/* Radio<->SPI coexistence fix: the worker body just ran a radio HAL op
		 * (a Wlan_* call), during which the bridge SPI slave could not be
		 * serviced -- so the host may have read 0x00000000 / drifted byte
		 * alignment.  Request a clean slave re-sync now that the op is done and
		 * BEFORE the next host poll lands (worker_execute has already published
		 * DONE/ERR above, but the host re-issues GET_MAC to collect it).  This is
		 * the weak transport hook: a no-op on the stub/native build (no radio,
		 * no SPI slave), the real FIFO-flush + poll-loop re-arm on the ti
		 * backend (hal/ti/transport_hw_ti_spi.c). */
		bridge_transport_spi_hw_reinit();
	}
}

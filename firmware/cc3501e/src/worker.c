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

/* ---- Bridge READY/host-IRQ line (weak; ti backend drives a real GPIO) ---- *
 * The transport's flow-control to the host: a radio op kills the SPI-slave DMA
 * (the bridge cannot be serviced while it runs), so the worker drives the line
 * BUSY before every blocking radio op and READY again once the slave is
 * re-armed (after the post-op bridge_transport_spi_hw_reinit).  The host gates
 * its clocking on READY so it never drives a transaction into a dead slave.
 * Default no-ops (host/native build has no host-IRQ line); the ti backend
 * (hal/ti/cc3501e_hw_ti.c) overrides them to drive CC35 GPIO17 (E1M IO16). */
__attribute__((weak)) void cc3501e_bridge_busy(void)
{
}

__attribute__((weak)) void cc3501e_bridge_ready(void)
{
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

/* Little-endian store helper for the socket reply wire (parallels protocol.c). */
static void wk_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* Read a 16-bit LE field out of the (volatile) request buffer byte-by-byte. */
static uint16_t wk_get_le16(const volatile uint8_t *p, size_t off)
{
	return (uint16_t)p[off] | ((uint16_t)p[off + 1u] << 8);
}

/* recv reply header = alp_cc3501e_sock_recv_resp_t: from(sock_addr 20) |
 * data_len(LE16) | reserved(LE16).  The received bytes follow inline. */
#define WK_SOCK_RECV_HDR 24u

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
	case ALP_CC3501E_CMD_BLE_ADV_START: {
		/* Ext-adv config+start BLOCKS on the shared-HIF HCI ack (2 s), so -- like
		 * the connect ops -- it MUST run here in the drain, not the SPI ISR (that
		 * was the adv-wedge -4).  The 7-byte header (connectable | reserved |
		 * itvl_min LE16 | itvl_max LE16 | adv_data_len) + inline adv_data were
		 * stashed in job.req by worker_submit_payload; the protocol handler already
		 * length-validated them.  Argless reply (OK carries no payload). */
		const uint8_t  connectable  = (uint8_t)job.req[0];
		const uint16_t itvl_min     = (uint16_t)job.req[2] | ((uint16_t)job.req[3] << 8);
		const uint16_t itvl_max     = (uint16_t)job.req[4] | ((uint16_t)job.req[5] << 8);
		const uint8_t  adv_data_len = (uint8_t)job.req[6];
		rv                          = cc3501e_hw_ble_adv_start(
		    connectable, itvl_min, itvl_max, (const uint8_t *)job.req + 7, adv_data_len);
		break;
	}
	case ALP_CC3501E_CMD_BLE_ADV_STOP:
		/* Stops the adv set; issues HCI over the shared HIF + re-syncs the bridge
		 * SPI (blocks), so it is worker-routed off the SPI ISR.  Argless. */
		rv = cc3501e_hw_ble_adv_stop();
		break;
	case ALP_CC3501E_CMD_BLE_DISABLE:
		/* Tears down adv+scan via NimBLE; issues HCI over the shared HIF + re-syncs
		 * the bridge SPI (blocks), so it is worker-routed off the SPI ISR.  Argless. */
		rv = cc3501e_hw_ble_disable();
		break;
	case ALP_CC3501E_CMD_BLE_CONNECT:
		/* GAP connect blocks on the connection-complete HCI event over the shared
		 * HIF, so it is worker-routed off the SPI ISR.  Payload = addr_type(1) |
		 * addr[6], stashed in job.req by worker_submit_payload (validated by the
		 * protocol handler).  Argless reply. */
		rv = cc3501e_hw_ble_connect((uint8_t)job.req[0], (const uint8_t *)job.req + 1);
		break;
	case ALP_CC3501E_CMD_BLE_GATT_REGISTER: {
		/* Registers the attribute table (blocks in ble_gatts_start), so it is worker-
		 * routed off the SPI ISR.  Payload = the descriptor (job.req_len bytes, already
		 * validated by protocol_ble.c).  Reply = status(1) | num_handles(1) |
		 * attr_handle(LE16)*num_handles -- see the wire-format doc block in
		 * <alp/protocol/cc3501e.h> and handle_worker_routed_payload_reply. */
		uint16_t handles[ALP_CC3501E_BLE_GATT_MAX_CHARS];
		uint16_t num_handles = 0u;
		rv                   = cc3501e_hw_ble_gatt_register((const uint8_t *)job.req,
		                                                    job.req_len,
		                                                    handles,
		                                                    ALP_CC3501E_BLE_GATT_MAX_CHARS,
		                                                    &num_handles);
		if (rv == CC3501E_HW_OK) {
			buf[0] = 0u; /* status: OK (the frame-level resp is authoritative; this mirrors it) */
			buf[1] = (uint8_t)num_handles;
			for (uint16_t i = 0u; i < num_handles; i++) {
				wk_put_le16(&buf[2u + 2u * i], handles[i]);
			}
			len = 2u + 2u * (size_t)num_handles;
		}
		break;
	}
	case ALP_CC3501E_CMD_BLE_GATT_NOTIFY: {
		/* Pushes a notification (blocks on HCI over the shared HIF), so it is
		 * worker-routed off the SPI ISR.  Payload = handle(LE16) | data[job.req_len-2]. */
		const uint16_t handle = (uint16_t)job.req[0] | ((uint16_t)job.req[1] << 8);
		rv                    = cc3501e_hw_ble_gatt_notify(
		    handle, (const uint8_t *)job.req + 2, (uint16_t)(job.req_len - 2u));
		break;
	}
	case ALP_CC3501E_CMD_BLE_GATT_WRITE: {
		/* GATT write (blocks on HCI over the shared HIF), so it is worker-routed off
		 * the SPI ISR.  Payload = handle(LE16) | data[job.req_len-2]. */
		const uint16_t handle = (uint16_t)job.req[0] | ((uint16_t)job.req[1] << 8);
		rv                    = cc3501e_hw_ble_gatt_write(
		    handle, (const uint8_t *)job.req + 2, (uint16_t)(job.req_len - 2u));
		break;
	}
	case ALP_CC3501E_CMD_BLE_GATT_READ: {
		/* GATT read (blocks on the read-response HCI over the shared HIF), so it is
		 * worker-routed off the SPI ISR.  Payload = handle(LE16); the attribute value
		 * is packed into buf and published so the payload+reply worker path copies it
		 * back to the host (see handle_worker_routed_payload_reply). */
		const uint16_t handle  = (uint16_t)job.req[0] | ((uint16_t)job.req[1] << 8);
		uint16_t       out_len = 0u;
		rv = cc3501e_hw_ble_gatt_read(handle, buf, (uint16_t)ALP_CC3501E_MAX_PAYLOAD, &out_len);
		if (rv == CC3501E_HW_OK) {
			len = out_len;
		}
		break;
	}
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
	case ALP_CC3501E_CMD_SOCK_OPEN: {
		/* job.req = alp_cc3501e_sock_open_t: family(0) | type(1) | protocol(2) |
		 * reserved(3).  Reply DATA = alp_cc3501e_sock_handle_t: handle(LE16) |
		 * reserved(2). */
		uint16_t handle = 0u;
		rv              = cc3501e_hw_sock_open(job.req[0], job.req[1], job.req[2], &handle);
		if (rv == CC3501E_HW_OK) {
			wk_put_le16(buf, handle);
			buf[2] = 0u;
			buf[3] = 0u;
			len    = 4u;
		}
		break;
	}
	case ALP_CC3501E_CMD_SOCK_CONNECT: {
		/* job.req = alp_cc3501e_sock_connect_t: handle(LE16 @0) | reserved(@2) |
		 * peer sock_addr @4 { family(@4) | reserved(@5) | port(LE16 @6) |
		 * addr[16] @8 }.  v1 IPv4: only addr[8..11] are meaningful. */
		const uint16_t handle = wk_get_le16(job.req, 0u);
		const uint8_t  family = job.req[4];
		const uint16_t port   = wk_get_le16(job.req, 6u);
		uint8_t        addr[4];
		for (unsigned i = 0u; i < 4u; ++i)
			addr[i] = job.req[8u + i];
		rv = cc3501e_hw_sock_connect(handle, family, port, addr);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_SEND: {
		/* job.req = alp_cc3501e_sock_send_t: handle(LE16 @0) | flags(@2) |
		 * reserved(@3) | data_len(LE16 @4) | reserved2(@6) | data @8.  Reply
		 * DATA = uint16_t LE byte count actually queued. */
		const uint16_t handle   = wk_get_le16(job.req, 0u);
		const uint8_t  flags    = job.req[2];
		const uint16_t data_len = wk_get_le16(job.req, 4u);
		uint16_t       sent     = 0u;
		/* The data rides inline in job.req at offset 8 (the handler validated
		 * req_len == 8 + data_len <= MAX_PAYLOAD); pass it in place, mirroring how
		 * the WIFI_CONNECT case reads ssid/psk straight out of job.req. */
		rv = cc3501e_hw_sock_send(handle, flags, (const uint8_t *)&job.req[8], data_len, &sent);
		if (rv == CC3501E_HW_OK) {
			wk_put_le16(buf, sent);
			len = 2u;
		}
		break;
	}
	case ALP_CC3501E_CMD_SOCK_RECV: {
		/* job.req = alp_cc3501e_sock_recv_t: handle(LE16 @0) | max_len(LE16 @2).
		 * Reply = recv_resp header (WK_SOCK_RECV_HDR) + up to max_len bytes.  Cap
		 * the data so header + data + the status byte fit one frame. */
		const uint16_t handle       = wk_get_le16(job.req, 0u);
		const uint16_t max_len      = wk_get_le16(job.req, 2u);
		const uint16_t data_cap     = (uint16_t)(ALP_CC3501E_MAX_PAYLOAD - WK_SOCK_RECV_HDR - 1u);
		uint8_t        from_addr[4] = { 0 };
		uint16_t       from_port    = 0u;
		uint16_t       recv_len     = 0u;
		rv                          = cc3501e_hw_sock_recv(
		    handle, max_len, &buf[WK_SOCK_RECV_HDR], data_cap, &recv_len, from_addr, &from_port);
		if (rv == CC3501E_HW_OK) {
			/* Build the from sock_addr (family | reserved | port(LE16) | addr[16]);
			 * STREAM leaves it zeroed (recv fills only for DGRAM). */
			memset(buf, 0, WK_SOCK_RECV_HDR);
			buf[0] = (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4;
			wk_put_le16(&buf[2], from_port);
			for (unsigned i = 0u; i < 4u; ++i)
				buf[4u + i] = from_addr[i];
			wk_put_le16(&buf[20], recv_len); /* data_len */
			len = (size_t)WK_SOCK_RECV_HDR + (size_t)recv_len;
		}
		break;
	}
	case ALP_CC3501E_CMD_SOCK_CLOSE: {
		/* job.req = alp_cc3501e_sock_close_t: handle(LE16 @0) | reserved(@2). */
		rv = cc3501e_hw_sock_close(wk_get_le16(job.req, 0u));
		break;
	}
	default:
		rv = CC3501E_HW_ERR_NOTIMPL;
		break;
	}

	/* Defensive clamp before the publish memcpy: every HAL body above is
	 * CONTRACTED to report len <= ALP_CC3501E_MAX_PAYLOAD (it fills buf,
	 * which is exactly that size), but a misbehaving backend that reports
	 * a larger len must corrupt at most its own answer -- never overrun
	 * job.result[] and smash the worker state the SPI ISR reads.
	 * Truncation is safe to publish: the poller copies min(out_cap,
	 * result_len) and the protocol layer length-checks every reply. */
	if (len > sizeof(job.result)) {
		len = sizeof(job.result);
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
		cc3501e_bridge_busy(); /* radio op about to kill the slave DMA -> hold host off */
		worker_execute(cmd);   /* may block for seconds (Wlan_* init + get) */

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

		/* CONNECT / AP_START are FIRE-AND-FORGET at the worker level: their outcome
		 * is mirrored into the HAL connection-status latch (read NON-blocking by
		 * CMD_WIFI_STATUS), so the host never collects their DONE/ERR through this
		 * single-job slot.  Free the slot to IDLE here so a SUBSEQUENT connect can
		 * submit -- otherwise the slot would stay DONE/ERR and the next CONNECT would
		 * re-collect the stale result instead of starting a fresh association.  This
		 * MUST happen BEFORE cc3501e_bridge_ready() below: once READY is HIGH the host
		 * may clock a transaction, and a second CONNECT landing while the slot still
		 * held this attempt's DONE/ERR would be collected as the new submit (returning
		 * RESP_OK off the stale result, skipping mark_connecting -> a stale latch and
		 * NO fresh association).  Resetting first makes the slot IDLE the instant the
		 * host is allowed to clock, so any next CONNECT hits the IDLE edge (fresh
		 * submit + mark_connecting).  All the other worker-routed ops (GET_MAC / SCAN /
		 * RSSI / BLE) stay poll-by-repeat: the host collects their DONE/ERR, which
		 * resets the slot in protocol.c (handle_worker_routed). */
		if (cmd == ALP_CC3501E_CMD_WIFI_CONNECT_STA || cmd == ALP_CC3501E_CMD_WIFI_AP_START) {
			worker_reset();
		}
		cc3501e_bridge_ready(); /* slave re-armed -> host may clock again */
	}
}

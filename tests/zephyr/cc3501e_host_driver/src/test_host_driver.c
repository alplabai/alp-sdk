/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic host-side tests for the CC3501E companion wrappers that the
 * OTA suite (tests/zephyr/cc3501e_host_ota) does not cover: the Wi-Fi,
 * BLE, socket, GPIO-proxy, power, and diagnostics helpers in
 * chips/cc3501e/cc3501e.c.  They drive the REAL host driver -- its request
 * ENCODE (opcode + payload byte layout) and its reply DECODE (struct field
 * extraction) -- against a software model of the firmware SPI slave.
 *
 * The model lives entirely in this test's alp_spi_transceive() stub (the
 * one seam that carries the wire contract): it plays the firmware slave in
 * the CS-less 3-wire lockstep (request header -> request payload -> reply
 * header -> reply payload), records the exact bytes the host EMITS for
 * each opcode, and stages a deterministic reply the host then DECODES back.
 * No TI silicon, no Zephyr SPI backend, no radio -- just the driver and the
 * wire format from <alp/protocol/cc3501e.h>.
 *
 * Wire framing mirrors <alp/protocol/cc3501e.h> and the firmware transport
 * (cc3501e-bridge-firmware:hal/ti/transport_hw_ti_spi.c): a 4-byte LE header
 * [cmd | flags | payload_len(LE16)] then payload; the reply header echoes
 * the request cmd and declares the reply payload length; the reply
 * payload's first byte is the response status (ALP_CC3501E_RESP_*).
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"
#include "cc3501e_reply_model.h"

/* #2035: what a real MAJOR-4 firmware actually reports as its SPI1 chunk cap
 * in the SPI1_CONFIGURE reply -- ALP_CC3501E_SPI1_MAX_XFER (4088) minus the
 * MAJOR-4 CRC trailer cc3501e_request() appends, i.e. CC3501E_SPI1_MAX_XFER_V4
 * from the firmware's own protocol_meta.c.  This suite otherwise models
 * MAJOR-4 replies throughout (see cc3501e_reply_model.h), so staging the bare
 * macro here instead of the real reported value was the one inconsistent
 * field -- see test_spi1_configure_encodes_request_and_decodes_reply below. */
#define CC3501E_SPI1_MAX_XFER_V4 ((uint16_t)(ALP_CC3501E_SPI1_MAX_XFER - ALP_CC3501E_CRC_BYTES))

/* ---- software model of the firmware slave ---------------------------------- */

enum slave_phase {
	PH_REQ_HDR = 0, /* next transfer is a 4-byte request header   */
	PH_REQ_PL,      /* next transfer is the request payload       */
	PH_REPLY_HDR,   /* host reads the 4-byte reply header         */
	PH_REPLY_PL,    /* host reads the reply payload (status+data) */
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;     /* opcode of the in-flight request (0 = none clocked) */
	uint8_t          flags;   /* header flags byte of the in-flight request (bits 3..7
	                          * carry the proto v8 retry seq) -- alp-sdk#2108's SOCK_RECV
	                          * ring model reads this to key its replay cache, since
	                          * flags_log below only ever holds HISTORY, not the current
	                          * in-flight request a dispatch function is deciding for. */
	uint16_t         req_len; /* declared request payload length                    */
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD]; /* captured request payload    */

	/* Staged reply (built at request completion, drained over phases 3+4). */
	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* status byte + data */
	uint16_t reply_len;                         /* == 1 + data bytes  */

	/* A tiny in-RAM pin model so GPIO configure -> write -> read round-trips
	 * through the real wire encode/decode, like the firmware stub HAL. */
	uint8_t pin_level[64];

	/* ---- fire-and-forget WIFI_CONNECT_STA / WIFI_STATUS model (#1376/#1377/
	 * #1378) -----------------------------------------------------------------
	 * A dedicated snapshot of the CONNECT_STA submit's own request bytes,
	 * because the generic cmd/req_len/req_pl above reflect whichever request
	 * was dispatched LAST -- cc3501e_wifi_connect() now issues the submit
	 * once and then polls WIFI_STATUS afterwards, so by the time it returns
	 * the generic fields hold WIFI_STATUS's (empty) request, not the
	 * connect's. */
	uint16_t connect_last_req_len;
	uint8_t  connect_last_req_pl[ALP_CC3501E_MAX_PAYLOAD];
	uint32_t connect_submit_count; /* how many CONNECT_STA submits landed */

	/* Same snapshot for WIFI_AP_START (#1385): AP_START is worker-routed
	 * through the IDENTICAL firmware handler as CONNECT_STA
	 * (handle_worker_routed_payload) and its host wrapper retries, so the
	 * generic req_pl/req_len hold whichever attempt landed LAST. */
	uint16_t ap_start_last_req_len;
	uint8_t  ap_start_last_req_pl[ALP_CC3501E_MAX_PAYLOAD];
	uint32_t ap_start_submit_count; /* how many AP_START submits landed */

	/* The WIFI_STATUS latch WIFI_CONNECT_STA drives + WIFI_STATUS reads --
	 * models the firmware's async connect-status latch (handle_wifi_status /
	 * cc3501e_hw_wifi_conn_status). */
	uint8_t wifi_conn_state;
	uint8_t wifi_fail_reason;
	int8_t  wifi_conn_rssi;
	/* Wire byte 3 of the WIFI_STATUS reply, decoded into
	 * alp_cc3501e_wifi_status_t::last_reason -- the IEEE 802.11 reason/status
	 * code (formerly an unused `reserved` byte, always 0). */
	uint8_t wifi_last_reason;

	/* Number of WIFI_STATUS polls that must still read CONNECTING before the
	 * latch above is reported -- simulates an association that takes a few
	 * polls to resolve, so a regression test can prove cc3501e_wifi_connect()
	 * polls WIFI_STATUS repeatedly while submitting CONNECT_STA exactly once. */
	uint32_t status_polls_before_terminal;

	/* Number of WIFI_GET_RSSI submits that must still ack RESP_ERR_BUSY
	 * before the real value is handed back -- models GET_RSSI's real
	 * worker-routed submit/collect shape (#1377). */
	uint32_t rssi_busy_polls_remaining;

	/* Every WIFI_STATUS request-header phase clocked, fault-injected or not
	 * -- lets a test count how many WIFI_STATUS attempts
	 * cc3501e_wifi_connect()'s own poll loop made for a given timeout_ms
	 * (#1382 timeout-accounting regression). */
	uint32_t wifi_status_attempt_count;

	/* #1435 entry-clean ordering: every opcode dispatched, in order.
	 * slave.cmd alone only ever holds the LAST opcode dispatched, which
	 * cannot prove WIFI_DISCONNECT landed BEFORE WIFI_CONNECT_STA -- this
	 * log can. Capacity is generous for one cc3501e_wifi_connect() call's
	 * worth of traffic; entries past capacity are dropped (cmd_log_count
	 * still counts them) but no #1435 test drives that many. */
	uint8_t  cmd_log[16];
	uint32_t cmd_log_count;

	/* Request FLAGS byte of every request-header phase clocked, in order
	 * (proto v8): bits 3..7 carry the retry seq, and the property under test
	 * is that it stays CONSTANT across the retries of one logical command and
	 * CHANGES between commands.  slave.cmd cannot show either -- it holds one
	 * opcode with no attempt history -- and cmd_log holds opcodes, not flags.
	 * Same capacity + drop-past-capacity rule as cmd_log above. */
	uint8_t  flags_log[16];
	uint32_t flags_log_count;

	/* cc3501e-bridge-firmware#107 (alp-sdk#2035) remainder-retry coverage: every
	 * CMD_SOCK_SEND dispatch that resolves DEFINITIVELY (RESP_OK -- a BUSY
	 * reply is a poll_by_repeat-internal retry of the SAME iteration, not a
	 * new one, and is deliberately NOT logged here, so one log entry always
	 * means one iteration) logs its payload seq (byte [3], NOT the
	 * wire-header flags -- see cc3501e_sock_send()'s seq comment), declared
	 * data_len (bytes [4..5]), and the inline data bytes themselves.
	 * slave.req_pl/req_len alone only ever hold the LAST dispatch, which
	 * cannot prove a multi-iteration send used a DISTINCT seq per iteration
	 * with only the UNSENT TAIL of the original buffer -- this log can (the
	 * data_len-only version of this log let a `remaining += queued` deletion
	 * stay green, since two iterations offering the SAME data_len as two
	 * DIFFERENT byte ranges look identical without the bytes themselves).
	 * Same drop-past-capacity rule as cmd_log above. */
	uint8_t  sock_send_seq_log[12];
	uint16_t sock_send_datalen_log[12];
	uint8_t  sock_send_payload_log[12][32]; /* [i] holds min(datalen_log[i], 32) bytes */
	uint32_t sock_send_log_count;

	/* cc3501e-bridge-firmware#107 (alp-sdk#2035) review follow-up: a real
	 * one-worker-slot, opcode-keyed model (see the g_sock_send_worker_*
	 * globals below for the full shape) needs its own "did the job body run"
	 * counter -- proving a collecting re-poll REVEALS an already-computed
	 * result rather than re-executing it (the firmware only ever calls
	 * lwip_send() ONCE per accepted job). */
	uint32_t sock_send_body_exec_count;
} slave;

/* Set by test_wifi_scan_buf_is_per_context_740 / test_ble_scan_buf_is_per_context_740
 * to pick the second context's distinct staged reply from slave_dispatch(); cleared by
 * slave_reset() so every other test keeps seeing the default fixtures. */
static bool g_scan_stage_ctx_b;

/* #1378 mutant control: when true, the WIFI_CONNECT_STA / WIFI_AP_START
 * submit handler stages a literal ALP_CC3501E_RESP_OK_LEGACY (0x00) status
 * byte instead of the real firmware's unconditional RESP_ERR_BUSY submit ack
 * -- reproducing "a valid header followed by an all-zero payload phase"
 * (this repo's own silicon finding: a dead bus phase reads back
 * 0x00000000).  MUST stay the LEGACY 0x00 value, not ALP_CC3501E_RESP_OK
 * (0x5A since wire MAJOR 4, #2035): the dead-phase byte a broken link
 * actually clocks back never changed, only which status code means success
 * did -- staging the new RESP_OK here would model a real, CRC-verifiable
 * success reply, not the all-zero alias this control exists to reproduce.
 * Cleared by slave_reset(). */
static bool g_connect_submit_force_ok;
/* Radio role GET_DIAG_INFO reports.  cc3501e_wifi_ap_start() confirms its
 * submit against this field (#1696), so a test can drive the AP up by setting
 * it to ALP_CC3501E_ROLE_WIFI_AP.  Defaults to STA = 'AP not up'. */
static uint8_t g_diag_role = ALP_CC3501E_ROLE_WIFI_STA;

/* Bilingual-decode control: when true, the shared bare-OK bucket in
 * slave_dispatch() stages ALP_CC3501E_RESP_OK_LEGACY (0x00, unpadded, no
 * CRC) instead of ALP_CC3501E_RESP_OK -- the shape a real pre-4.0 (MAJOR 3)
 * firmware actually sends.  Cleared by slave_reset(). */
static bool g_bare_ok_legacy;

/* FLASH-derived pending image reported in OTA_STATUS byte [12].
 * cc3501e_ota_promote() refuses to commit unless this says STAGED (#1123),
 * so it defaults to STAGED: the promote tests model a device that really
 * does have an installable image waiting. */
static uint8_t g_ota_pending = ALP_CC3501E_OTA_PENDING_STAGED;

/* #1377 mutant control: while > 0, a whole WIFI_STATUS transaction's REQUEST
 * HEADER phase fails outright (as if the shared bridge transport itself were
 * down, e.g. a radio op in flight) -- decremented per attempt, letting a
 * test prove cc3501e_wifi_status() rides the down-window out.  The failure
 * is injected before slave.phase advances, so the next attempt starts clean
 * (models a transport-level fault, not a framing desync needing a resync).
 * Cleared by slave_reset(). */
static uint32_t g_status_io_down_remaining;

/* #1371 mutant controls for cc3501e_reset()'s wire-protocol compatibility
 * gate.  Both cleared by slave_reset() so every other test keeps seeing the
 * default (matching-version, answers-immediately) fixture. */
static bool     g_get_version_override_active; /* stage a specific reply value below */
static uint16_t g_get_version_override_value;
static uint32_t g_get_version_io_down_remaining; /* fail the transaction outright, N times */

/* #2126 mutant control: while true, EVERY request header phase fails
 * outright, regardless of opcode -- models a genuinely dead bridge link (no
 * PING, no anything, ever answers), unlike the narrower per-opcode down-
 * windows above. Cleared by slave_reset(). Healed by alp_gpio_write()'s own
 * fake below the moment it sees the ctx's OWN reset_pin released HIGH --
 * i.e. the model treats a warm nRESET pulse (cc3501e_hard_reset()) as what
 * actually cures a #2126 wedge, the same bench-established fact
 * cc3501e_recover()'s own doc comment records, so a test staging this can
 * assert BOTH that recovery was attempted (the link healed) and that it was
 * attempted at most once (the cooldown -- see ctx->recover_count). */
static bool g_all_io_down;

/* #2136 mutant controls: force the two wire patterns the link-failure ring
 * must tell apart (test_link_log_distinguishes_deaf_armed_from_desynced_2136).
 * Cleared by slave_reset(). */
static bool g_req_hdr_desynced; /* PH_REQ_HDR MISO reads 0x00 x4, not the armed
                                 * ALP_CC3501E_SYNC_IDLE x4 -- a slave mid-payload,
                                 * genuinely desynced rather than merely unarmed. */
static bool g_reply_hdr_deaf;   /* PH_REPLY_HDR never echoes the request opcode -- stays
                                 * ALP_CC3501E_SYNC_IDLE x4, i.e. the slave armed the
                                 * header phase but never dispatched a reply at all. */

/* #2126 review follow-up mutant controls. Cleared by slave_reset() (bool/u32
 * ones) -- g_deaf_from_ms/g_deaf_until_ms are timestamps, meaningless to
 * reset to 0 (that would just mean "deaf from boot", not "never deaf"), so
 * each test that uses them sets AND clears both itself. */
/* A warm nRESET does NOT cure the wedge -- see alp_gpio_write()'s own
 * healing hook below. Models a fault a reset genuinely cannot fix (vs.
 * g_all_io_down's default healing model of the #1691 wedge, which every
 * bench observation says a warm reset DOES cure). */
static bool g_heal_disabled;
/* CMD_BLE_ENABLE always answers a genuine DECODED RESP_ERR_BUSY (worker
 * still running) regardless of anything else -- models a bridge that is
 * alive and answering, not silent, for the whole span this is set. */
static bool g_ble_enable_busy;
/* Time-windowed silence: every PH_REQ_HDR transceive fails outright (same
 * pre-decode shape as g_all_io_down) ONLY while alp_uptime_ms() is in
 * [g_deaf_from_ms, g_deaf_until_ms) -- models a bounded, legitimate
 * transport blackout (a radio op, a teardown/re-arm race) on an otherwise
 * healthy bridge, as opposed to g_all_io_down's permanent silence. */
static uint64_t g_deaf_from_ms, g_deaf_until_ms;
/* Counts alp_gpio_write(reset_pin, true) calls -- the nRESET RELEASE edge,
 * i.e. how many times cc3501e_hard_reset() actually pulsed the line. The
 * direct, unambiguous proxy for "how many real warm resets happened",
 * independent of what cc3501e_recover()'s own bookkeeping (recover_count,
 * which only counts SUCCESSFUL recoveries) can prove on its own. */
static uint32_t g_reset_release_count;
/* CMD_PING always fails outright at the transport level -- independent of
 * g_all_io_down, so a test can make ONE specific opcode (e.g. CMD_BLE_ENABLE
 * via g_ble_enable_busy) answer a genuine DECODED status while the
 * link-check probe's OWN PING still cannot succeed, giving an incorrectly
 * fired trigger somewhere to actually reach cc3501e_recover() and bump
 * recover_attempt_count -- see test_poll_by_repeat_busy_persists_no_
 * recovery_2126's own comment for why that distinction matters. */
static bool g_ping_always_fails;

/* ADR 0033 mutant controls for CMD_GET_CAPABILITIES (opcode 0x06).  Both
 * cleared by slave_reset(), so a test that never touches them still gets a
 * deterministic (zero) bitmap back rather than whatever the previous test
 * left staged. */
static uint32_t g_caps_override_value; /* bitmap staged for the next GET_CAPABILITIES reply */
static bool     g_caps_reply_short;    /* stage a <4-byte reply -- must decode as ALP_ERR_IO */

/* SPI1 TRANSFER mutant control: when true, the reply echoes seq+1 instead of
 * the request's real seq -- models a desynced/stale reply (the firmware
 * answering some OTHER request) so a test can prove cc3501e_spi1_transfer()
 * treats a seq mismatch as ALP_ERR_IO rather than handing back another
 * transaction's RX bytes.  Cleared by slave_reset(). */
static bool g_spi1_reply_bad_seq;

/* Stage the 16-byte protocol-v8 DIAG_GET_STATS reply instead of the 8-byte v7
 * one.  Cleared by slave_reset(), so the default across the suite is the OLD
 * firmware -- the compatibility direction that would otherwise go untested. */
static bool g_diag_stats_v8;

/* #2039 mutant control for CMD_GET_MAC: flip one bit in byte 0 of the reply,
 * modelling the single-bit corruption a mis-tuned RX sample delay produced on
 * E1M-AEN803 serial 2026W36-0002 (44:3e:8a:.. read back as 46:3e:8a:..).
 * Counts down, so a test can corrupt exactly one read of several.  Cleared by
 * slave_reset(). */
static uint32_t g_get_mac_corrupt_remaining;

/* How many CMD_GET_MAC requests the slave has served.  Proves a repeated
 * cc3501e_wifi_get_mac() is a fresh wire transaction, not a cached reply. */
static uint32_t g_get_mac_serve_count;

/* #2035 follow-up (the coverage gap that let two review findings ship): a
 * dead phase on the LEGACY (wire MAJOR 3) wire for an opcode whose reply
 * carries real data -- a valid reply HEADER followed by an ALL-ZERO payload
 * phase, this repo's own silicon-measured dead-link shape.  When true,
 * slave_dispatch() below stages exactly that (status 0x00 +
 * ALP_CC3501E_REPLY_PAD-1 zero data bytes) for BLE_GATT_REGISTER,
 * WIFI_SCAN_START, and BLE_SCAN_START regardless of their normal fixture --
 * see test_ble_gatt_register_dead_phase_legacy_rejected_2035 and the two
 * scan acceptance tests below it.  Cleared by slave_reset(). */
static bool g_force_dead_phase_zero_payload;

/* #2035 mutant controls for cc3501e_wifi_get_ip()'s ALP_ERR_IO/ALP_ERR_NOT_READY
 * split. Cleared by slave_reset().
 *   - g_get_ip_no_address: the firmware DECODES the request and answers
 *     ALP_CC3501E_RESP_ERR_RADIO -- its real "no address on this interface
 *     yet" status on this opcode -- so ctx->rx_scratch[0] holds a genuine
 *     decoded status byte.  Must map to ALP_ERR_NOT_READY.
 *   - g_get_ip_io_down_remaining: the REQUEST HEADER phase transceive fails
 *     outright, exactly like g_status_io_down_remaining above -- no status is
 *     ever decoded, so rx_scratch[0] is left poisoned to
 *     ALP_CC3501E_RX_SCRATCH_NO_STATUS.  Must stay ALP_ERR_IO -- proves the
 *     fix does not reclassify every failure. */
static bool     g_get_ip_no_address;
static uint32_t g_get_ip_io_down_remaining;

/* #2035 GET_DIAG_INFO reply length dial. The bridge firmware appended
 * dhcp_state (byte 16) + netif_status (byte 17) ADDITIVELY, growing the reply
 * 16 -> 18; this lets a test stage the pre-#2035 16-byte shape (backward
 * compat: must still succeed, new fields read as "not reported") or a
 * malformed <16-byte shape (must fail). Cleared by slave_reset(), so the
 * default across the suite is the full 18-byte reply. */
static uint8_t g_diag_info_reply_len = 18u;

/* #2035 review follow-up: stage the GET_DIAG_INFO reply as a genuinely
 * UNPADDED 16-byte-data wire frame (via cc3501e_model_stage_legacy_reply(),
 * no CRC trailer, no REPLY_PAD rounding) instead of the padded model every
 * other diag_info test uses -- see
 * test_diag_info_legacy_unpadded_16byte_reply_reports_new_fields_not_reported
 * for why this is the ONE shape that actually distinguishes the got<16u
 * guard from a got<18u regression: every reply staged through the padded
 * model (cc3501e_model_stage_reply(), including
 * test_diag_info_16byte_reply_reports_new_fields_not_reported above) rounds
 * up to a 24-byte wire payload regardless of whether 16 or 18 data bytes
 * were staged, so `got` is 18 either way and a got<16u vs got<18u guard is
 * unfalsifiable against it. Cleared by slave_reset(). */
static bool g_diag_info_legacy_unpadded;

/* cc3501e-bridge-firmware `fix/107-bound-sock-send-seqguard` (the ONLY
 * SOCK_SEND firmware shape this suite models -- alp-sdk has no active
 * customers on the older, unbound shape) mutant controls for
 * cc3501e_sock_send()'s remainder-retry loop, backed by a state machine that
 * mirrors that firmware's `src/protocol_sockets.c` (`handle_sock_send()`,
 * the seq-keyed reply cache) and `src/worker.c` (the one-job worker slot,
 * `worker_submit_payload` / `worker_discard_stale_terminal` /
 * `worker_reclaim_matching_terminal`) exactly, instead of a bare
 * clock-threshold check:
 *
 *   1. Every dispatch first lets a PENDING job that has reached its own
 *      ready time become TERMINAL and fills the reply cache -- by seq,
 *      status AND reply, success or a decoded ERR alike -- exactly as
 *      `worker_execute()` publishes into `protocol_sock_send_on_worker_
 *      complete()` the instant a job completes, regardless of who (if
 *      anyone) is polling at that moment.
 *   2. A request whose seq matches a VALID cache entry invalidates nothing,
 *      reclaims a matching terminal job (frees the slot, mirroring
 *      `worker_reclaim_matching_terminal`), and is answered straight from
 *      the cache -- no re-execution.
 *   3. A request whose seq does NOT match a valid cache entry invalidates
 *      it. It then discards a SITTING TERMINAL job whose seq differs
 *      (`worker_discard_stale_terminal` -- freeing the slot for step 4) but
 *      leaves a QUEUED/RUNNING job alone regardless of seq.
 *   4. If the slot is now genuinely idle, SUBMIT: compute the result exactly
 *      ONCE (bumping slave.sock_send_body_exec_count so a test can prove
 *      that), arm its ready time, and answer BUSY unconditionally --
 *      `worker_submit_payload()` never resolves a brand-new job on its own
 *      submitting poll. If a DIFFERENT job is still QUEUED/RUNNING, answer
 *      BUSY and submit NOTHING (`worker_submit_payload` refuses a non-IDLE
 *      slot) -- this is the one case a bare clock-threshold fake cannot even
 *      represent, since it has no notion of "still running" independent of
 *      elapsed time.
 *
 * Without this, a fake that only checks a clock cannot fail a mutation that
 * changes the grace round's seq (nothing to mismatch against), lets the "job
 * body" re-run on every poll instead of once (no exec counter to prove
 * otherwise), or resubmits over a job that is genuinely still running.
 *
 * All cleared by slave_reset(), so the default across the suite is one
 * BUSY-then-OK round trip per frame, queuing the whole declared data_len.
 *
 *   - g_sock_send_queue_plan / _len: per-SUBMISSION queued-byte counts to
 *     report, in order (indexed by slave.sock_send_body_exec_count AT SUBMIT
 *     TIME, i.e. how many frames have been submitted so far -- one per
 *     genuinely new iteration, not one per raw BUSY-then-OK dispatch pair);
 *     once the plan is exhausted, a submission falls back to computing the
 *     full data_len, same as the default.
 *   - g_sock_send_always_zero: every submission computes 0 bytes queued,
 *     regardless of the plan above -- models a peer that never reads, so the
 *     loop must give up on ITS OWN elapsed-time budget rather than hang.
 *   - g_sock_send_short_reply: stage an unpadded ALP_CC3501E_RESP_OK_LEGACY
 *     reply with only 1 data byte (not the 2 a real queued-count needs) on
 *     EVERY dispatch, bypassing the worker model entirely -- a firmware/wire
 *     gap, not backpressure. Takes priority over every other control below.
 *   - g_sock_send_busy_step_ms[] / _step_count: how long (from ITS OWN
 *     submission time) a frame stays QUEUED/RUNNING before it is ready to
 *     become terminal, indexed the same way as the queue plan. step_count==0
 *     means every submitted frame is ready on its very next poll (the
 *     default: one BUSY, one OK). Models two review follow-ups (both
 *     alp-sdk#2035):
 *       - a per-iteration remaining budget can run out while poll_by_repeat()
 *         is still retrying a genuinely in-flight BUSY job -- a single
 *         busy_step_ms longer than timeout_ms, so the PER-ITERATION
 *         poll_by_repeat() call times out, but short enough that the
 *         collection grace afterward covers it.
 *       - cc3501e_sock_send() must budget each iteration off the genuinely
 *         REMAINING time, not a fresh copy of timeout_ms -- a
 *         `budget = timeout_ms` regression lets a LATER iteration retry with
 *         far more time than it correctly has left, resolving a busy_step_ms
 *         that a correctly-shrunk budget (even with the grace window added)
 *         could not have.
 *   - g_sock_send_resolve_as_error: once a submitted job becomes ready,
 *     completing it publishes this REAL decoded device-side error (an
 *     ALP_CC3501E_RESP_ERR_* byte) into the cache instead of RESP_OK+queued-
 *     count -- models a genuine failure (e.g. a peer reset) discovered once
 *     the collection grace catches up with an in-flight job. 0 (the
 *     default) means resolve normally. */
static uint16_t g_sock_send_queue_plan[4];
static uint32_t g_sock_send_queue_plan_len;
static bool     g_sock_send_always_zero;
static bool     g_sock_send_short_reply;
static uint32_t g_sock_send_busy_step_ms[4];
static uint32_t g_sock_send_busy_step_count;
static uint8_t  g_sock_send_resolve_as_error;

/* The one-worker-slot state (IDLE implied by pending==sitting_terminal==
 * false) + the seq-keyed reply cache -- opcode-keyed (this whole model only
 * ever handles CMD_SOCK_SEND, so there is implicitly one slot per opcode
 * already). See the mutant-control doc comment above for the exact
 * step-by-step algorithm these back. */
static bool     g_sock_send_worker_pending;          /* QUEUED/RUNNING          */
static bool     g_sock_send_worker_sitting_terminal; /* DONE/ERR, uncollected   */
static uint8_t  g_sock_send_worker_seq;              /* pending OR sitting job's seq */
static uint16_t g_sock_send_worker_computed_queued;
static uint8_t  g_sock_send_worker_resolve_status; /* 0 = OK; else an ALP_CC3501E_RESP_ERR_* */
static uint64_t g_sock_send_worker_ready_ms;
/* The submitted request's OWN declared length + inline bytes, captured AT
 * SUBMIT time (step 4b below) -- NOT re-read from slave.req_pl once the job
 * becomes terminal, because by then req_pl may belong to a COMPLETELY
 * UNRELATED dispatch (this job can complete in the "background", noticed by
 * whatever request happens to be dispatched once its ready time passes --
 * see step 1). Needed only to log THIS job's own frame correctly at
 * completion. */
static uint16_t g_sock_send_worker_dl;
static uint8_t  g_sock_send_worker_payload[32];
static bool     g_sock_send_cache_valid;
static uint8_t  g_sock_send_cache_seq;
static uint8_t  g_sock_send_cache_status; /* the ALP_CC3501E_RESP_* answered -- OK or an ERR */
static uint16_t g_sock_send_cache_queued; /* valid iff cache_status == ALP_CC3501E_RESP_OK */

/* alp-sdk#2035 review follow-up: simulates a genuinely CONCURRENT caller
 * holding ctx->request_lock, for the two cc3501e_sock_send() lock-timeout
 * tests below. `g_lock_contention_arm_on_submit`, set by a test BEFORE
 * calling cc3501e_sock_send(), makes the SOCK_SEND fake's very first SUBMIT
 * (case ALP_CC3501E_CMD_SOCK_SEND, step 4b) request that the lock become
 * externally held starting from the very NEXT alp_delay_ms() call -- i.e.
 * AFTER that first transaction's own cc3501e_lock_release(), never during
 * it (setting request_lock=true mid-transaction would just be clobbered by
 * that same transaction's own release). Held for
 * LOCK_CONTENTION_WINDOW_MS (150 ms: comfortably longer than
 * CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS's default 100 ms internal
 * spin, so a lock-acquire attempt landing inside the window genuinely
 * exhausts it, then comfortably inside a 250 ms+ caller budget so a LATER
 * attempt has time to succeed once the window ends), then released.
 * Declared here (ahead of slave_dispatch()'s own use of
 * g_lock_contention_arm_on_submit in the SOCK_SEND case below) rather than
 * beside alp_delay_ms(), which is the only other place that reads them. */
#define LOCK_CONTENTION_WINDOW_MS 150u
static bool     g_lock_contention_arm_on_submit;
static bool     g_lock_contention_pending;
static uint64_t g_lock_contention_release_at_ms;

/* alp-sdk#2108 CMD_SOCK_RECV lazy-commit replay model, gated by
 * g_sock_recv_use_ring_model (default false keeps the whole suite's existing
 * fixed 5-byte "hello" SOCK_RECV reply -- see the default case in
 * slave_dispatch() below -- completely unchanged). This mirrors the
 * cc3501e-bridge-firmware fix/sock-recv-retry-safe design (NOT YET MERGED --
 * see ctx->sock_recv_seq's own comment in <alp/chips/cc3501e/core.h>) that
 * motivates ctx->sock_recv_seq: the ring advance is LAZY-COMMITTED, keyed on
 * (header seq, socket handle) recorded on EVERY SOCK_RECV dispatch, so
 *
 *   - a request whose seq AND handle match the key recorded by the
 *     IMMEDIATELY PRECEDING dispatch is a REPLAY -- the ring does NOT
 *     advance, and the reply serves the same bytes as before, or MORE of
 *     them if this dispatch asks for a bigger cap than the original commit
 *     did (see slave_dispatch()'s SOCK_RECV case for exactly how);
 *   - any other request (different seq, different handle, or the very first
 *     recv) COMMITS: the ring advances by up to g_sock_recv_chunk_len bytes.
 *
 * ONE shared key for the whole fake, not one per handle -- matching
 * ctx->sock_recv_seq being one counter per ctx, not one per handle (see its
 * own comment for why that is sufficient: the firmware/fake only ever
 * compares the CURRENT dispatch against the single most recent entry, never
 * against history, so a numeric seq coincidence between two dispatches many
 * calls apart is harmless as long as neither is the OTHER's immediate
 * predecessor).
 *
 * A bare clock/counter fake cannot fail a mutation that re-allocates a fresh
 * seq per RETRY (it would make a genuine retry look like a brand-new recv,
 * skipping bytes) or one that shares the seq counter across two DIFFERENT
 * recvs (it would make two genuinely different recvs alias into one replay,
 * losing a whole block) -- this model can, because it actually keys its
 * decision on (seq, handle) exactly as the pending firmware change does. */
static bool     g_sock_recv_use_ring_model;
static uint8_t  g_sock_recv_source[64]; /* bytes available to serve, in order */
static size_t   g_sock_recv_source_len;
static size_t   g_sock_recv_chunk_len = 5u; /* bytes newly exposed per COMMIT */
static size_t   g_sock_recv_ring_pos;       /* bytes already committed off the source */
static bool     g_sock_recv_last_valid;
static uint8_t  g_sock_recv_last_seq;
static uint16_t g_sock_recv_last_handle;
/* Where the LAST COMMIT started serving from -- NOT the same as
 * g_sock_recv_ring_pos, which by commit time has already moved PAST that
 * point. A REPLAY must re-serve starting from here, not from the current
 * (already-advanced) ring position -- see slave_dispatch()'s SOCK_RECV case. */
static size_t   g_sock_recv_last_commit_pos;
static uint32_t g_sock_recv_commit_count; /* # of times the ring actually advanced */
/* Corrupts the CRC trailer of the next N staged SOCK_RECV replies, each
 * decrementing this by one (0 = don't corrupt) -- models a genuine reply
 * that the firmware committed and sent correctly but which arrived on the
 * wire with a bad CRC (a real transport bit-flip, not a firmware bug), so
 * the host must reject each such transmission. Set to 1 for a single lost
 * reply that a same-call retry re-collects; set higher (e.g. large enough to
 * outlast one cc3501e_sock_recv() call's own internal retry budget) to make
 * that WHOLE call time out, modelling a reply lost on every attempt within
 * it -- see test_sock_recv_timeout_keeps_seq_and_retry_recovers_the_chunk. */
static uint32_t g_sock_recv_corrupt_crc_remaining;

/* Worker-routed BUSY acks before the real reply, same pattern as
 * slave.rssi_busy_polls_remaining -- lets a test drive SOCK_RECV through
 * poll_by_repeat_seq()'s OWN internal retry loop (independent of the ring
 * model above) to prove the retry re-sends the SAME seq every attempt. */
static uint32_t g_sock_recv_busy_polls_remaining;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase                        = PH_REQ_HDR;
	slave.wifi_conn_state              = ALP_CC3501E_WIFI_CONNECTED; /* preserves the pre-#1376
	                                                                * behaviour of tests that
	                                                                * poll WIFI_STATUS directly
	                                                                * without going through
	                                                                * cc3501e_wifi_connect() first. */
	slave.wifi_fail_reason             = ALP_CC3501E_WIFI_FAIL_NONE;
	slave.wifi_conn_rssi               = -50;
	slave.status_polls_before_terminal = 0u;
	g_scan_stage_ctx_b                 = false;
	g_connect_submit_force_ok          = false;
	g_diag_role                        = ALP_CC3501E_ROLE_WIFI_STA;
	g_ota_pending                      = ALP_CC3501E_OTA_PENDING_STAGED;
	g_status_io_down_remaining         = 0u;
	g_get_version_override_active      = false;
	g_get_version_override_value       = 0u;
	g_get_version_io_down_remaining    = 0u;
	g_all_io_down                      = false;
	g_req_hdr_desynced                 = false;
	g_reply_hdr_deaf                   = false;
	g_heal_disabled                    = false;
	g_ble_enable_busy                  = false;
	g_reset_release_count              = 0u;
	g_ping_always_fails                = false;
	g_spi1_reply_bad_seq               = false;
	g_diag_stats_v8                    = false;
	g_caps_override_value              = 0u;
	g_caps_reply_short                 = false;
	g_get_mac_corrupt_remaining        = 0u;
	g_get_mac_serve_count              = 0u;
	g_bare_ok_legacy                   = false;
	g_force_dead_phase_zero_payload    = false;
	g_get_ip_no_address                = false;
	g_get_ip_io_down_remaining         = 0u;
	g_diag_info_reply_len              = 18u;
	g_diag_info_legacy_unpadded        = false;
	memset(g_sock_send_queue_plan, 0, sizeof(g_sock_send_queue_plan));
	g_sock_send_queue_plan_len = 0u;
	g_sock_send_always_zero    = false;
	g_sock_send_short_reply    = false;
	memset(g_sock_send_busy_step_ms, 0, sizeof(g_sock_send_busy_step_ms));
	g_sock_send_busy_step_count         = 0u;
	g_sock_send_resolve_as_error        = 0u;
	g_sock_send_worker_pending          = false;
	g_sock_send_worker_sitting_terminal = false;
	g_sock_send_worker_seq              = 0u;
	g_sock_send_worker_computed_queued  = 0u;
	g_sock_send_worker_resolve_status   = 0u;
	g_sock_send_worker_ready_ms         = 0u;
	g_sock_send_worker_dl               = 0u;
	memset(g_sock_send_worker_payload, 0, sizeof(g_sock_send_worker_payload));
	g_sock_send_cache_valid    = false;
	g_sock_send_cache_seq      = 0u;
	g_sock_send_cache_status   = 0u;
	g_sock_send_cache_queued   = 0u;
	g_sock_recv_use_ring_model = false;
	memset(g_sock_recv_source, 0, sizeof(g_sock_recv_source));
	g_sock_recv_source_len            = 0u;
	g_sock_recv_chunk_len             = 5u;
	g_sock_recv_ring_pos              = 0u;
	g_sock_recv_last_valid            = false;
	g_sock_recv_last_seq              = 0u;
	g_sock_recv_last_handle           = 0u;
	g_sock_recv_last_commit_pos       = 0u;
	g_sock_recv_commit_count          = 0u;
	g_sock_recv_corrupt_crc_remaining = 0u;
	g_sock_recv_busy_polls_remaining  = 0u;
}

/* RESP_OK stages the real MAJOR-4 shape (padded + CRC trailer -- the only
 * shape cc3501e_reply_verdict() accepts for a 0x5A status, see
 * cc3501e_reply_model.h); any ALP_CC3501E_RESP_ERR_* code keeps the plain
 * legacy shape, which the reply-verdict decode also accepts pre-negotiation. */
static void stage_status(uint8_t st)
{
	if (st == ALP_CC3501E_RESP_OK) {
		slave.reply_len = cc3501e_model_stage_reply(slave.reply_pl, slave.cmd, st, NULL, 0u);
	} else {
		slave.reply_len = cc3501e_model_stage_legacy_reply(slave.reply_pl, st, NULL, 0u);
	}
}

/* status(1) + @n data bytes copied from @data -- same shape rule as
 * stage_status() above. */
static void stage_reply(uint8_t st, const uint8_t *data, uint16_t n)
{
	if (st == ALP_CC3501E_RESP_OK) {
		slave.reply_len = cc3501e_model_stage_reply(slave.reply_pl, slave.cmd, st, data, n);
	} else {
		slave.reply_len = cc3501e_model_stage_legacy_reply(slave.reply_pl, st, data, n);
	}
}

/* ---- canned decode fixtures (the values the DECODE tests assert on) -------- */

static const uint8_t FIX_MAC[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* Two Wi-Fi scan records, packed exactly as the firmware returns them:
 * bssid[6] | rssi(int8) | channel | security_info(LE16) | ssid_len | ssid[]. */
static uint16_t build_wifi_scan(uint8_t *p)
{
	uint16_t o = 0u;
	/* rec0: "Test", ch6, -40 dBm, WPA2 (sec-type bitmap 0x04 in the high byte). */
	const uint8_t b0[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
	memcpy(&p[o], b0, 6);
	o += 6u;
	p[o++] = (uint8_t)(-40);   /* rssi */
	p[o++] = 6u;               /* channel */
	p[o++] = 0x00u;            /* security_info LE lo */
	p[o++] = 0x04u;            /* security_info LE hi -> 0x0400 = WPA2 */
	p[o++] = 4u;               /* ssid_len */
	memcpy(&p[o], "Test", 4u); /* ssid */
	o += 4u;
	/* rec1: "OpenNet", ch11, -70 dBm, open (sec bits 0). */
	const uint8_t b1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	memcpy(&p[o], b1, 6);
	o += 6u;
	p[o++] = (uint8_t)(-70);
	p[o++] = 11u;
	p[o++] = 0x00u;
	p[o++] = 0x00u; /* open */
	p[o++] = 7u;
	memcpy(&p[o], "OpenNet", 7u);
	o += 7u;
	return o;
}

/* A single, deliberately DIFFERENT-content Wi-Fi scan record ("Ctx2Net") used
 * by test_wifi_scan_buf_is_per_context_740 to stage a second context's own
 * scan reply -- so a byte-compare of ctx A's raw decode buffer after ctx B's
 * scan is a meaningful "did B's bytes leak into A" check, not a comparison
 * of two identical payloads that would pass even if they aliased. */
static uint16_t build_wifi_scan_ctx_b(uint8_t *p)
{
	uint16_t      o     = 0u;
	const uint8_t b0[6] = { 0x99, 0x88, 0x77, 0x66, 0x55, 0x44 };
	memcpy(&p[o], b0, 6);
	o += 6u;
	p[o++] = (uint8_t)(-60);      /* rssi */
	p[o++] = 1u;                  /* channel */
	p[o++] = 0x00u;               /* security_info LE lo */
	p[o++] = 0x00u;               /* security_info LE hi -> open */
	p[o++] = 7u;                  /* ssid_len */
	memcpy(&p[o], "Ctx2Net", 7u); /* ssid */
	o += 7u;
	return o;
}

/* Two BLE scan records: addr[6] | addr_type | rssi(int8) | name_len | name[]. */
static uint16_t build_ble_scan(uint8_t *p)
{
	uint16_t o = 0u;
	/* rec0: named "MyBLE", public addr, -55 dBm. */
	const uint8_t a0[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	memcpy(&p[o], a0, 6);
	o += 6u;
	p[o++] = 0u; /* addr_type public */
	p[o++] = (uint8_t)(-55);
	p[o++] = 5u;
	memcpy(&p[o], "MyBLE", 5u);
	o += 5u;
	/* rec1: nameless, random addr, -88 dBm. */
	const uint8_t a1[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
	memcpy(&p[o], a1, 6);
	o += 6u;
	p[o++] = 1u; /* addr_type random */
	p[o++] = (uint8_t)(-88);
	p[o++] = 0u; /* no name */
	return o;
}

/* Single, deliberately DIFFERENT-content BLE record ("Ctx2Dev") -- BLE
 * counterpart of build_wifi_scan_ctx_b(), see its comment. */
static uint16_t build_ble_scan_ctx_b(uint8_t *p)
{
	uint16_t      o     = 0u;
	const uint8_t a0[6] = { 0x60, 0x50, 0x40, 0x30, 0x20, 0x10 };
	memcpy(&p[o], a0, 6);
	o += 6u;
	p[o++] = 1u; /* addr_type random */
	p[o++] = (uint8_t)(-33);
	p[o++] = 7u;
	memcpy(&p[o], "Ctx2Dev", 7u);
	o += 7u;
	return o;
}

/* Build the reply for the just-received request. */
static void slave_dispatch(void)
{
	if (g_ble_enable_busy && slave.cmd == ALP_CC3501E_CMD_BLE_ENABLE) {
		/* Genuine decoded status every time -- a bridge that is alive and
		 * still working the request, never silent. See g_ble_enable_busy's
		 * own comment above. */
		stage_status(ALP_CC3501E_RESP_ERR_BUSY);
		return;
	}
	if (g_force_dead_phase_zero_payload && (slave.cmd == ALP_CC3501E_CMD_BLE_GATT_REGISTER ||
	                                        slave.cmd == ALP_CC3501E_CMD_WIFI_SCAN_START ||
	                                        slave.cmd == ALP_CC3501E_CMD_BLE_SCAN_START)) {
		/* See g_force_dead_phase_zero_payload's comment above -- overrides
		 * the normal per-opcode fixture below entirely. */
		uint8_t zeros[ALP_CC3501E_REPLY_PAD] = { 0 };
		slave.reply_len                      = cc3501e_model_stage_legacy_reply(
		    slave.reply_pl, 0x00u, &zeros[1], (uint16_t)(ALP_CC3501E_REPLY_PAD - 1u));
		return;
	}
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_PING:
	case ALP_CC3501E_CMD_RESET:
	case ALP_CC3501E_CMD_WIFI_DISCONNECT:
	case ALP_CC3501E_CMD_WIFI_AP_STOP:
	case ALP_CC3501E_CMD_WIFI_SCAN_STOP:
	case ALP_CC3501E_CMD_BLE_ENABLE:
	case ALP_CC3501E_CMD_BLE_DISABLE:
	case ALP_CC3501E_CMD_BLE_ADV_START:
	case ALP_CC3501E_CMD_BLE_ADV_STOP:
	case ALP_CC3501E_CMD_BLE_SCAN_STOP:
	case ALP_CC3501E_CMD_BLE_CONNECT:
	case ALP_CC3501E_CMD_BLE_DISCONNECT:
	case ALP_CC3501E_CMD_BLE_GATT_REGISTER:
	case ALP_CC3501E_CMD_BLE_GATT_NOTIFY:
	case ALP_CC3501E_CMD_BLE_GATT_WRITE:
	case ALP_CC3501E_CMD_GPIO_SET_INTERRUPT:
	case ALP_CC3501E_CMD_CAM_ENABLE:
	case ALP_CC3501E_CMD_CAM_DISABLE:
	case ALP_CC3501E_CMD_POWER_POLICY:
	case ALP_CC3501E_CMD_DIAG_LOG_LEVEL:
	/* BIND / LISTEN (protocol v9) reply with the bare status too: neither
	 * carries reply data, and there is no ACCEPT opcode to model -- an inbound
	 * connection arrives as an EVT_SOCK_ACCEPTED entry on the event queue,
	 * which the cc3501e_host_events suite covers. */
	case ALP_CC3501E_CMD_SOCK_BIND:
	case ALP_CC3501E_CMD_SOCK_LISTEN:
	/* OTA_PROMOTE (0x46) belongs in THIS bucket, not with the worker-routed
	 * submits below: handle_ota_promote() returns
	 * hw_to_resp(cc3501e_hw_ota_promote()), and the TI HAL's
	 * cc3501e_hw_ota_promote() arms the deferred swap-reboot and returns
	 * CC3501E_HW_OK unconditionally -- a bare RESP_OK is its ONLY success
	 * reply.  Modelled here so test_ota_promote_bare_ok_still_accepted_1385
	 * fences the #1385 check against being over-extended onto it. */
	case ALP_CC3501E_CMD_OTA_PROMOTE:
		/* Argless / write-only ops: success is the bare OK status.
		 * g_bare_ok_legacy opts into the MAJOR-3 shape (RESP_OK_LEGACY,
		 * unpadded, no CRC) instead -- see
		 * test_ping_accepts_legacy_major3_bare_ok_shape_bilingual. */
		stage_status(g_bare_ok_legacy ? ALP_CC3501E_RESP_OK_LEGACY : ALP_CC3501E_RESP_OK);
		break;

	case ALP_CC3501E_CMD_SOCK_CONNECT:
	case ALP_CC3501E_CMD_SOCK_CLOSE:
		/* alp-sdk#2108 fake fidelity: CONNECT/CLOSE start or end a socket
		 * session, so any cached SOCK_RECV replay key is stale afterward --
		 * a fresh session (even one that reuses the same handle value) must
		 * not be answered from a PRIOR session's cached reply. Otherwise
		 * identical to the bare-status group above; kept separate purely to
		 * run this one extra side effect. Resets only the replay-key
		 * validity, not the ring/source position -- this simplified fake
		 * models one shared receive stream, not one per handle (see the
		 * g_sock_recv_* globals' own comment), so a still-open OTHER
		 * handle's next recv is unaffected. */
		g_sock_recv_last_valid = false;
		stage_status(g_bare_ok_legacy ? ALP_CC3501E_RESP_OK_LEGACY : ALP_CC3501E_RESP_OK);
		break;

	case ALP_CC3501E_CMD_OTA_STATUS: {
		/* 16 bytes: state(1) | reserved(3) | bytes_written(LE32) |
		 * total_len(LE32) | pending(1) | reserved2(3).  Only `pending` matters
		 * to the promote path -- it is the flash-derived byte the commit is
		 * gated on, and the one an ack cannot forge. */
		uint8_t d[16] = { 0 };
		d[0]          = ALP_CC3501E_OTA_STATE_STAGED;
		d[12]         = g_ota_pending;
		stage_reply(ALP_CC3501E_RESP_OK, d, 16u);
		break;
	}

	case ALP_CC3501E_CMD_OTA_UPDATE_MODE: {
		/* #2126: instant-mode-switch model -- reply echoes back the requested
		 * mode byte (slave.req_pl[0]) immediately, so
		 * cc3501e_ota_update_mode()'s update_mode_reads_as() readback check
		 * succeeds on the FIRST round trip and cc3501e_set_peer_polled() runs
		 * with no blind-settle/reboot needed. Good enough for this suite's
		 * purpose (proving cc3501e_link_check_and_recover() honours
		 * cc3501e_peer_is_polled()); the real reboot-and-poll shape is out of
		 * scope here. */
		uint8_t d[4] = { slave.req_pl[0], 0u, 0u, 0u };
		stage_reply(ALP_CC3501E_RESP_OK, d, 4u);
		break;
	}

	case ALP_CC3501E_CMD_WIFI_CONNECT_STA:
		/* Fire-and-forget submit model (#1376/#1377/#1378): snapshot the
		 * submit's own request bytes separately (the generic req_pl/req_len
		 * above get overwritten by cc3501e_wifi_connect()'s follow-up
		 * WIFI_STATUS polls before the caller ever sees them), count the
		 * submit, and ack RESP_ERR_BUSY -- the firmware's real, unconditional
		 * WORKER_IDLE ack -- unless a test forces the #1378 dead-phase-alias
		 * scenario via g_connect_submit_force_ok. */
		slave.connect_last_req_len = slave.req_len;
		memcpy(slave.connect_last_req_pl, slave.req_pl, slave.req_len);
		slave.connect_submit_count++;
		stage_status(g_connect_submit_force_ok ? ALP_CC3501E_RESP_OK_LEGACY
		                                       : ALP_CC3501E_RESP_ERR_BUSY);
		break;

	case ALP_CC3501E_CMD_WIFI_AP_START:
		/* #1385: AP_START runs through the SAME firmware handler as
		 * CONNECT_STA (handle_worker_routed_payload), so its submit ack is
		 * the same unconditional RESP_ERR_BUSY -- and the WORKER_DONE branch
		 * that would reply a bare RESP_OK is unreachable to the host, because
		 * worker_run_pending() calls worker_reset() for CONNECT_STA/AP_START
		 * before cc3501e_bridge_ready() re-arms the link.  The old model
		 * staged a bare RESP_OK here (the argless bucket above), which is a
		 * byte pattern the real firmware can never produce for this opcode --
		 * it modelled the dead-phase alias itself as success. */
		slave.ap_start_last_req_len = slave.req_len;
		memcpy(slave.ap_start_last_req_pl, slave.req_pl, slave.req_len);
		slave.ap_start_submit_count++;
		stage_status(g_connect_submit_force_ok ? ALP_CC3501E_RESP_OK_LEGACY
		                                       : ALP_CC3501E_RESP_ERR_BUSY);
		break;

	case ALP_CC3501E_CMD_GET_VERSION: {
		uint16_t      ver  = g_get_version_override_active ? g_get_version_override_value
		                                                   : (uint16_t)ALP_CC3501E_PROTOCOL_VERSION;
		const uint8_t v[2] = { (uint8_t)(ver & 0xFFu), (uint8_t)((ver >> 8) & 0xFFu) };
		stage_reply(ALP_CC3501E_RESP_OK, v, 2u);
		break;
	}
	case ALP_CC3501E_CMD_GET_MAC: {
		uint8_t m[6];
		memcpy(m, FIX_MAC, sizeof(m));
		if (g_get_mac_corrupt_remaining > 0u) {
			g_get_mac_corrupt_remaining--;
			m[0] ^= 0x04u; /* one bit, exactly as the bench saw */
		}
		g_get_mac_serve_count++;
		stage_reply(ALP_CC3501E_RESP_OK, m, 6u);
		break;
	}

	case ALP_CC3501E_CMD_GET_DIAG_INFO: {
		/* alp_cc3501e_diag_info_t: fw_version(LE16) | reset_cause | role |
		 * uptime_ms(LE32) | free_heap(LE32) | last_error | reserved[3] |
		 * dhcp_state | netif_status.  The last two bytes grew ADDITIVELY
		 * (alp-sdk#2035); g_diag_info_reply_len dials how many the slave
		 * actually sends, per the pre/post-#2035 tests below. */
		uint8_t d[18] = { 0 };
		d[0]          = 0x02u; /* fw_version = 0x0102 */
		d[1]          = 0x01u;
		d[2]          = ALP_CC3501E_RESET_POWER_ON;
		d[3]          = g_diag_role;
		d[4]          = 0xEFu; /* uptime = 0x00ABCDEF */
		d[5]          = 0xCDu;
		d[6]          = 0xABu;
		d[7]          = 0x00u;
		d[8]          = 0x40u; /* free_heap = 0x00012340 */
		d[9]          = 0x23u;
		d[10]         = 0x01u;
		d[11]         = 0x00u;
		d[12]         = ALP_CC3501E_RESP_OK;          /* last_error */
		d[16]         = ALP_CC3501E_DHCP_STATE_BOUND; /* dhcp_state */
		d[17]         = 0x17u;                        /* netif_status: UP|LINK_UP + dhcp->tries=5 */
		if (g_diag_info_legacy_unpadded) {
			/* Genuinely 16 data bytes on the wire, no pad, no CRC -- see
			 * g_diag_info_legacy_unpadded's doc comment above. */
			slave.reply_len =
			    cc3501e_model_stage_legacy_reply(slave.reply_pl, ALP_CC3501E_RESP_OK, d, 16u);
		} else {
			stage_reply(ALP_CC3501E_RESP_OK, d, g_diag_info_reply_len);
		}
		break;
	}
	case ALP_CC3501E_CMD_DIAG_GET_STATS: {
		/* v7 answers 8 bytes, v8 answers 16 -- ADDITIVELY, same first two
		 * counters.  Default to the v7 shape so every pre-existing test keeps
		 * exercising the old-firmware path; g_diag_stats_v8 opts in. */
		uint8_t s[16] = { 0x44, 0x33, 0x22, 0x11,   /* frames_ok        = 0x11223344 */
			              0x05, 0x00, 0x00, 0x00,   /* frames_err       = 0x00000005 */
			              0x07, 0x00, 0x00, 0x00,   /* worker_execs     = 0x00000007 */
			              0x03, 0x00, 0x00, 0x00 }; /* retry_latch_hits = 0x00000003 */
		stage_reply(ALP_CC3501E_RESP_OK, s, g_diag_stats_v8 ? 16u : 8u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_RSSI: {
		/* Worker-routed (#1377): ack BUSY for rssi_busy_polls_remaining
		 * submits (simulating the firmware's WORKER_IDLE-then-collect shape)
		 * before finally handing back the value. */
		if (slave.rssi_busy_polls_remaining > 0u) {
			slave.rssi_busy_polls_remaining--;
			stage_status(ALP_CC3501E_RESP_ERR_BUSY);
			break;
		}
		const uint8_t r = (uint8_t)(-42);
		stage_reply(ALP_CC3501E_RESP_OK, &r, 1u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_GET_IP: {
		if (g_get_ip_no_address) {
			/* #2035: the real firmware's only status for "no address on this
			 * interface yet" -- see hal/ti/cc3501e_hw_ti_wifi.c. */
			stage_status(ALP_CC3501E_RESP_ERR_RADIO);
			break;
		}
		/* On the wire the octets arrive REVERSED (the firmware extracts the lwIP
		 * network-order u32 MSB-first); the host reverses them back.  Stage the
		 * wire order for 192.168.1.14 (0xC0A8010E) = {0x0E,0x01,0xA8,0xC0}. */
		const uint8_t wire[4] = { 0x0E, 0x01, 0xA8, 0xC0 };
		stage_reply(ALP_CC3501E_RESP_OK, wire, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_STATUS: {
		uint8_t st[4];
		if (slave.status_polls_before_terminal > 0u) {
			/* Still resolving: report CONNECTING (bridge busy on the real
			 * link) rather than the terminal latch below. */
			slave.status_polls_before_terminal--;
			st[0] = ALP_CC3501E_WIFI_CONNECTING;
			st[1] = ALP_CC3501E_WIFI_FAIL_NONE;
			st[2] = 0;
			st[3] = 0u;
		} else {
			st[0] = slave.wifi_conn_state;
			st[1] = slave.wifi_fail_reason;
			st[2] = (uint8_t)slave.wifi_conn_rssi;
			st[3] = slave.wifi_last_reason;
		}
		stage_reply(ALP_CC3501E_RESP_OK, st, 4u);
		break;
	}
	case ALP_CC3501E_CMD_WIFI_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = g_scan_stage_ctx_b ? build_wifi_scan_ctx_b(recs) : build_wifi_scan(recs);
		stage_reply(ALP_CC3501E_RESP_OK, recs, n);
		break;
	}
	case ALP_CC3501E_CMD_BLE_SCAN_START: {
		uint8_t  recs[ALP_CC3501E_MAX_PAYLOAD];
		uint16_t n = g_scan_stage_ctx_b ? build_ble_scan_ctx_b(recs) : build_ble_scan(recs);
		stage_reply(ALP_CC3501E_RESP_OK, recs, n);
		break;
	}
	case ALP_CC3501E_CMD_BLE_GATT_READ: {
		/* attribute value bytes -- deliberately the LEGACY (RESP_OK_LEGACY,
		 * unpadded) shape: GATT_READ's reply DATA is the raw attribute value
		 * with NO length field of its own (chips/cc3501e/cc3501e_ble.c),
		 * self-delimited only by the wire's declared payload_len. Under
		 * MAJOR-4 every reply is padded to an ALP_CC3501E_REPLY_PAD multiple
		 * (cc3501e_reply_model.h), so a genuinely short attribute value would
		 * arrive with trailing pad+CRC bytes indistinguishable from more
		 * attribute data -- a real firmware can never send an exact 2-byte
		 * MAJOR-4 reply here. The legacy shape is the only one that can still
		 * assert an exact decoded length. */
		const uint8_t val[2] = { 0xAB, 0xCD };
		stage_reply(ALP_CC3501E_RESP_OK_LEGACY, val, 2u);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_OPEN: {
		/* reply DATA = alp_cc3501e_sock_handle_t { handle(LE16) | rsvd[2] }. */
		/* #2126 review: firmware handle 0x0034 -- kept <= 0xFF so
		 * cc3501e_sock_open()'s epoch-encode (upper byte = ctx->
		 * link_epoch) does not refuse it defensively (see
		 * cc3501e_handle_encode()'s own comment, chips/cc3501e/
		 * cc3501e_sockets.c) the way a real 2-byte firmware handle
		 * would today, since that range is unverified. */
		const uint8_t h[4] = { 0x34, 0x00, 0x00, 0x00 };
		stage_reply(ALP_CC3501E_RESP_OK, h, 4u);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_SEND: {
		/* Every SOCK_SEND dispatch costs the fake clock at least 1 ms of
		 * simulated wire-transfer time, unconditionally -- including under
		 * g_sock_send_always_zero, where NOTHING else advances it (an
		 * ALP_OK+0-queued reply is not retryable, so poll_by_repeat() itself
		 * never sleeps for it). Without this, deleting the driver's own
		 * zero-progress back-off leaves nothing to advance g_fake_now_ms
		 * toward the deadline and test_sock_send_never_queued_times_out_
		 * and_backs_off_107 HANGS instead of failing -- a killed-agent-style
		 * false read, worse than a red assertion. With it, that mutation
		 * instead blows the test's dispatch-count bound (many more 1 ms
		 * dispatches than the ~20 ms-paced back-off needs), a clean FAIL. */
		alp_delay_ms(1u);

		uint16_t dl  = (uint16_t)slave.req_pl[4] | ((uint16_t)slave.req_pl[5] << 8);
		uint8_t  seq = slave.req_pl[3];

		/* alp-sdk#2035 review follow-up: the seqguard firmware
		 * (cc3501e-bridge-firmware#134) rejects a frame whose actual length
		 * does not match its OWN declared data_len (protocol_sockets.c
		 * ~236: req_len != 8 + data_len -> RESP_ERR_INVALID) -- catching a
		 * host that offers a shrunk/grown data_len but still ships the OLD
		 * (or a wrong) byte count. Checked before anything else, same as
		 * the real handler. */
		if (slave.req_len != (uint16_t)(8u + dl)) { /* 8 == the SOCK_SEND wire header */
			stage_status(ALP_CC3501E_RESP_ERR_INVALID);
			break;
		}

		if (g_sock_send_short_reply) {
			/* Malformed reply: ALP_OK with only 1 data byte, not the 2 a real
			 * queued-count needs (alp-sdk#2035). RESP_OK_LEGACY (the unpadded
			 * shape, via stage_reply()'s own branch on the status byte -- see
			 * cc3501e_reply_model.h), not RESP_OK: the padded MAJOR-4 shape
			 * cc3501e_model_stage_reply() builds for RESP_OK always rounds up
			 * to at least ALP_CC3501E_REPLY_PAD + CRC bytes, so a 1-byte
			 * request there is unfalsifiable -- got can never come back under
			 * 2 through it (same reason the GATT_READ test above uses the
			 * legacy shape). Bypasses the worker model entirely -- not logged
			 * as a resolved iteration, the driver must reject this outright,
			 * not treat it as zero-progress backpressure. */
			const uint8_t c[1] = { 0xAAu };
			stage_reply(ALP_CC3501E_RESP_OK_LEGACY, c, 1u);
			break;
		}

		/* Step 1 (mutant-control doc comment above): let a PENDING job that
		 * has reached its own ready time become TERMINAL and fill the reply
		 * cache -- by seq, status AND reply, success or a decoded ERR alike --
		 * regardless of what seq THIS dispatch itself carries. Mirrors
		 * worker_execute() publishing into
		 * protocol_sock_send_on_worker_complete() the instant a job
		 * completes, independent of who is polling at that moment. Logged
		 * HERE, using the completing job's OWN captured seq/dl/payload (NOT
		 * this dispatch's, which may belong to a different, later request)
		 * -- exactly once per genuine resolution, regardless of how many
		 * later cache hits re-serve it. */
		if (g_sock_send_worker_pending && alp_uptime_ms() >= g_sock_send_worker_ready_ms) {
			g_sock_send_worker_pending          = false;
			g_sock_send_worker_sitting_terminal = true;
			g_sock_send_cache_valid             = true;
			g_sock_send_cache_seq               = g_sock_send_worker_seq;
			g_sock_send_cache_status            = (g_sock_send_worker_resolve_status != 0u)
			                                          ? g_sock_send_worker_resolve_status
			                                          : (uint8_t)ALP_CC3501E_RESP_OK;
			g_sock_send_cache_queued            = g_sock_send_worker_computed_queued;
			if (slave.sock_send_log_count < ARRAY_SIZE(slave.sock_send_seq_log)) {
				uint32_t i                     = slave.sock_send_log_count;
				slave.sock_send_seq_log[i]     = g_sock_send_worker_seq;
				slave.sock_send_datalen_log[i] = g_sock_send_worker_dl;
				uint16_t copy = (g_sock_send_worker_dl < sizeof(slave.sock_send_payload_log[i]))
				                    ? g_sock_send_worker_dl
				                    : (uint16_t)sizeof(slave.sock_send_payload_log[i]);
				memcpy(slave.sock_send_payload_log[i], g_sock_send_worker_payload, copy);
				slave.sock_send_log_count++;
			}
		}

		/* Step 2: a request whose seq matches a VALID cache entry invalidates
		 * nothing; a request whose seq does NOT match invalidates it
		 * (mirrors handle_sock_send()'s own seq check, before anything else). */
		if (g_sock_send_cache_valid && g_sock_send_cache_seq != seq) {
			g_sock_send_cache_valid = false;
		}

		if (g_sock_send_cache_valid) {
			/* Cache hit: reclaim a matching sitting-terminal job (frees the
			 * slot -- worker_reclaim_matching_terminal) and answer straight
			 * from the cache, success or a decoded ERR alike. No
			 * re-execution, no re-log -- step 1 already logged this
			 * resolution once, whether that happened on THIS dispatch or an
			 * earlier one. */
			if (g_sock_send_worker_sitting_terminal && g_sock_send_worker_seq == seq) {
				g_sock_send_worker_sitting_terminal = false;
			}
			if (g_sock_send_cache_status == (uint8_t)ALP_CC3501E_RESP_OK) {
				const uint8_t c[2] = { (uint8_t)(g_sock_send_cache_queued & 0xFFu),
					                   (uint8_t)((g_sock_send_cache_queued >> 8) & 0xFFu) };
				stage_reply(ALP_CC3501E_RESP_OK, c, 2u);
			} else {
				stage_status(g_sock_send_cache_status);
			}
			break;
		}

		/* Step 3: cache miss. Discard a SITTING TERMINAL job whose seq
		 * differs (worker_discard_stale_terminal) -- freeing the slot -- but
		 * leave a QUEUED/RUNNING job alone regardless of seq: there is no
		 * terminal result yet for it to misclaim. */
		if (g_sock_send_worker_sitting_terminal && g_sock_send_worker_seq != seq) {
			g_sock_send_worker_sitting_terminal = false;
		}

		/* Step 4a: a DIFFERENT job is still QUEUED/RUNNING -- BUSY, submit
		 * NOTHING (worker_submit_payload refuses a non-IDLE slot). This is
		 * the case a bare clock-threshold fake cannot represent at all. */
		if (g_sock_send_worker_pending) {
			stage_status(ALP_CC3501E_RESP_ERR_BUSY);
			break;
		}

		/* Step 4b: the slot is genuinely IDLE -- SUBMIT. Compute the result
		 * exactly ONCE (bumping sock_send_body_exec_count so a test can
		 * prove that), capture this frame's OWN dl/payload for step 1's later
		 * logging, arm its ready time, and answer BUSY unconditionally --
		 * real firmware never resolves a brand-new job on its own submitting
		 * poll. */
		uint32_t exec_idx = slave.sock_send_body_exec_count;
		uint16_t computed = dl;
		if (g_sock_send_always_zero) {
			computed = 0u;
		} else if (exec_idx < g_sock_send_queue_plan_len) {
			computed = g_sock_send_queue_plan[exec_idx];
			if (computed > dl) computed = dl;
		} else if (g_sock_send_busy_step_count != 0u) {
			/* No explicit plan alongside busy_step -- default to exactly 1
			 * byte per submission, so a multi-entry busy_step array can
			 * require the DECLARED data_len to shrink across genuinely
			 * separate iterations (an explicit plan above still wins). */
			computed = (dl > 1u) ? 1u : dl;
		}
		slave.sock_send_body_exec_count++;

		uint32_t step_ms = 0u;
		if (g_sock_send_busy_step_count != 0u) {
			uint32_t step_idx = (exec_idx < g_sock_send_busy_step_count)
			                        ? exec_idx
			                        : g_sock_send_busy_step_count - 1u;
			step_ms           = g_sock_send_busy_step_ms[step_idx];
		}
		g_sock_send_worker_pending         = true;
		g_sock_send_worker_seq             = seq;
		g_sock_send_worker_computed_queued = computed;
		g_sock_send_worker_resolve_status  = g_sock_send_resolve_as_error;
		g_sock_send_worker_ready_ms        = alp_uptime_ms() + step_ms;
		g_sock_send_worker_dl              = dl;
		{
			uint16_t copy = (dl < sizeof(g_sock_send_worker_payload))
			                    ? dl
			                    : (uint16_t)sizeof(g_sock_send_worker_payload);
			memcpy(g_sock_send_worker_payload, &slave.req_pl[8], copy);
		}
		if (g_lock_contention_arm_on_submit) {
			/* See the global's own comment: arm the NEXT alp_delay_ms() call
			 * to force the lock externally held, not this dispatch -- that
			 * would just be undone by this SAME transaction's own
			 * cc3501e_lock_release() afterward. */
			g_lock_contention_arm_on_submit = false;
			g_lock_contention_pending       = true;
		}
		stage_status(ALP_CC3501E_RESP_ERR_BUSY);
		break;
	}
	case ALP_CC3501E_CMD_SOCK_RECV: {
		if (g_sock_recv_busy_polls_remaining > 0u) {
			g_sock_recv_busy_polls_remaining--;
			stage_status(ALP_CC3501E_RESP_ERR_BUSY);
			break;
		}
		if (!g_sock_recv_use_ring_model) {
			/* reply DATA = sock_addr(20) | data_len(LE16) | reserved(2) | data[]. */
			static const uint8_t payload[5] = { 'h', 'e', 'l', 'l', 'o' };
			uint8_t              d[24 + 5];
			memset(d, 0, sizeof(d));
			d[20] = (uint8_t)sizeof(payload); /* data_len lo */
			d[21] = 0u;                       /* data_len hi */
			memcpy(&d[24], payload, sizeof(payload));
			stage_reply(ALP_CC3501E_RESP_OK, d, (uint16_t)sizeof(d));
			break;
		}
		/* alp-sdk#2108 lazy-commit replay model -- see the mutant-control
		 * globals' own comment above slave_reset() for the full algorithm.
		 *
		 * The key is recorded on EVERY dispatch below, replay or commit
		 * alike (matching the pending firmware change this fake models --
		 * see the globals' comment), and the reply is recomputed FRESH from
		 * the source every time rather than replayed from a separate cache:
		 * a REPLAY re-serves from where the ORIGINAL commit started, capped
		 * by the SAME g_sock_recv_chunk_len "how much has genuinely arrived
		 * so far" limit a commit applies -- by default that reproduces the
		 * exact same bytes as the original commit, byte for byte. A test can
		 * still make a replay expose MORE than the original commit did by
		 * raising g_sock_recv_chunk_len (or g_sock_recv_source_len) between
		 * the original dispatch and the replay, simulating more genuinely
		 * becoming available in between -- the firmware is not re-consuming
		 * the socket on a replay, only re-reporting what is unread at that
		 * position, so a raised limit can expose more of it. */
		const uint16_t handle = (uint16_t)slave.req_pl[0] | ((uint16_t)slave.req_pl[1] << 8);
		const uint16_t want   = (uint16_t)slave.req_pl[2] | ((uint16_t)slave.req_pl[3] << 8);
		const uint8_t  seq =
		    (uint8_t)((slave.flags >> ALP_CC3501E_FLAG_REQ_SEQ_SHIFT) & ALP_CC3501E_REQ_SEQ_MASK);
		const bool is_replay = g_sock_recv_last_valid && seq == g_sock_recv_last_seq &&
		                       handle == g_sock_recv_last_handle;
		/* A REPLAY re-serves from where the ORIGINAL commit started --
		 * g_sock_recv_ring_pos has already moved PAST that point (a commit
		 * advances it immediately, win or lose on the wire), so re-reading
		 * it here would silently skip to the NEXT chunk instead of
		 * re-reporting the one this replay is supposed to recover. */
		const size_t serve_from = is_replay ? g_sock_recv_last_commit_pos : g_sock_recv_ring_pos;
		const size_t avail      = g_sock_recv_source_len - serve_from;
		size_t       serve      = ((size_t)want < avail) ? (size_t)want : avail;
		if (serve > g_sock_recv_chunk_len) serve = g_sock_recv_chunk_len;
		if (!is_replay) {
			g_sock_recv_last_commit_pos = serve_from;
			g_sock_recv_ring_pos += serve;
			g_sock_recv_commit_count++;
		}
		g_sock_recv_last_valid  = true;
		g_sock_recv_last_seq    = seq;
		g_sock_recv_last_handle = handle;

		uint8_t d[24 + sizeof(g_sock_recv_source)];
		memset(d, 0, sizeof(d));
		d[20] = (uint8_t)(serve & 0xFFu);
		d[21] = (uint8_t)((serve >> 8) & 0xFFu);
		memcpy(&d[24], &g_sock_recv_source[serve_from], serve);
		stage_reply(ALP_CC3501E_RESP_OK, d, (uint16_t)(24u + serve));
		if (g_sock_recv_corrupt_crc_remaining > 0u) {
			g_sock_recv_corrupt_crc_remaining--;
			slave.reply_pl[slave.reply_len - 1u] ^= 0xFFu; /* flip the CRC trailer's high byte */
		}
		break;
	}
	case ALP_CC3501E_CMD_GPIO_CONFIGURE:
		/* Accept; the pin model needs no state change on configure. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	case ALP_CC3501E_CMD_GPIO_WRITE: {
		uint8_t pad = slave.req_pl[0];
		if (pad < sizeof(slave.pin_level)) {
			slave.pin_level[pad] = slave.req_pl[1] ? 1u : 0u;
		}
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	}
	case ALP_CC3501E_CMD_GPIO_READ: {
		uint8_t pad = slave.req_pl[0];
		uint8_t lvl = (pad < sizeof(slave.pin_level)) ? slave.pin_level[pad] : 0u;
		stage_reply(ALP_CC3501E_RESP_OK, &lvl, 1u);
		break;
	}
	case ALP_CC3501E_CMD_SPI1_CONFIGURE: {
		/* reply DATA = alp_cc3501e_spi1_config_resp_t { freq_hz(LE32) |
		 * max_xfer(LE16) | bits_per_word | reserved }.  Reports the request
		 * back as the "actual" rate (no divider to quantise here) and this
		 * family's real chunk cap, exactly what CONFIGURE hands a real host. */
		const uint32_t freq_hz = (uint32_t)slave.req_pl[0] | ((uint32_t)slave.req_pl[1] << 8) |
		                         ((uint32_t)slave.req_pl[2] << 16) |
		                         ((uint32_t)slave.req_pl[3] << 24);
		const uint8_t d[8] = {
			(uint8_t)(freq_hz & 0xFFu),
			(uint8_t)((freq_hz >> 8) & 0xFFu),
			(uint8_t)((freq_hz >> 16) & 0xFFu),
			(uint8_t)((freq_hz >> 24) & 0xFFu),
			(uint8_t)(CC3501E_SPI1_MAX_XFER_V4 & 0xFFu),
			(uint8_t)((CC3501E_SPI1_MAX_XFER_V4 >> 8) & 0xFFu),
			slave.req_pl[5], /* bits_per_word echoed back */
			0u,
		};
		stage_reply(ALP_CC3501E_RESP_OK, d, 8u);
		break;
	}
	case ALP_CC3501E_CMD_SPI1_TRANSFER: {
		/* Software model of the real stub HAL (hal/cc3501e_hw_stub.c in the
		 * firmware repo): a wire loop, MOSI tied straight to MISO, so a test
		 * that clocks bytes out gets those same bytes back.  Self-delimiting
		 * on the request's own len/flags/seq, same as the real firmware. */
		const uint16_t len     = (uint16_t)slave.req_pl[0] | ((uint16_t)slave.req_pl[1] << 8);
		const uint8_t  flags   = slave.req_pl[2];
		const uint8_t  seq     = slave.req_pl[3];
		const uint8_t  tx_fill = slave.req_pl[4];
		const bool     no_rx   = (flags & ALP_CC3501E_SPI1_XFER_NO_RX) != 0u;
		const bool     no_tx   = (flags & ALP_CC3501E_SPI1_XFER_NO_TX) != 0u;
		uint8_t        d[4u + ALP_CC3501E_SPI1_MAX_XFER];

		d[0] = no_rx ? 0u : (uint8_t)(len & 0xFFu);
		d[1] = no_rx ? 0u : (uint8_t)((len >> 8) & 0xFFu);
		d[2] = (uint8_t)(flags & ALP_CC3501E_SPI1_XFER_CS_HOLD); /* echo of requested CS_HOLD */
		d[3] = g_spi1_reply_bad_seq ? (uint8_t)(seq + 1u) : seq; /* #g_spi1_reply_bad_seq mutant */
		if (!no_rx) {
			if (!no_tx) {
				memcpy(&d[4], &slave.req_pl[8], len);
			} else {
				memset(&d[4], tx_fill, len);
			}
		}
		stage_reply(ALP_CC3501E_RESP_OK, d, (uint16_t)(4u + (no_rx ? 0u : len)));
		break;
	}
	case ALP_CC3501E_CMD_SPI1_RELEASE:
		/* Argless escape hatch: bare OK, same as the real firmware. */
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	case ALP_CC3501E_CMD_GET_CAPABILITIES: {
		/* reply DATA = alp_cc3501e_capabilities_t { caps(LE32) | reserved(LE32) }. */
		if (g_caps_reply_short) {
			/* Fewer than 4 data bytes -- the host must treat this as a wire
			 * gap (ALP_ERR_IO), not decode a truncated bitmap.  Deliberately
			 * the LEGACY (RESP_OK_LEGACY, unpadded) shape: a real MAJOR-4
			 * reply is ALWAYS padded to an ALP_CC3501E_REPLY_PAD multiple
			 * (cc3501e_reply_model.h), so a genuinely short wire reply -- data
			 * narrower than the 4-byte bitmap -- is not a shape a MAJOR-4
			 * firmware can produce at all; this case tests the SHORT-DATA
			 * guard, not CRC framing. */
			const uint8_t d[2] = { 0xAAu, 0xBBu };
			stage_reply(ALP_CC3501E_RESP_OK_LEGACY, d, 2u);
			break;
		}
		const uint8_t d[8] = {
			(uint8_t)(g_caps_override_value & 0xFFu),
			(uint8_t)((g_caps_override_value >> 8) & 0xFFu),
			(uint8_t)((g_caps_override_value >> 16) & 0xFFu),
			(uint8_t)((g_caps_override_value >> 24) & 0xFFu),
			0u,
			0u,
			0u,
			0u, /* reserved, always 0 in this revision */
		};
		stage_reply(ALP_CC3501E_RESP_OK, d, 8u);
		break;
	}
	default:
		stage_status(ALP_CC3501E_RESP_ERR_INVALID);
		break;
	}
}

/* ---- test doubles for the alp_* seams the host driver links against -------- */

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) {
		return ALP_OK;
	}
	/* #2126: a genuinely dead link -- see g_all_io_down's own comment.
	 * Checked before every per-opcode down-window below, same PRE-DECODE
	 * shape (fails outright, so rx_scratch[0] is left poisoned to
	 * ALP_CC3501E_RX_SCRATCH_NO_STATUS -- exactly the signal
	 * cc3501e_link_check_and_recover() probes on). */
	if (slave.phase == PH_REQ_HDR && g_all_io_down) {
		return ALP_ERR_IO;
	}
	/* #2126 review: time-windowed silence -- see g_deaf_from_ms/
	 * g_deaf_until_ms's own comment. Same PRE-DECODE shape. */
	if (slave.phase == PH_REQ_HDR && alp_uptime_ms() >= g_deaf_from_ms &&
	    alp_uptime_ms() < g_deaf_until_ms) {
		return ALP_ERR_IO;
	}
	/* #2126 review: CMD_PING-only outright failure -- see g_ping_always_fails's
	 * own comment above. */
	if (slave.phase == PH_REQ_HDR && g_ping_always_fails &&
	    tx[0] == (uint8_t)ALP_CC3501E_CMD_PING) {
		return ALP_ERR_IO;
	}
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_STATUS) {
		slave.wifi_status_attempt_count++;
	}
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_STATUS &&
	    g_status_io_down_remaining > 0u) {
		g_status_io_down_remaining--;
		return ALP_ERR_IO;
	}
	/* #2035: same shape as g_status_io_down_remaining above, for
	 * WIFI_GET_IP -- fails the transaction PRE-DECODE, so no status byte is
	 * ever decoded and rx_scratch[0] is left poisoned to
	 * ALP_CC3501E_RX_SCRATCH_NO_STATUS. */
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_WIFI_GET_IP &&
	    g_get_ip_io_down_remaining > 0u) {
		g_get_ip_io_down_remaining--;
		return ALP_ERR_IO;
	}
	/* #1371: fail a GET_VERSION transaction outright -- models the CC3501E's
	 * documented Puya cold-boot flash bug (chips/cc3501e/cc3501e_core.c's
	 * cc3501e_hard_reset comment), where the slave has not armed its SPI yet
	 * and the request never lands. */
	if (slave.phase == PH_REQ_HDR && tx[0] == ALP_CC3501E_CMD_GET_VERSION &&
	    g_get_version_io_down_remaining > 0u) {
		g_get_version_io_down_remaining--;
		return ALP_ERR_IO;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd   = tx[0];
		slave.flags = tx[1];
		if (slave.cmd_log_count < sizeof(slave.cmd_log)) {
			slave.cmd_log[slave.cmd_log_count] = slave.cmd;
		}
		slave.cmd_log_count++;
		if (slave.flags_log_count < sizeof(slave.flags_log)) {
			slave.flags_log[slave.flags_log_count] = tx[1];
		}
		slave.flags_log_count++;
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (rx != NULL) {
			/* #2136: g_req_hdr_desynced models a slave mid-payload (0x00,
			 * not the armed marker) instead of the normal armed-idle
			 * pattern. */
			memset(rx, g_req_hdr_desynced ? 0x00u : ALP_CC3501E_SYNC_IDLE, len);
		}
		if (slave.req_len > 0u) {
			slave.phase = PH_REQ_PL;
		} else {
			slave_dispatch();
			slave.phase = PH_REPLY_HDR;
		}
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave_dispatch();
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		/* #2136: g_reply_hdr_deaf models a slave that armed the request-header
		 * phase but never dispatched a reply -- the header stays parked at
		 * the idle marker instead of echoing the request opcode. */
		if (g_reply_hdr_deaf) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		} else {
			rx[0] = slave.cmd; /* reply header echoes the cmd */
			rx[1] = 0x00u;     /* solicited */
			rx[2] = (uint8_t)(slave.reply_len & 0xFFu);
			rx[3] = (uint8_t)((slave.reply_len >> 8) & 0xFFu);
		}
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		memcpy(rx, slave.reply_pl, len);
		slave.phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

/* Forward reference -- the real fixture is declared below, in its usual spot
 * (the "fixture" section); this lets the lock-contention hooks in
 * alp_delay_ms() below reach ctx->request_lock directly (the REAL lock seam,
 * cc3501e_lock_acquire()'s cc3501e_lock_try(), cc3501e_core.c) without
 * reordering the file. A file-scope `static` tentative declaration + later
 * definition is one object, not two, per C's tentative-definition rules. */
static cc3501e_t fw;

/* alp_delay_us is a no-op under the sim; the GPIO seams are inert (the
 * fixture's ctx leaves reset/enable/ready pins unset, so the wrappers under
 * test never call them -- they exercise cc3501e_request, not the reset-pin
 * pulse). alp_delay_ms and alp_uptime_ms share one fake millisecond counter
 * (same pattern as tests/zephyr/cc3501e_poll_deadline): poll_by_repeat()'s
 * deadline (issue #1953) is what bounds test_wifi_status_gives_up_after_
 * the_down_window_1377's retry to a real ALP_ERR_TIMEOUT rather than an
 * infinite spin, without any real sleeping. */
static uint64_t g_fake_now_ms;

/* Fake delay accounting for the reply-header gate (scaled by the real
 * expected byte count since the "size the reply-header gate by the expected
 * reply length" fix): every alp_delay_us() call this run is logged in order,
 * so a test can find its own gate's fallback_us without a real clock, plus a
 * running total (g_fake_delay_us_total) for a test that only cares about the
 * sum. Cleared by delay_log_reset() at the top of each test that inspects
 * either. */
#define DELAY_LOG_CAP 8u
static uint32_t g_delay_us_log[DELAY_LOG_CAP];
static size_t   g_delay_us_count;
static uint64_t g_fake_delay_us_total;

static void delay_log_reset(void)
{
	g_delay_us_count      = 0u;
	g_fake_delay_us_total = 0u;
}

void alp_delay_us(uint32_t us)
{
	g_fake_delay_us_total += us;
	if (g_delay_us_count < DELAY_LOG_CAP) {
		g_delay_us_log[g_delay_us_count] = us;
	}
	g_delay_us_count++;
}
void alp_delay_ms(uint32_t ms)
{
	g_fake_now_ms += ms;
	if (g_lock_contention_pending) {
		g_lock_contention_pending       = false;
		fw.request_lock                 = true;
		g_lock_contention_release_at_ms = g_fake_now_ms + LOCK_CONTENTION_WINDOW_MS;
	} else if (g_lock_contention_release_at_ms != 0u) {
		if (g_fake_now_ms >= g_lock_contention_release_at_ms) {
			fw.request_lock                 = false;
			g_lock_contention_release_at_ms = 0u;
		} else {
			fw.request_lock = true;
		}
	}
}
uint64_t alp_uptime_ms(void)
{
	return g_fake_now_ms;
}
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	return NULL;
}
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	/* #2126: model a warm nRESET pulse as what cures g_all_io_down -- the
	 * RELEASE edge (level == true) on the ctx's OWN reset_pin is
	 * cc3501e_hard_reset()'s "let the module re-boot" step. Harmless no-op
	 * on every OTHER test (g_all_io_down is false by default, and most
	 * tests never set fw.reset_pin at all, per this function's usual
	 * ALP_ERR_NOSUPPORT contract below, which callers that DO care about
	 * gpio failure already rely on). */
	if (level && pin == fw.reset_pin) {
		g_reset_release_count++;
		/* #2126 review: g_heal_disabled models a fault a warm reset
		 * genuinely cannot fix -- see its own comment above. */
		if (!g_heal_disabled) g_all_io_down = false;
	}
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw; /* opaque, non-NULL; the stub ignores it */

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	g_lock_contention_arm_on_submit = false;
	g_lock_contention_pending       = false;
	g_lock_contention_release_at_ms = 0u;
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
}

/* ================================ META ===================================== */

ZTEST(cc3501e_host_driver, test_ping_encodes_opcode)
{
	zassert_equal(cc3501e_ping(&fw), ALP_OK, "PING -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_PING, "opcode 0x00 reached the slave");
	zassert_equal(slave.req_len, 0u, "PING carries no payload");
}

ZTEST(cc3501e_host_driver, test_soft_reset_encodes_opcode)
{
	zassert_equal(cc3501e_soft_reset(&fw), ALP_OK, "RESET -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_RESET, "opcode 0x02 reached the slave");
}

/* ---- reply-header gate scaled by the real expected byte count ------------- *
 *
 * cc3501e_reply_header_gate_us() (cc3501e_core.c) replaced the reply-header
 * phase's flat 200 us wait with one scaled by max(expected reply,
 * wire_tx_len) -- NOT by rx_cap, which is a defensive local staging-buffer
 * size, not the bridge's own reply size (cc3501e_sock_recv() always passes
 * rx_cap == sizeof(ctx->sock_buf) == ALP_CC3501E_MAX_PAYLOAD no matter how
 * few bytes it actually asked for). See cc3501e_expected_reply_bytes()'s own
 * comment in cc3501e_core.c for the full derivation and the bench data:
 * SOCK_RECV want 512 (reply 536 B) proven fine at 200 us; want 4071 (the
 * bridge's true wire reply is capped at 4093 B, not the naive 24 + 4071 =
 * 4095 B -- see that comment) wedged the link at 200 us and was proven fine
 * at 2000 us; a STREAM_WRITE *request* of 512 B was clean at 200 us while
 * 1024 B and 4092 B both failed (examples/aen/aen-cc3501e-socket-throughput/
 * README.md ~159-165) -- CONSISTENT WITH (not itself proof of a fix for)
 * why large REQUESTS gate too, via wire_tx_len.
 *
 * ctx->ready_pin is NULL in this fixture (alp_gpio_open stub always returns
 * NULL), so cc3501e_reply_gate() takes its unconditional
 * alp_delay_us(fallback_us) fallback path with no GPIO polling in between --
 * exactly the path a CS-less r1 board, or any board whose READY pin never
 * proves itself, runs -- so the logged alp_delay_us() calls line up 1:1 with
 * the gate's phases.  (On dev without alp-sdk#2105, a READY pin that reads
 * CONSTANT HIGH -- a stuck-high pad, not a wired-and-idle one -- proves
 * itself on the very first read and then bypasses this fallback gate
 * entirely, level-gating instead; this fix only changes behaviour on the
 * fixed-wait path exercised here, until #2105 lands.)
 *
 * A request with NO TX payload phase (tx_len == 0, e.g. PING or
 * GET_PENDING_EVENTS) logs exactly 3 delays: [request-header, reply-header,
 * reply-payload] -- index 1 is the reply-header gate under test.  A request
 * WITH a payload phase (tx_len > 0, e.g. SOCK_RECV, STREAM_WRITE,
 * SPI1_TRANSFER) logs 4: [request-header, request-payload, reply-header,
 * reply-payload] -- index 2 is the one under test. */

ZTEST(cc3501e_host_driver, test_reply_gate_ping_small_reply_cap_stays_at_proven_floor)
{
	/* PING isn't SOCK_RECV/SPI1_TRANSFER/GET_PENDING_EVENTS, so
	 * cc3501e_expected_reply_bytes() falls back to rx_cap here -- exercising
	 * that conservative default path, not the special-cased ones below. */
	uint8_t reply[8] = { 0 };
	size_t  got      = 0u;
	delay_log_reset();
	alp_status_t s =
	    cc3501e_request(&fw, ALP_CC3501E_CMD_PING, NULL, 0, reply, sizeof(reply), &got, 100u);
	zassert_equal(s, ALP_OK, "PING with an 8 B reply cap -> OK");
	zassert_equal(g_delay_us_count, 3u, "3 gated phases for a header-only (no TX payload) request");
	zassert_equal(
	    g_delay_us_log[1], 200u, "rx_cap 8 B (<= 536 B) stays at the proven 200 us floor");
}

ZTEST(cc3501e_host_driver, test_reply_gate_ping_max_payload_reply_cap_reaches_proven_2000us)
{
	static uint8_t reply[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0u;
	memset(reply, 0, sizeof(reply));
	delay_log_reset();
	alp_status_t s =
	    cc3501e_request(&fw, ALP_CC3501E_CMD_PING, NULL, 0, reply, sizeof(reply), &got, 100u);
	zassert_equal(s, ALP_OK, "PING with a 4096 B (MAX_PAYLOAD) reply cap -> OK");
	zassert_equal(g_delay_us_count, 3u, "3 gated phases for a header-only (no TX payload) request");
	zassert_equal(g_delay_us_log[1],
	              2000u,
	              "rx_cap 4096 B is above the 4092 B clamp point, so it reaches the proven "
	              "2000 us ceiling");
}

ZTEST(cc3501e_host_driver, test_reply_gate_interpolates_between_the_two_measured_points)
{
	/* 2314 B sits exactly halfway between the 536 B floor and the 4092 B
	 * ceiling anchors -- expect the gate exactly halfway between 200 us and
	 * 2000 us (1100 us).  This size was never bench-measured; the formula
	 * interpolates it linearly (see cc3501e_expected_reply_bytes()'s and
	 * cc3501e_reply_header_gate_us()'s comments in cc3501e_core.c). */
	static uint8_t reply[2314];
	size_t         got = 0u;
	memset(reply, 0, sizeof(reply));
	delay_log_reset();
	alp_status_t s =
	    cc3501e_request(&fw, ALP_CC3501E_CMD_PING, NULL, 0, reply, sizeof(reply), &got, 100u);
	zassert_equal(s, ALP_OK, "PING with a 2314 B reply cap -> OK");
	zassert_equal(g_delay_us_log[1],
	              1100u,
	              "the exact byte-count midpoint interpolates to the gate midpoint");
}

/* ---- the real key: SOCK_RECV's `want`, not its fixed rx_cap ---------------- *
 *
 * Mutation check: reverting cc3501e_expected_reply_bytes()'s SOCK_RECV
 * special case (falling back to rx_cap, which cc3501e_sock_recv() always
 * sets to sizeof(ctx->sock_buf) == 4096) turns the want-512 test below RED --
 * it would see the 2000 us ceiling instead of the 200 us floor. */
ZTEST(cc3501e_host_driver, test_sock_recv_want_512_gates_floor)
{
	uint8_t buf[512];
	size_t  recv_len = 0u;
	delay_log_reset();
	alp_status_t s = cc3501e_sock_recv(&fw, 1u, buf, sizeof(buf), &recv_len, 100u);
	zassert_equal(s, ALP_OK, "SOCK_RECV want=512 -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_RECV, "opcode 0x23 reached the slave");
	zassert_equal(
	    g_delay_us_count, 4u, "4 gated phases (SOCK_RECV always carries a request payload)");
	zassert_equal(g_delay_us_log[2],
	              200u,
	              "want=512 -> expected reply 536 B (24 B header + 512), the proven floor -- NOT "
	              "the 2000 us the buggy rx_cap key would have given");
}

ZTEST(cc3501e_host_driver, test_sock_recv_want_4071_gates_ceiling)
{
	static uint8_t buf[4071];
	size_t         recv_len = 0u;
	delay_log_reset();
	alp_status_t s = cc3501e_sock_recv(&fw, 1u, buf, sizeof(buf), &recv_len, 100u);
	zassert_equal(s, ALP_OK, "SOCK_RECV want=4071 -> OK");
	zassert_equal(
	    g_delay_us_count, 4u, "4 gated phases (SOCK_RECV always carries a request payload)");
	zassert_true(g_delay_us_log[2] >= 2000u,
	             "want=4071 -> expected reply (24 + 4071) B is above the 4092 B clamp point, "
	             "reaching the proven 2000 us ceiling");
}

/* Mutation check: reverting cc3501e_expected_reply_bytes()'s want == 0 special
 * case (falling through to the general 24 + want = 24 formula) turns this
 * test RED -- it would see the 200 us floor instead of the ceiling a
 * possibly-4093-B reply needs. */
ZTEST(cc3501e_host_driver, test_sock_recv_want_zero_gates_ceiling)
{
	size_t recv_len = 0u;
	delay_log_reset();
	/* cap == 0 -> cc3501e_sock_recv() sends max_len == 0 on the wire literally
	 * (no floor). The bridge (protocol_sockets.c) treats max_len == 0 as "no
	 * limit", not "expect nothing" -- the reply can be as large as the frame
	 * allows, so this must gate like a large reply, not like a tiny one. */
	alp_status_t s = cc3501e_sock_recv(&fw, 1u, NULL, 0u, &recv_len, 100u);
	zassert_equal(s, ALP_OK, "SOCK_RECV cap=0 -> OK");
	zassert_equal(
	    g_delay_us_count, 4u, "4 gated phases (SOCK_RECV always carries a request payload)");
	zassert_true(g_delay_us_log[2] >= 2000u,
	             "want=0 means 'no limit' to the firmware -- falls back to the driver's own "
	             "sock_buf rx_cap (4096 B), reaching the proven 2000 us ceiling");
}

/* ---- large REQUESTS need the long gate too (dispatch_frame CRCs + handles
 * the request in the SAME ISR before arming the reply header) --------------- *
 *
 * Mutation check: reverting the call site to gate on cc3501e_expected_
 * reply_bytes() alone (dropping the max() against wire_tx_len) turns this
 * test RED -- STREAM_WRITE's own expected reply is 0 (rx_cap 0, no special
 * case), so it would see the 200 us floor instead of the 2000 us ceiling a
 * 4092 B request needs. */
ZTEST(cc3501e_host_driver, test_stream_write_large_request_gates_ceiling)
{
	static uint8_t data[ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES]; /* 4092 B, the max
	                                                                          * cc3501e_stream_write
	                                                                          * accepts. */
	memset(data, 0xAA, sizeof(data));
	delay_log_reset();
	alp_status_t s = cc3501e_stream_write(&fw, data, sizeof(data));
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_STREAM_WRITE, "opcode reached the slave");
	zassert_equal(g_delay_us_count, 4u, "4 gated phases (a 4092 B request has a payload phase)");
	zassert_true(g_delay_us_log[2] >= 2000u,
	             "a 4092 B request reaches the proven 2000 us ceiling via wire_tx_len");
	(void)s; /* STREAM_WRITE isn't modelled by slave_dispatch's switch (falls to its
	          * default RESP_ERR_INVALID); only the gate timing is under test here. */
}

/* ---- SPI1_TRANSFER: expected reply is derived from len/NO_RX, not rx_cap -- */
ZTEST(cc3501e_host_driver, test_spi1_transfer_large_no_rx_tx_gates_by_tx_size)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE before TRANSFER");
	static uint8_t tx[ALP_CC3501E_SPI1_MAX_XFER]; /* 4088 B, the max single chunk. */
	memset(tx, 0x55, sizeof(tx));
	delay_log_reset();
	/* rx == NULL sets the NO_RX flag: expected reply collapses to the 4 B
	 * resp header alone, so only wire_tx_len (8 B header + 4088 B TX = 4096)
	 * can be what pushes this to the ceiling. */
	alp_status_t s = cc3501e_spi1_transfer(&fw, tx, NULL, (uint16_t)sizeof(tx), 0u, false, 100u);
	zassert_equal(s, ALP_OK, "SPI1_TRANSFER (NO_RX, 4088 B TX) -> OK");
	zassert_equal(
	    g_delay_us_count, 4u, "4 gated phases (a TX-bearing transfer has a payload phase)");
	zassert_true(g_delay_us_log[2] >= 2000u,
	             "a 4096 B request (8 B header + 4088 B TX) reaches the proven 2000 us ceiling "
	             "even though the reply itself is NO_RX-tiny");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_small_read_gates_floor)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE before TRANSFER");
	uint8_t tx[8] = { 0 };
	uint8_t rx[8] = { 0 };
	delay_log_reset();
	alp_status_t s = cc3501e_spi1_transfer(&fw, tx, rx, sizeof(tx), 0u, false, 100u);
	zassert_equal(s, ALP_OK, "SPI1_TRANSFER (8 B, RX wanted) -> OK");
	zassert_equal(g_delay_us_count, 4u, "4 gated phases (an 8 B TX has a payload phase)");
	zassert_equal(g_delay_us_log[2],
	              200u,
	              "expected reply 12 B (4 B header + 8 B RX), wire_tx_len 16 B -- both well under "
	              "the 536 B floor");
}

/* NO_TX + NO_RX together: the firmware clocks len dummy bytes each way (the
 * fill byte out, discarded in) with no inline TX array on the wire and no RX
 * data in the reply.  The request PAYLOAD phase still runs (the 8 B
 * alp_cc3501e_spi1_transfer_t header itself is always sent as this exchange's
 * tx_payload, NO_TX or not -- only the inline TX array beyond it is dropped),
 * so wire_tx_len is the 8 B header alone regardless of how large len is.
 * Nothing else in this suite exercises the NO_RX branch of cc3501e_
 * expected_reply_bytes() where it actually matters: every other NO_RX case
 * here also carries real inline TX bytes, so wire_tx_len alone already
 * dominates and would mask a broken NO_RX check.
 *
 * Mutation check: removing the NO_RX branch (always returning sizeof(resp) +
 * len instead) turns this test RED -- it would compute an expected reply of
 * 4 + 4088 = 4092 B (>= the ceiling) instead of the tiny 4 B a NO_RX reply
 * actually is, and 4092 B >= the 8 B wire_tx_len here too, so gate_bytes
 * would wrongly reach the 2000 us ceiling instead of the 200 us floor. */
ZTEST(cc3501e_host_driver, test_spi1_transfer_no_tx_no_rx_dummy_clock_gates_floor)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE before TRANSFER");
	delay_log_reset();
	alp_status_t s = cc3501e_spi1_transfer(
	    &fw, NULL, NULL, (uint16_t)ALP_CC3501E_SPI1_MAX_XFER, 0xFFu, false, 100u);
	zassert_equal(s, ALP_OK, "SPI1_TRANSFER (NO_TX|NO_RX, 4088 B dummy-clocked) -> OK");
	zassert_equal(g_delay_us_count,
	              4u,
	              "4 gated phases (the 8 B header itself is still a request-payload phase)");
	zassert_equal(g_delay_us_log[2],
	              200u,
	              "expected reply 4 B (NO_RX), wire_tx_len 8 B (header only, NO_TX) -- both well "
	              "under the 536 B floor even though len asks for 4088 B of dummy clocking");
}

/* ---- GET_PENDING_EVENTS falls back to rx_cap like every other opcode not
 * specifically known to over-report it (SOCK_RECV, SPI1_TRANSFER) -------------
 *
 * An earlier version of this fix special-cased this opcode to the firmware's
 * documented 16-entry event ring (288 B) to avoid taxing every poll with the
 * large-reply gate. That special case was removed: it depended on a
 * firmware-internal constant (cc3501e-bridge-firmware src/event_ring.h) this
 * driver has no way to verify stays 16, so a silent ring-depth change on the
 * firmware side would silently under-gate this poll again -- exactly the
 * class of bug this whole fix exists to close. rx_cap here is
 * sizeof(ctx->evt_buf) == ALP_CC3501E_MAX_PAYLOAD, so this poll now pays the
 * full ~2000 us ceiling every time -- measured at roughly 1.8 ms added per
 * poll, about 0.36% additional transport-lock hold at a realistic 2 polls/s
 * cadence, accepted as the cost of not trusting a number this file cannot
 * verify. */
static void gate_test_evt_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	(void)opcode;
	(void)payload;
	(void)len;
	(void)user;
}

ZTEST(cc3501e_host_driver, test_poll_events_gates_by_rx_cap)
{
	zassert_equal(
	    cc3501e_add_event_callback(&fw, gate_test_evt_cb, NULL), ALP_OK, "register a sink");
	delay_log_reset();
	alp_status_t s = cc3501e_poll_events(&fw);
	zassert_equal(
	    g_delay_us_count, 3u, "3 gated phases (GET_PENDING_EVENTS carries no request payload)");
	zassert_true(g_delay_us_log[1] >= 2000u,
	             "GET_PENDING_EVENTS falls back to rx_cap (evt_buf's 4096 B), like every other "
	             "opcode not specifically known to over-report it, reaching the proven ceiling");
	(void)s; /* GET_PENDING_EVENTS isn't modelled by slave_dispatch's switch (falls to its
	          * default RESP_ERR_INVALID); only the gate timing is under test here. */
}

ZTEST(cc3501e_host_driver, test_get_version_decodes_le16)
{
	uint16_t v = 0u;
	zassert_equal(cc3501e_get_version(&fw, &v), ALP_OK, "GET_VERSION -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_VERSION, "opcode 0x01");
	zassert_equal(v, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION, "decoded LE16 protocol version");
}

/* ---- #1371: cc3501e_reset()'s wire-protocol compatibility gate ------------- *
 *
 * DESIGN.md always claimed "host refuses a mismatch" for GET_VERSION; these
 * pin the gate that now makes that claim true, and its two required
 * non-effects: the #1116 concurrency suite drives cc3501e_get_version()
 * directly (never through cc3501e_reset()) against a modelled slave that
 * never claims ALP_CC3501E_PROTOCOL_VERSION, and the cold-boot liveness
 * soaks use cc3501e_get_version() as a bare round-trip probe -- neither may
 * regress from this gate living in cc3501e_reset() instead. */

/* Any non-NULL pointer -- alp_gpio_write() is stubbed ALP_ERR_NOSUPPORT and
 * its result is (void)-discarded by cc3501e_reset(), so these are never
 * dereferenced; they only need to be non-NULL to clear reset()'s "pins not
 * bound" gate. */
#define FAKE_RESET_PIN  ((alp_gpio_t *)&fw)
#define FAKE_ENABLE_PIN ((alp_gpio_t *)&slave)

ZTEST(cc3501e_host_driver, test_reset_accepts_matching_protocol_version_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	zassert_equal(cc3501e_reset(&fw), ALP_OK, "matching GET_VERSION -> reset succeeds");
	zassert_true(fw.initialised, "a matching version leaves the context usable");
}

ZTEST(cc3501e_host_driver, test_reset_refuses_protocol_version_mismatch_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* A genuine MAJOR mismatch (ADR 0033).  This test used to add 1 to the
	 * flat composed value; under MAJOR.MINOR that only bumps MINOR (0x0301 +
	 * 1 = 0x0302, still MAJOR 3) and must NOT refuse any more -- see
	 * test_reset_accepts_higher_minor_0033.  Bump MAJOR explicitly instead so
	 * this test keeps pinning what it always meant to: a wire disagreement
	 * this host cannot connect across. */
	g_get_version_override_active = true;
	g_get_version_override_value  = (uint16_t)(((uint16_t)(ALP_CC3501E_PROTOCOL_MAJOR + 1) << 8) |
	                                           (uint16_t)ALP_CC3501E_PROTOCOL_MINOR);

	zassert_equal(cc3501e_reset(&fw),
	              ALP_ERR_VERSION,
	              "GET_VERSION answered with a different MAJOR -> ALP_ERR_VERSION");
	zassert_false(fw.initialised, "a refused context is left uninitialised");

	/* The dead end this leaves behind, deliberately: once refused, EVERY
	 * later call (including re-reading the version for a diagnostic) fails
	 * ALP_ERR_NOT_READY rather than reporting a value nobody re-measured. */
	uint16_t v = 0xDEADu;
	zassert_equal(cc3501e_get_version(&fw, &v),
	              ALP_ERR_NOT_READY,
	              "cc3501e_get_version() does not keep working across a refusal");
}

ZTEST(cc3501e_host_driver, test_reset_tolerates_transport_failure_during_probe_1371)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* Models the CC3501E's documented Puya cold-boot flash bug: the FIRST
	 * boot's GET_VERSION never lands at all (transport failure), which is
	 * NOT a version verdict -- only an answered request can be compared, so
	 * this must stay non-fatal and leave the context usable for a caller's
	 * own hard-reset retry (examples/peripheral-io/alp-console's
	 * cc3501e_bridge_bringup 8-iteration soak; aen-cc3501e-gpio's liveness
	 * gate) -- exactly like it was before this context ever probed. */
	g_get_version_io_down_remaining = 1u;

	zassert_equal(cc3501e_reset(&fw),
	              ALP_OK,
	              "an unanswered GET_VERSION probe must not be treated as a refusal");
	zassert_true(fw.initialised, "an unanswered probe leaves the context usable for a retry");

	/* And the retry lands normally afterwards (the down-counter above is
	 * exhausted; the fixture reverts to its default matching reply). */
	uint16_t v = 0u;
	zassert_equal(cc3501e_get_version(&fw, &v), ALP_OK, "a following GET_VERSION works");
	zassert_equal(v, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION, "and reads the real value");
}

/* ---- ADR 0033: MAJOR.MINOR wire versioning --------------------------------
 *
 * cc3501e_reset()'s GET_VERSION gate now refuses ONLY on a MAJOR mismatch; a
 * MINOR difference in either direction is additive by definition and must
 * leave the link usable -- that split is the entire point of ADR 0033. */

ZTEST(cc3501e_host_driver, test_reset_accepts_lower_minor_0033)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* Same MAJOR, a MINOR one BELOW this host's -- an older, additive-only
	 * firmware that simply lacks a newer feature.  The pre-0033 flat-integer
	 * gate refused on ANY difference; this is the exact case that gate cost a
	 * customer a needless reflash for. */
	/* The minor byte must be truncated to uint8_t BEFORE the OR: MINOR is
	 * currently 0, so MINOR - 1 done at uint16_t width is 0xFFFF, which
	 * clobbers the MAJOR byte this composes it with too (0x0400 | 0xFFFF ==
	 * 0xFFFF, decoding MAJOR 0xFF -- an unrelated pre-existing bug this test
	 * never actually ran against real twister to catch). Truncating first
	 * gives the intended byte-wise wrap (0x00FF), composing cleanly with
	 * MAJOR into 0x04FF. */
	g_get_version_override_active = true;
	g_get_version_override_value  = (uint16_t)(((uint16_t)ALP_CC3501E_PROTOCOL_MAJOR << 8) |
	                                           (uint16_t)(uint8_t)(ALP_CC3501E_PROTOCOL_MINOR - 1));

	zassert_equal(
	    cc3501e_reset(&fw), ALP_OK, "a lower MINOR, same MAJOR, must not refuse the link");
	zassert_true(fw.initialised, "the context stays usable across a lower MINOR");
	zassert_equal(fw.fw_proto_major, (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR, "MAJOR recorded");
	zassert_equal(fw.fw_proto_minor,
	              (uint8_t)(ALP_CC3501E_PROTOCOL_MINOR - 1),
	              "the firmware's own (lower) MINOR is recorded, not this host's");
}

ZTEST(cc3501e_host_driver, test_reset_accepts_higher_minor_0033)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* Same MAJOR, a MINOR one ABOVE this host's -- a newer firmware carrying
	 * features this host does not use yet.  Also additive, also must connect. */
	g_get_version_override_active = true;
	g_get_version_override_value  = (uint16_t)(((uint16_t)ALP_CC3501E_PROTOCOL_MAJOR << 8) |
	                                           (uint16_t)(ALP_CC3501E_PROTOCOL_MINOR + 1));

	zassert_equal(
	    cc3501e_reset(&fw), ALP_OK, "a higher MINOR, same MAJOR, must not refuse the link");
	zassert_true(fw.initialised, "the context stays usable across a higher MINOR");
	zassert_equal(fw.fw_proto_minor,
	              (uint8_t)(ALP_CC3501E_PROTOCOL_MINOR + 1),
	              "the firmware's own (higher) MINOR is recorded");
}

ZTEST(cc3501e_host_driver, test_reset_refuses_legacy_raw_integer_0033)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	/* A pre-ADR-0033 firmware answers with its old raw v1..v9 integer -- e.g.
	 * 9 (0x0009), the last released flat value -- which decodes to MAJOR 0.
	 * MAJOR 0 is RESERVED (never a real release) and refuses like any other
	 * MAJOR mismatch, but recording it lets a caller tell "older than the
	 * versioning scheme" from "disagrees about the frame layout". */
	g_get_version_override_active = true;
	g_get_version_override_value  = 9u;

	zassert_equal(cc3501e_reset(&fw),
	              ALP_ERR_VERSION,
	              "a pre-0033 raw integer decodes to MAJOR 0 and is refused");
	zassert_false(fw.initialised, "a refused context is left uninitialised");
	zassert_equal(
	    fw.fw_proto_major, 0u, "MAJOR 0 marks 'older than the versioning scheme', not corrupt");
}

/* v4.0 (#2035): THE HOST IS BILINGUAL, deliberately -- a board still on 3.1
 * firmware must keep working until its own OTA (which rides this same host)
 * gets it to 4.0 (<alp/protocol/cc3501e.h>'s migration-order note above
 * ALP_CC3501E_PROTOCOL_MAJOR).  Poke ctx->fw_proto_major to the LEGACY value
 * directly (as if a prior cc3501e_reset() had already negotiated it) and
 * drive a real opcode against a reply in the actual MAJOR-3 shape --
 * ALP_CC3501E_RESP_OK_LEGACY (0x00), unpadded, no CRC trailer -- the frame a
 * real 3.1 firmware sends, not the MAJOR-4 shape every other test in this
 * file exercises. */
ZTEST(cc3501e_host_driver, test_ping_accepts_legacy_major3_bare_ok_shape_bilingual)
{
	fw.fw_proto_major = (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	g_bare_ok_legacy  = true;

	zassert_equal(cc3501e_ping(&fw), ALP_OK, "a legacy 0x00 bare-OK reply decodes fine on MAJOR 3");
}

/* #2035 review follow-up: the coverage gap that let two blockers ship.  No
 * test exercised a dead phase on the LEGACY wire for an opcode whose reply
 * carries real data beyond the status byte -- exactly the shape a bus that
 * dies mid-transaction clocks back (this repo's own silicon finding: a dead
 * link reads 0x00000000).  Stage that shape directly at the transport layer
 * via cc3501e_request(), same pattern as
 * test_connect_sta_dead_phase_alias_rejected_at_transport_1378 above.
 *
 * BLE_GATT_REGISTER (0x38) must REJECT it: <alp/protocol/cc3501e.h>
 * documents num_handles == num_chars, at least 1 on a real success, so
 * num_handles == 0 is never legitimate -- this all-zero payload can only be
 * the dead-phase alias.  Mutation check: removing
 * ALP_CC3501E_CMD_BLE_GATT_REGISTER from cc3501e_reply_carries_data()
 * (chips/cc3501e/cc3501e_core.c) reddens this test. */
ZTEST(cc3501e_host_driver, test_ble_gatt_register_dead_phase_legacy_rejected_2035)
{
	fw.fw_proto_major               = (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	g_force_dead_phase_zero_payload = true;

	const uint8_t descriptor[1] = { 0x01u };
	uint8_t       reply[8]      = { 0 };
	size_t        got           = 0u;
	alp_status_t  s             = cc3501e_request(&fw,
	                                              ALP_CC3501E_CMD_BLE_GATT_REGISTER,
	                                              descriptor,
	                                              sizeof(descriptor),
	                                              reply,
	                                              sizeof(reply),
	                                              &got,
	                                              100u);
	zassert_not_equal(s,
	                  ALP_OK,
	                  "an all-zero padded BLE_GATT_REGISTER reply on the legacy wire must not "
	                  "read as ALP_OK -- num_handles==0 is not a legitimate success shape (#2035)");
	zassert_equal(s, ALP_ERR_IO, "rejected as a transport error, not silently accepted");
}

/* WIFI_SCAN_START (0x10) and BLE_SCAN_START (0x34) are the opposite trade,
 * deliberately: the SAME all-zero padded reply must be ACCEPTED, because
 * "zero networks/peripherals in range" is a legitimate, unremarkable scan
 * result that reads back byte-identical to a dead phase -- shape alone
 * cannot tell them apart (see cc3501e_reply_may_be_all_zero()'s comment on
 * these two cases).  Mutation check: removing either opcode from
 * cc3501e_reply_may_be_all_zero() reddens its matching test below with
 * ALP_ERR_IO instead of ALP_OK. */
ZTEST(cc3501e_host_driver, test_wifi_scan_start_empty_legacy_accepted_2035)
{
	fw.fw_proto_major               = (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	g_force_dead_phase_zero_payload = true;

	uint8_t      reply[8] = { 0 };
	size_t       got      = 0u;
	alp_status_t s        = cc3501e_request(
	    &fw, ALP_CC3501E_CMD_WIFI_SCAN_START, NULL, 0u, reply, sizeof(reply), &got, 100u);
	zassert_equal(s,
	              ALP_OK,
	              "an all-zero padded WIFI_SCAN_START reply is a legitimate empty-scan result, "
	              "not the dead-phase alias (#2035)");
}

ZTEST(cc3501e_host_driver, test_ble_scan_start_empty_legacy_accepted_2035)
{
	fw.fw_proto_major               = (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	g_force_dead_phase_zero_payload = true;

	uint8_t      reply[8] = { 0 };
	size_t       got      = 0u;
	alp_status_t s        = cc3501e_request(
	    &fw, ALP_CC3501E_CMD_BLE_SCAN_START, NULL, 0u, reply, sizeof(reply), &got, 100u);
	zassert_equal(s,
	              ALP_OK,
	              "an all-zero padded BLE_SCAN_START reply is a legitimate empty-scan result, "
	              "not the dead-phase alias (#2035)");
}

/* ---- ADR 0033: CMD_GET_CAPABILITIES (0x06) --------------------------------- */

ZTEST(cc3501e_host_driver, test_get_capabilities_decodes_le32_bitmap_0033)
{
	g_caps_override_value =
	    (uint32_t)(ALP_CC3501E_CAP_WIFI_STA | ALP_CC3501E_CAP_BLE | ALP_CC3501E_CAP_SPI1_MASTER);

	uint32_t caps = 0xDEADBEEFu;
	zassert_equal(cc3501e_get_capabilities(&fw, &caps), ALP_OK, "GET_CAPABILITIES -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_CAPABILITIES, "opcode 0x06");
	zassert_equal(
	    caps,
	    (uint32_t)(ALP_CC3501E_CAP_WIFI_STA | ALP_CC3501E_CAP_BLE | ALP_CC3501E_CAP_SPI1_MASTER),
	    "decoded LE32 capability bitmap");
}

ZTEST(cc3501e_host_driver, test_get_capabilities_null_out_invalid_0033)
{
	zassert_equal(cc3501e_get_capabilities(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_get_capabilities_short_reply_is_io_0033)
{
	g_caps_reply_short = true;

	uint32_t caps = 0x11111111u;
	zassert_equal(cc3501e_get_capabilities(&fw, &caps),
	              ALP_ERR_IO,
	              "fewer than 4 data bytes must not decode a half-formed bitmap");
	zassert_equal(caps, 0u, "the out-pointer is zeroed up front, not left half-decoded");
}

/* ============================ DIAGNOSTICS ================================== */

ZTEST(cc3501e_host_driver, test_diag_info_decodes_all_fields)
{
	alp_cc3501e_diag_info_t d;
	memset(&d, 0xA5, sizeof(d));
	zassert_equal(cc3501e_diag_info(&fw, &d), ALP_OK, "GET_DIAG_INFO -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_DIAG_INFO, "opcode 0x04");
	zassert_equal(d.fw_version, 0x0102u, "fw_version LE16");
	zassert_equal(d.reset_cause, ALP_CC3501E_RESET_POWER_ON, "reset_cause");
	zassert_equal(d.role, ALP_CC3501E_ROLE_WIFI_STA, "role");
	zassert_equal(d.uptime_ms, 0x00ABCDEFu, "uptime_ms LE32");
	zassert_equal(d.free_heap_bytes, 0x00012340u, "free_heap_bytes LE32");
	zassert_equal(d.last_error, ALP_CC3501E_RESP_OK, "last_error");
	/* #2035: the 18-byte reply's two new bytes. */
	zassert_equal(d.dhcp_state, ALP_CC3501E_DHCP_STATE_BOUND, "dhcp_state byte 16");
	zassert_equal(d.netif_status & ALP_CC3501E_NETIF_UP, ALP_CC3501E_NETIF_UP, "netif UP bit");
	zassert_equal(
	    d.netif_status & ALP_CC3501E_NETIF_LINK_UP, ALP_CC3501E_NETIF_LINK_UP, "netif LINK_UP bit");
	zassert_equal(ALP_CC3501E_NETIF_DHCP_TRIES(d.netif_status), 5u, "dhcp->tries bits 2..7");
}

/* #2035: an OLDER bridge firmware that only ever answers the pre-#2035
 * 16-byte shape must NOT start failing -- growing reply[] to 18 bytes on the
 * host side is additive, not a floor. The two new fields read as
 * "not reported" (0), not as garbage or a decode error. */
ZTEST(cc3501e_host_driver, test_diag_info_16byte_reply_reports_new_fields_not_reported)
{
	g_diag_info_reply_len = 16u;
	alp_cc3501e_diag_info_t d;
	memset(&d, 0xA5, sizeof(d));
	zassert_equal(cc3501e_diag_info(&fw, &d), ALP_OK, "16-byte reply is still a SUCCESS");
	zassert_equal(d.fw_version, 0x0102u, "the original 16 bytes still decode");
	zassert_equal(d.last_error, ALP_CC3501E_RESP_OK, "the original 16 bytes still decode");
	zassert_equal(d.dhcp_state,
	              ALP_CC3501E_DHCP_STATE_NOT_REPORTED,
	              "no lwIP dhcp_state byte -> not-reported, never DHCP_STATE_OFF");
	zassert_equal(d.netif_status, 0u, "no netif_status byte -> not-reported");
}

/* #2035 review follow-up: test_diag_info_16byte_reply_reports_new_fields_not_reported
 * above stages its 16-byte reply through the PADDED wire model, so it rounds
 * up to the identical 24-byte frame the full 18-byte reply also produces --
 * `got` is 18 in both, meaning that test cannot distinguish the intended
 * got<16u guard from a got<18u regression (a reviewer mutated the guard to
 * got<18u and the suite stayed green). Stage the reply through the UNPADDED
 * legacy model instead: a real 16-data-byte reply with no pad and no CRC
 * gives got=16 exactly, which DOES fall on the wrong side of a got<18u
 * guard -- this is the property that actually needs pinning.
 *
 * Mutation check (verified by hand, not just asserted here): change the
 * `if (got < 16u) return ALP_ERR_IO;` guard in cc3501e_diag_info()
 * (chips/cc3501e/cc3501e_diag.c) to `if (got < 18u) return ALP_ERR_IO;` --
 * this test goes RED (ALP_ERR_IO instead of ALP_OK), while
 * test_diag_info_16byte_reply_reports_new_fields_not_reported above and
 * test_diag_info_decodes_all_fields stay green either way, exactly because
 * their padded-model got is always 18. Revert the guard after checking. */
ZTEST(cc3501e_host_driver,
      test_diag_info_legacy_unpadded_16byte_reply_reports_new_fields_not_reported)
{
	fw.fw_proto_major           = (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY;
	g_diag_info_legacy_unpadded = true;

	alp_cc3501e_diag_info_t d;
	memset(&d, 0xA5, sizeof(d));
	zassert_equal(cc3501e_diag_info(&fw, &d),
	              ALP_OK,
	              "a genuinely unpadded 16-byte wire reply (got=16, not 18) is still a SUCCESS");
	zassert_equal(d.fw_version, 0x0102u, "the original 16 bytes still decode");
	zassert_equal(d.dhcp_state,
	              ALP_CC3501E_DHCP_STATE_NOT_REPORTED,
	              "no dhcp_state byte on the wire -> not-reported, never DHCP_STATE_OFF");
	zassert_equal(d.netif_status, 0u, "no netif_status byte on the wire -> not-reported");
}

/* A reply shorter than the original 16-byte shape is still a genuine fault.
 * 8, not some other short length: cc3501e_request()'s `got` this host call
 * ever sees is `min(payload_len - 1, rx_cap=18)`, and payload_len is always
 * padded to an ALP_CC3501E_REPLY_PAD (8 B) multiple, so measured behaviour is
 * got IN {7, 15, 18} ONLY -- never anything else. A data_len (this fixture's
 * g_diag_info_reply_len) of 1..8 pads to payload_len 16 -> got=15 (rejected,
 * below the got<16u guard); 9..15 ALSO pads to payload_len 24 -> got=18
 * (accepted, same as the full 16/18-byte replies -- NOT "rounded up to 16 and
 * passed spuriously" as an earlier version of this comment claimed, which
 * described a length that cannot occur on this wire). The effective floor
 * this guard enforces is payload_len >= 24 (got >= 16), not "16 data bytes";
 * a genuinely short reply in the data_len 9..15 range is NOT caught by it --
 * that hole predates this change and is not a regression this fix owns, but
 * this comment must not claim it is rejected. 8 is chosen here only because
 * it is the largest data_len that still arrives as got=15. */
ZTEST(cc3501e_host_driver, test_diag_info_short_reply_fails)
{
	g_diag_info_reply_len = 8u;
	alp_cc3501e_diag_info_t d;
	memset(&d, 0xA5, sizeof(d));
	zassert_equal(cc3501e_diag_info(&fw, &d), ALP_ERR_IO, "<16 bytes -> ALP_ERR_IO");
}

ZTEST(cc3501e_host_driver, test_diag_info_null_out_invalid)
{
	zassert_equal(cc3501e_diag_info(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_diag_stats_decodes_two_le32)
{
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "DIAG_GET_STATS -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_DIAG_GET_STATS, "opcode 0x70");
	zassert_equal(st.frames_ok, 0x11223344u, "frames_ok LE32");
	zassert_equal(st.frames_err, 0x00000005u, "frames_err LE32");
}

/* A v7 firmware answers only the first two counters.  The two v8 counters must
 * then report as ABSENT, not as a measured zero -- a bench run that reads
 * "retry_latch_hits = 0" off firmware that never counted them would record a
 * pass for a mechanism that was not running. */
ZTEST(cc3501e_host_driver, test_diag_stats_short_v7_reply_reports_counters_absent)
{
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "8-byte reply is not a fault");
	zassert_false(st.has_worker_counters, "v7 firmware does not report the worker counters");
	zassert_equal(st.worker_execs, 0u, "absent counters read zero, flagged by has_*");
	zassert_equal(st.retry_latch_hits, 0u, "absent counters read zero, flagged by has_*");
	zassert_equal(st.frames_ok, 0x11223344u, "the two v7 counters still decode");
}

ZTEST(cc3501e_host_driver, test_diag_stats_v8_reply_decodes_worker_counters)
{
	g_diag_stats_v8 = true;
	cc3501e_diag_stats_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "16-byte reply -> OK");
	zassert_true(st.has_worker_counters, "v8 firmware reports the worker counters");
	zassert_equal(st.frames_ok, 0x11223344u, "frames_ok LE32");
	zassert_equal(st.frames_err, 0x00000005u, "frames_err LE32");
	zassert_equal(st.worker_execs, 0x00000007u, "worker_execs LE32 at offset 8");
	zassert_equal(st.retry_latch_hits, 0x00000003u, "retry_latch_hits LE32 at offset 12");
}

/* ---- proto v8 request identity (cc3501e-bridge-firmware#102) --------------
 *
 * The firmware can only absorb a retry if the retry is RECOGNISABLE, which
 * means the seq in flags bits 3..7 is identical across every attempt of one
 * logical command.  These assert the wire property directly off the captured
 * header bytes, because that is the contract the firmware reads -- not the
 * host-side counter that produced it. */

static uint8_t seq_of(uint8_t flags)
{
	return (uint8_t)((flags >> ALP_CC3501E_FLAG_REQ_SEQ_SHIFT) & ALP_CC3501E_REQ_SEQ_MASK);
}

ZTEST(cc3501e_host_driver, test_retry_seq_is_constant_across_one_commands_retries)
{
	slave.rssi_busy_polls_remaining = 3u; /* 3 BUSY acks, then the value */
	int8_t rssi                     = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK after riding out BUSY");
	zassert_true(slave.flags_log_count >= 4u, "3 BUSY attempts + the collect were clocked");

	const uint8_t seq = seq_of(slave.flags_log[0]);
	zassert_not_equal(seq,
	                  ALP_CC3501E_REQ_SEQ_NONE,
	                  "a retryable command must carry a real seq, not the reserved 0");
	for (uint32_t i = 1u; i < slave.flags_log_count && i < ARRAY_SIZE(slave.flags_log); i++) {
		zassert_equal(seq_of(slave.flags_log[i]),
		              seq,
		              "every retry of ONE logical command re-sends the SAME seq");
	}
}

ZTEST(cc3501e_host_driver, test_each_logical_command_gets_a_different_seq)
{
	int8_t rssi = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "first command");
	const uint8_t  first       = seq_of(slave.flags_log[0]);
	const uint32_t after_first = slave.flags_log_count;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "second command");
	zassert_true(slave.flags_log_count > after_first, "the second command clocked a header");
	zassert_not_equal(seq_of(slave.flags_log[after_first]),
	                  first,
	                  "a NEW logical command must not reuse the previous command's seq, or the "
	                  "firmware would serve it the previous command's cached reply");
}

/* The single-shot path has no retry loop, so nothing it sends is ever a repeat
 * of anything -- it must therefore be un-latchable.  Seq 0 says exactly that. */
ZTEST(cc3501e_host_driver, test_single_shot_request_sends_the_reserved_seq_none)
{
	cc3501e_diag_stats_t st;
	zassert_equal(cc3501e_diag_stats(&fw, &st), ALP_OK, "DIAG_GET_STATS -> OK");
	zassert_true(slave.flags_log_count >= 1u, "a header was clocked");
	zassert_equal(seq_of(slave.flags_log[0]),
	              ALP_CC3501E_REQ_SEQ_NONE,
	              "cc3501e_request() is single-shot -- it must not claim an identity");
	zassert_true((slave.flags_log[0] & ALP_CC3501E_FLAG_RESP_REQUIRED) != 0u,
	             "the v1 flag bits are untouched by the seq");
}

ZTEST(cc3501e_host_driver, test_diag_log_level_encodes_level_byte)
{
	zassert_equal(cc3501e_diag_log_level(&fw, 3u), ALP_OK, "DIAG_LOG_LEVEL -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_DIAG_LOG_LEVEL, "opcode 0x71");
	zassert_equal(slave.req_len, 1u, "single level byte");
	zassert_equal(slave.req_pl[0], 3u, "level byte value");
}

/* =============================== WI-FI ===================================== */

ZTEST(cc3501e_host_driver, test_wifi_get_mac_decodes_6_bytes)
{
	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	zassert_equal(cc3501e_wifi_get_mac(&fw, mac, 100u), ALP_OK, "GET_MAC -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_MAC, "opcode 0x03");
	zassert_mem_equal(mac, FIX_MAC, CC3501E_MAC_LEN, "6-byte MAC decoded");
}

/* #2039: the property aen-evk-demo's phase-8 MAC gate rests on.
 *
 * That gate reads GET_MAC twice and requires the two replies to match, because
 * a single flipped bit yields an address that passes every structural test --
 * group bit clear, non-zero OUI, neither constant pattern -- and one bench run
 * on E1M-AEN803 serial 2026W36-0002 produced exactly that. The gate is only
 * worth anything if the second read is a SECOND WIRE TRANSACTION rather than a
 * cached reply, so that is what is asserted here: two calls serve two
 * CMD_GET_MAC requests, and corrupting only the first makes the two buffers
 * differ.
 *
 * Mutation-verified: make the fake slave corrupt neither read (or make
 * cc3501e_wifi_get_mac cache), and the mem_not_equal below fails. */
ZTEST(cc3501e_host_driver, test_wifi_get_mac_repeat_is_a_second_wire_read)
{
	uint8_t first[CC3501E_MAC_LEN]  = { 0 };
	uint8_t second[CC3501E_MAC_LEN] = { 0 };

	g_get_mac_corrupt_remaining = 1u; /* corrupt the FIRST read only */
	zassert_equal(cc3501e_wifi_get_mac(&fw, first, 100u), ALP_OK, "first GET_MAC -> OK");
	zassert_equal(cc3501e_wifi_get_mac(&fw, second, 100u), ALP_OK, "second GET_MAC -> OK");

	zassert_equal(g_get_mac_serve_count, 2u, "two calls issue two CMD_GET_MAC requests");
	zassert_mem_equal(second, FIX_MAC, CC3501E_MAC_LEN, "uncorrupted read decodes the real MAC");
	/* The corrupted read is still structurally a valid station address --
	 * only the repeat separates it from the real one. */
	zassert_equal(first[0] & 0x01u, 0u, "corrupted byte 0 still has the group bit clear");
	zassert_true(memcmp(first, second, CC3501E_MAC_LEN) != 0,
	             "a corrupted read differs from a clean one, so the repeat catches it");
}

ZTEST(cc3501e_host_driver, test_wifi_rssi_decodes_signed)
{
	int8_t rssi = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_GET_RSSI, "opcode 0x16");
	zassert_equal(rssi, -42, "signed dBm decoded");
}

/* #1377: WIFI_GET_RSSI is worker-routed on the firmware side -- a fresh
 * submit acks RESP_ERR_BUSY and only later returns the value once the drain
 * has collected it.  Against the pre-fix single cc3501e_request() this test
 * fails outright: that call collects only the first BUSY ack and returns
 * ALP_ERR_BUSY, leaving the job orphaned.  Poll-by-repeat must ride the busy
 * window out and still land on the real value. */
ZTEST(cc3501e_host_driver, test_wifi_rssi_retries_worker_busy_1377)
{
	slave.rssi_busy_polls_remaining = 3u;
	int8_t rssi                     = 0;
	zassert_equal(cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "GET_RSSI -> OK after riding out BUSY");
	zassert_equal(rssi, -42, "signed dBm decoded once the drain collected it");
	zassert_equal(slave.rssi_busy_polls_remaining, 0u, "all staged busy polls were consumed");
}

/* The network-order -> dotted-quad fix: the wire octets arrive reversed and the
 * host reverses them.  0xC0A8010E on the wire must decode to {192,168,1,14}. */
ZTEST(cc3501e_host_driver, test_wifi_get_ip_byte_order)
{
	uint8_t ip[4] = { 0 };
	zassert_equal(cc3501e_wifi_get_ip(&fw, ALP_CC3501E_WIFI_IFACE_STA, ip), ALP_OK, "GET_IP -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_GET_IP, "opcode 0x17");
	zassert_equal(ip[0], 192, "ip[0]");
	zassert_equal(ip[1], 168, "ip[1]");
	zassert_equal(ip[2], 1, "ip[2]");
	zassert_equal(ip[3], 14, "ip[3] -- 0xC0A8010E -> 192.168.1.14");
}

/* #2035: the firmware's only status for "no address on this interface yet"
 * is RESP_ERR_RADIO (see hal/ti/cc3501e_hw_ti_wifi.c) -- a genuinely decoded
 * reply, so ctx->rx_scratch[0] holds the real status byte.  The host must
 * disambiguate this from a broken transport and report it distinctly. */
ZTEST(cc3501e_host_driver, test_wifi_get_ip_no_address_is_not_ready_2035)
{
	g_get_ip_no_address = true;
	uint8_t ip[4]       = { 0xAA, 0xAA, 0xAA, 0xAA };
	zassert_equal(cc3501e_wifi_get_ip(&fw, ALP_CC3501E_WIFI_IFACE_STA, ip),
	              ALP_ERR_NOT_READY,
	              "decoded RESP_ERR_RADIO -> ALP_ERR_NOT_READY, not ALP_ERR_IO");
}

/* The case that matters (#2035): a genuine transport-level failure -- the
 * REQUEST HEADER transceive itself fails, so NO status is ever decoded and
 * ctx->rx_scratch[0] is left at ALP_CC3501E_RX_SCRATCH_NO_STATUS -- must stay
 * ALP_ERR_IO.  Proves the fix disambiguates rather than reclassifying every
 * get_ip failure as ALP_ERR_NOT_READY. */
ZTEST(cc3501e_host_driver, test_wifi_get_ip_transport_failure_stays_io_2035)
{
	g_get_ip_io_down_remaining = 1u;
	uint8_t ip[4]              = { 0xAA, 0xAA, 0xAA, 0xAA };
	zassert_equal(cc3501e_wifi_get_ip(&fw, ALP_CC3501E_WIFI_IFACE_STA, ip),
	              ALP_ERR_IO,
	              "no decoded status -> stays ALP_ERR_IO, not ALP_ERR_NOT_READY");
}

ZTEST(cc3501e_host_driver, test_wifi_status_decodes_fields)
{
	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_wifi_status(&fw, &st), ALP_OK, "WIFI_STATUS -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_STATUS, "opcode 0x1B");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONNECTED, "state");
	zassert_equal(st.fail_reason, ALP_CC3501E_WIFI_FAIL_NONE, "fail_reason");
	zassert_equal(st.rssi_dbm, -50, "rssi_dbm (signed)");
	zassert_equal(st.last_reason, 0u, "last_reason (nothing recorded)");
}

/* #2099: the WIFI_STATUS reply's 4th wire byte -- formerly an unused
 * `reserved` byte the host always decoded as 0 -- now carries the IEEE
 * 802.11 reason/status code that ended or rejected the connect attempt.
 * Proves the host decodes reply[3] into last_reason rather than dropping it
 * on the floor. */
ZTEST(cc3501e_host_driver, test_wifi_status_decodes_last_reason_2099)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.wifi_last_reason = 15u; /* IEEE 802.11 reason 15: 4-way handshake timeout */

	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_wifi_status(&fw, &st), ALP_OK, "WIFI_STATUS -> OK");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONN_FAILED, "state");
	zassert_equal(st.fail_reason, ALP_CC3501E_WIFI_FAIL_REJECTED, "fail_reason");
	zassert_equal(st.last_reason, 15u, "last_reason decoded off reply[3]");
}

ZTEST(cc3501e_host_driver, test_wifi_status_null_out_invalid)
{
	zassert_equal(cc3501e_wifi_status(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* #2099: cc3501e_wifi_status_once() -- the internal single-shot helper made
 * public so a caller (the console's `wifi connect` result line) can fetch
 * last_reason without risking cc3501e_wifi_status()'s own down-window retry.
 * Same wire decode, no retry-specific behaviour to prove here beyond that. */
ZTEST(cc3501e_host_driver, test_wifi_status_once_decodes_fields_2099)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.wifi_last_reason = 15u;

	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(cc3501e_wifi_status_once(&fw, &st), ALP_OK, "WIFI_STATUS -> OK");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONN_FAILED, "state");
	zassert_equal(st.last_reason, 15u, "last_reason decoded off reply[3]");
}

ZTEST(cc3501e_host_driver, test_wifi_status_once_null_out_invalid_2099)
{
	zassert_equal(cc3501e_wifi_status_once(&fw, NULL), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* #1377: `alp companion wifi status` returned -5 (ALP_ERR_IO) repeatedly right
 * after a healthy ver/scan/connect sequence -- the shared bridge transport is
 * briefly down whenever a radio op is in flight, and a status read landing in
 * that window desynced like any other transaction.  Against the pre-fix
 * single cc3501e_request() this test fails outright: the first (injected)
 * transport fault returns ALP_ERR_IO immediately, with no retry.  Riding the
 * down-window out via poll-by-repeat must still land on the real state. */
ZTEST(cc3501e_host_driver, test_wifi_status_retries_transient_io_1377)
{
	g_status_io_down_remaining = 5u;
	alp_cc3501e_wifi_status_t st;
	memset(&st, 0xA5, sizeof(st));
	zassert_equal(
	    cc3501e_wifi_status(&fw, &st), ALP_OK, "WIFI_STATUS -> OK after riding out the IO window");
	zassert_equal(st.state, ALP_CC3501E_WIFI_CONNECTED, "state decoded once the link recovered");
	zassert_equal(g_status_io_down_remaining, 0u, "all staged transport faults were consumed");
}

/* Same fault, but never recovers within a short caller budget: this proves
 * the retry is BOUNDED (not an infinite spin) and the honest answer -- an
 * unrecoverable transport -- surfaces as ALP_ERR_TIMEOUT, not the raw
 * ALP_ERR_IO of a single attempt, once the down-window elapses. */
ZTEST(cc3501e_host_driver, test_wifi_status_gives_up_after_the_down_window_1377)
{
	g_status_io_down_remaining = UINT32_MAX;
	alp_cc3501e_wifi_status_t st;
	alp_status_t              s = cc3501e_wifi_status(&fw, &st);
	zassert_equal(s, ALP_ERR_TIMEOUT, "permanently-down transport -> bounded TIMEOUT, not a hang");
}

/* SCAN_START reply is a packed sequence of records; the host walks them out into
 * the caller's array, copying + NUL-terminating each length-prefixed SSID. */
ZTEST(cc3501e_host_driver, test_wifi_scan_walks_records)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 0u;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "SCAN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_SCAN_START, "opcode 0x10");
	zassert_equal(n, 2u, "two records parsed");

	zassert_equal(recs[0].channel, 6u, "rec0 channel");
	zassert_equal(recs[0].rssi_dbm, -40, "rec0 rssi (signed)");
	zassert_equal(recs[0].ssid_len, 4u, "rec0 ssid_len");
	zassert_str_equal(recs[0].ssid, "Test", "rec0 SSID copied + NUL-terminated");
	zassert_equal(recs[0].security_info, 0x0400u, "rec0 security_info LE16");
	zassert_equal(cc3501e_wifi_sec_kind(recs[0].security_info), CC3501E_WIFI_SEC_WPA2, "rec0 WPA2");

	zassert_equal(recs[1].channel, 11u, "rec1 channel");
	zassert_str_equal(recs[1].ssid, "OpenNet", "rec1 SSID");
	zassert_equal(cc3501e_wifi_sec_kind(recs[1].security_info), CC3501E_WIFI_SEC_OPEN, "rec1 open");
}

/* #740: cc3501e_wifi_scan's decode buffer moved from a function-local `static`
 * (shared by every cc3501e_t, process-wide) into per-context storage
 * (ctx->wifi_scan_buf). A second, independent context must not be able to
 * disturb a first context's already-decoded scratch.
 *
 * Regression-proof (not vacuous -- see the fix-round note): the FIRST
 * zassert_mem_equal below compares ctx A's ctx->wifi_scan_buf against the
 * exact raw wire bytes the mock slave staged, independently reconstructed
 * via build_wifi_scan() -- it is not a self-referential snapshot-vs-itself
 * check. That assertion FAILS against the pre-#740 cc3501e_wifi_scan (a
 * function-local `static scan_buf`, reverted from origin/dev): that form
 * never writes ctx->wifi_scan_buf at all, so the field stays all-zero and
 * the byte-compare against the real staged reply mismatches immediately.
 * ctx B then runs its OWN real cc3501e_wifi_scan() call (through the driver,
 * not a direct memset) with a DIFFERENT staged reply (build_wifi_scan_ctx_b,
 * a distinct BSSID/SSID/channel), and ctx A's buffer must still read back
 * unchanged afterward -- the non-aliasing half of #740, made meaningful by
 * using different content for A and B instead of two identical payloads. */
ZTEST(cc3501e_host_driver, test_wifi_scan_buf_is_per_context_740)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 0u;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "SCAN ctx A -> OK");
	zassert_equal(n, 2u, "two records parsed on ctx A");

	uint8_t  wire_a[ALP_CC3501E_MAX_PAYLOAD];
	uint16_t wire_a_len = build_wifi_scan(wire_a);
	zassert_mem_equal(fw.wifi_scan_buf,
	                  wire_a,
	                  wire_a_len,
	                  "ctx A's cc3501e_wifi_scan must decode into ctx->wifi_scan_buf itself "
	                  "(#740) -- fails against the pre-fix function-local `static` buffer, "
	                  "which never touches this field");

	uint8_t snapshot[ALP_CC3501E_MAX_PAYLOAD];
	memcpy(snapshot, fw.wifi_scan_buf, sizeof(snapshot));

	/* Independent second context runs its OWN real scan, through the same
	 * driver entry point, staged with genuinely different content. */
	cc3501e_t ctx_b;
	zassert_equal(cc3501e_init(&ctx_b, fake_bus), ALP_OK, "init ctx B");
	slave_reset();
	g_scan_stage_ctx_b = true;
	zassert_equal(cc3501e_wifi_scan(&ctx_b, recs, 8u, &n, 100u), ALP_OK, "SCAN ctx B -> OK");
	zassert_equal(n, 1u, "ctx B's distinct single-record reply parsed");
	zassert_str_equal(recs[0].ssid, "Ctx2Net", "ctx B decoded ITS OWN staged SSID");

	zassert_mem_equal(fw.wifi_scan_buf,
	                  snapshot,
	                  sizeof(snapshot),
	                  "ctx A's scan buffer must be unaffected by ctx B's OWN real scan (#740)");
}

/* #740: same-context reentrancy is now an explicit ALP_ERR_BUSY instead of
 * silently racing the shared decode buffer. */
ZTEST(cc3501e_host_driver, test_wifi_scan_busy_rejects_reentrant_same_ctx_740)
{
	cc3501e_scan_record_t recs[8];
	size_t                n = 123u;

	fw.wifi_scan_busy = true; /* simulate an in-flight scan on this ctx */
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_ERR_BUSY, "reentrant -> BUSY");
	zassert_equal(slave.cmd, 0u, "no transfer clocked while busy");
	zassert_equal(n, 0u, "count still reset to 0 before the busy check");

	fw.wifi_scan_busy = false;
	zassert_equal(cc3501e_wifi_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "cleared -> scan proceeds");
	zassert_equal(n, 2u, "normal scan after the busy flag clears");
}

ZTEST(cc3501e_host_driver, test_wifi_scan_null_ctx_not_ready)
{
	cc3501e_scan_record_t recs[8];
	zassert_equal(
	    cc3501e_wifi_scan(NULL, recs, 8u, NULL, 100u), ALP_ERR_NOT_READY, "NULL ctx -> NOT_READY");
}

/* The scan-security decoder is a pure host function over the raw TI SecurityInfo. */
ZTEST(cc3501e_host_driver, test_wifi_sec_kind_and_name)
{
	zassert_equal(cc3501e_wifi_sec_kind(0x0000u), CC3501E_WIFI_SEC_OPEN, "open");
	zassert_equal(cc3501e_wifi_sec_kind(0x0400u), CC3501E_WIFI_SEC_WPA2, "wpa2 (bit 0x04)");
	zassert_equal(cc3501e_wifi_sec_kind(0x0800u), CC3501E_WIFI_SEC_WPA3, "wpa3 (SAE bit 0x08)");
	zassert_equal(cc3501e_wifi_sec_kind(0x1000u), CC3501E_WIFI_SEC_WPA3, "wpa3 (SAE bit 0x10)");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0000u), "open", "name open");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0400u), "wpa2", "name wpa2");
	zassert_str_equal(cc3501e_wifi_sec_name(0x0800u), "wpa3", "name wpa3");
}

/* CONNECT packs the connect header (ssid_len | psk_len | security | rsvd) then
 * the inline SSID then the inline passphrase, with no padding.  The wire
 * bytes are asserted off the DEDICATED connect_last_req_* snapshot, not the
 * generic slave.req_pl/req_len -- by the time cc3501e_wifi_connect() returns,
 * those reflect its own follow-up WIFI_STATUS poll (empty request), not the
 * CONNECT_STA submit (see the mock's WIFI_CONNECT_STA case). */
ZTEST(cc3501e_host_driver, test_wifi_connect_encodes_header_ssid_psk)
{
	zassert_equal(
	    cc3501e_wifi_connect(&fw, "mynet", 1u, "secretpw", 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.connect_submit_count, 1u, "exactly one submit");
	/* header(4) + ssid(5) + psk(8) = 17. */
	zassert_equal(slave.connect_last_req_len, 4u + 5u + 8u, "submit payload = header + ssid + psk");
	zassert_equal(slave.connect_last_req_pl[0], 5u, "ssid_len");
	zassert_equal(slave.connect_last_req_pl[1], 8u, "psk_len");
	zassert_equal(slave.connect_last_req_pl[2], 1u, "security");
	zassert_mem_equal(&slave.connect_last_req_pl[4], "mynet", 5u, "inline SSID");
	zassert_mem_equal(&slave.connect_last_req_pl[9], "secretpw", 8u, "inline passphrase");
}

ZTEST(cc3501e_host_driver, test_wifi_connect_oversize_ssid_rejected)
{
	static const char big[40] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 35 > 32 */
	zassert_equal(cc3501e_wifi_connect(&fw, big, 1u, "pw", 100u),
	              ALP_ERR_INVAL,
	              "SSID > 32 -> INVAL (host guard)");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* #1376 regression proof: an association that takes several WIFI_STATUS polls
 * to resolve must still have submitted CONNECT_STA exactly ONCE.  Against the
 * pre-fix poll_by_repeat(WIFI_CONNECT_STA, ...) contract this fails outright
 * -- every 50 ms repeat that landed on the mock's stateless "always ack OK"
 * WIFI_CONNECT_STA case (the model faithful to the OLD contract) would have
 * been read as immediate success; against the OLD real firmware contract
 * (fire-and-forget + a job slot reset to IDLE the instant it drains) each
 * retry submits a brand-new association -- five retries, five real joins,
 * for one user command, per issue #1376. */
ZTEST(cc3501e_host_driver, test_wifi_connect_submits_exactly_once_1376)
{
	slave.status_polls_before_terminal = 4u; /* CONNECTING for 4 polls, then terminal */
	zassert_equal(cc3501e_wifi_connect(&fw, "slownet", 1u, "pw", 5000u),
	              ALP_OK,
	              "eventually CONNECTED -> OK");
	zassert_equal(slave.connect_submit_count,
	              1u,
	              "exactly one CONNECT_STA submit regardless of how many status polls it took");
	/* Reviewer finding: the success path was asserted only via ALP_OK + the
	 * submit count, never off slave.cmd -- a mutant that tore the
	 * association down on the SUCCESS path too (e.g. an unconditional
	 * post-submit disconnect) would still pass both of the above. The last
	 * thing a healthy connect touches the wire with is a WIFI_STATUS poll
	 * reading CONNECTED, never WIFI_DISCONNECT; fence that here. */
	zassert_equal(slave.cmd,
	              ALP_CC3501E_CMD_WIFI_STATUS,
	              "success must end on the WIFI_STATUS read, not a stray WIFI_DISCONNECT");
}

/* #1376: a connection to an SSID that never actually associates must not be
 * reported as connected.  The mock never advances past DISCONNECTED here
 * (the fixture default after slave_reset() is CONNECTED, so override it) --
 * cc3501e_wifi_connect() must time out, not report ALP_OK, and must still
 * have submitted only once. */
ZTEST(cc3501e_host_driver, test_wifi_connect_never_confirmed_times_out_1376)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_DISCONNECTED;
	zassert_equal(cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u),
	              ALP_ERR_TIMEOUT,
	              "never confirmed -> TIMEOUT, not a false OK");
	zassert_equal(slave.connect_submit_count, 1u, "still exactly one submit, not a retry storm");
}

/* #1382 timeout-accounting regression: cc3501e_wifi_connect()'s poll loop
 * must OWN the retry budget, not delegate it to an inner call that retries
 * on its own.  Before the #1382 fix, the loop called the public
 * cc3501e_wifi_status(), whose own poll_by_repeat rides out an IO fault for
 * up to CC3501E_WIFI_DOWN_WINDOW_MS (10 s) per call, while the outer loop's
 * `remaining -= gap` only ever debited its own 50 ms sleep -- so a
 * permanently wedged transport meant every outer iteration hid up to 10 s
 * the caller's declared timeout_ms never accounted for. Measured against
 * this exact harness with a wedged transport: connect(timeout_ms=200) made
 * 1005 WIFI_STATUS attempts (50250 ms simulated), 251x the declared budget.
 *
 * Wedge the transport permanently (g_status_io_down_remaining = UINT32_MAX,
 * as test_wifi_status_gives_up_after_the_down_window_1377 does for a direct
 * cc3501e_wifi_status() call) and assert the number of WIFI_STATUS attempts
 * cc3501e_wifi_connect() makes stays in the ballpark ITS OWN cadence
 * predicts.
 *
 * #1481's later fix replaced the poll loop's decrementing `remaining` ledger
 * (which also, on the ss != ALP_OK path exercised here, phantom-debited
 * CC3501E_REQ_TMO_MS per failed read on top of the real poll gap -- see
 * cc3501e_wifi_connect()'s #1481 note) with an `elapsed_ms` accumulator that
 * only ever grows by the real CC3501E_WIFI_STATUS_POLL_GAP_MS (50 ms) it
 * slept.  On an always-failing transport every iteration takes that branch,
 * so the loop now runs floor(timeout_ms / 50) + 1 reads before elapsed_ms
 * reaches timeout_ms, plus the one WIFI_STATUS read cc3501e_wifi_connect()'s
 * entry stale-association check always makes regardless of outcome: for
 * timeout_ms=200 that is 1 + (200 / 50 + 1) = 6 attempts.
 *
 * Asserted EXACTLY, not as an upper bound, and that is deliberate.  A `<= 7`
 * bound passes at both 6 and 7, so it would hide the loop gaining or losing an
 * iteration -- which is precisely the kind of drift this test exists to catch.
 *
 * 6 is cross-validated: a second, independent conversion of this loop (issue
 * #1985, a different shape -- an alp_uptime_ms() deadline rather than this
 * elapsed_ms accumulator) measured the same 6 under the same parameters, and
 * its first pass predicted 5 before the real run corrected it.  Both shapes
 * land on the same off-by-one for the same reason: cc3501e_wifi_connect()
 * makes one unconditional wifi_status_once() read at entry -- the #1435
 * stale-association clear -- before the loop or its budget exist at all.
 *
 * If this ever fails at 5 or 7, do not relax it.  The entry read or the loop
 * shape changed, and the derivation above is what needs revisiting. */
ZTEST(cc3501e_host_driver, test_wifi_connect_bounds_status_attempts_on_wedged_transport_1382)
{
	g_status_io_down_remaining = UINT32_MAX;
	alp_status_t s             = cc3501e_wifi_connect(&fw, "wedgednet", 1u, "pw", 200u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "permanently wedged transport -> bounded TIMEOUT");
	zassert_equal(slave.wifi_status_attempt_count,
	              6u,
	              "WIFI_STATUS attempts must be EXACTLY 1 entry-check read + floor(200/50)+1 "
	              "= 6 loop reads, bounded by connect()'s own 200 ms budget and not by an inner "
	              "down-window retry loop it doesn't account for (got %u attempts)",
	              slave.wifi_status_attempt_count);
}

/* #1481 regression: a HEALTHY poll (every WIFI_STATUS read returns ALP_OK,
 * simply reporting CONNECTING) must debit only the CC3501E_WIFI_STATUS_POLL_GAP_MS
 * (50 ms) it actually slept, not the CC3501E_REQ_TMO_MS (100 ms) worst-case
 * attempt cost the ss != ALP_OK path reserves for a failed read that never
 * happened here.  Before the fix, that 100 ms was debited on EVERY iteration
 * regardless of ss, so a healthy 3-iterations-of-CONNECTING poll burned
 * 3 * 150 ms = 450 ms of a caller's declared budget for only 3 * 50 ms =
 * 150 ms of real elapsed time (alp_delay_ms is a no-op stub here, but the
 * `remaining` accounting is exactly what a real caller's wall clock would
 * see) -- collapsing timeout_ms to roughly 1/3 of what was asked for.
 *
 * slave.status_polls_before_terminal = 4u yields four CONNECTING reads
 * total: one consumed by cc3501e_wifi_connect()'s own entry stale-
 * association check (before the loop's `remaining` budget is even
 * initialised, same fixture-order accounting
 * test_wifi_connect_submits_exactly_once_1376 relies on), then three more
 * inside the poll loop, before the fifth WIFI_STATUS read reports the
 * fixture default CONNECTED and the call returns ALP_OK.  A timeout_ms of
 * 320 ms comfortably covers the honest 3 * 50 ms = 150 ms the fixed loop
 * actually spends, but is well under the 3 * 150 ms = 450 ms the pre-fix
 * unconditional debit would have needed -- so this proves ALP_OK on the
 * fix and would have proven a premature ALP_ERR_TIMEOUT on the bug. */
ZTEST(cc3501e_host_driver, test_wifi_connect_healthy_poll_not_over_debited_1481)
{
	slave.status_polls_before_terminal = 4u; /* CONNECTING x3 in-loop, then CONNECTED */
	alp_status_t s                     = cc3501e_wifi_connect(&fw, "healthynet", 1u, "pw", 320u);
	zassert_equal(s,
	              ALP_OK,
	              "a healthy CONNECTING poll must consume ~wall-clock time (150 ms), not "
	              "~3x it (450 ms) against a 320 ms budget (got status %d)",
	              s);
}

/* #1376/#1378: an association that genuinely FAILS (auth reject / no AP) must
 * be reported as a failure, not an OK -- and, as above, from exactly one
 * submit. */
ZTEST(cc3501e_host_driver, test_wifi_connect_reports_failure_not_ok_1376)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	zassert_equal(cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u),
	              ALP_ERR_IO,
	              "CONN_FAILED/REJECTED -> IO, not OK");
	zassert_equal(slave.connect_submit_count, 1u, "exactly one submit");
}

/* #1435 helper: first index in slave.cmd_log at which @p cmd appears, or
 * slave.cmd_log_count (never a valid index) if it never did. slave.cmd alone
 * only ever holds the LAST opcode dispatched -- not enough to prove ORDER
 * (WIFI_DISCONNECT strictly before WIFI_CONNECT_STA), which is the actual
 * property under test below. */
static uint32_t cmd_log_index_of(uint8_t cmd)
{
	uint32_t n =
	    (slave.cmd_log_count < sizeof(slave.cmd_log)) ? slave.cmd_log_count : sizeof(slave.cmd_log);
	for (uint32_t i = 0; i < n; i++) {
		if (slave.cmd_log[i] == cmd) {
			return i;
		}
	}
	return slave.cmd_log_count;
}

/* #1435 bench-proven stale-association wedge: a connect that FOLLOWS a
 * failed attempt (the WIFI_STATUS latch already reads CONN_FAILED when this
 * one is entered) must clear it -- issue WIFI_DISCONNECT (0x13) -- BEFORE
 * submitting WIFI_CONNECT_STA (0x12), else the new association's own
 * Wlan_Connect kick fails against the leftover NWP state (ALP_CC3501E_WIFI_
 * FAIL_KICK) even for a correct SSID/passphrase -- reproduced 2/2 on real
 * silicon (E1M-AEN801 r1). Checking ORDER (not just "a disconnect happened
 * somewhere") is the point: the previous round of this fix cleared on the
 * way OUT of a failed connect instead, which a same-opcode-count assertion
 * could not have told apart from clearing on the way IN.
 *
 * The connect must also PROCEED normally afterwards, not short-circuit at
 * the clear: connect_submit_count == 1 proves the submit still happened, and
 * the terminal read afterwards (the mock's latch is untouched by
 * WIFI_DISCONNECT, matching a fresh CONN_FAILED still being current) still
 * reports the CONNECT's own ALP_ERR_IO, not the clear's own ALP_OK. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_clears_stale_failed_association_1435)
{
	slave.wifi_conn_state  = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason = ALP_CC3501E_WIFI_FAIL_REJECTED;
	alp_status_t s         = cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u);
	zassert_equal(
	    s, ALP_ERR_IO, "CONN_FAILED/REJECTED -> the CONNECT's own IO, not the clear's OK");
	zassert_equal(slave.connect_submit_count,
	              1u,
	              "the entry clean must not short-circuit -- CONNECT_STA still submits (#1435)");
	uint32_t disc_idx    = cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT);
	uint32_t connect_idx = cmd_log_index_of(ALP_CC3501E_CMD_WIFI_CONNECT_STA);
	zassert_true(disc_idx < slave.cmd_log_count, "WIFI_DISCONNECT must be issued at all (#1435)");
	zassert_true(connect_idx < slave.cmd_log_count, "WIFI_CONNECT_STA must still be submitted");
	zassert_true(disc_idx < connect_idx,
	             "WIFI_DISCONNECT (idx %u) must land strictly BEFORE WIFI_CONNECT_STA (idx %u)",
	             disc_idx,
	             connect_idx);
}

/* Negative case: a latch that is NOT CONN_FAILED at entry must never see a
 * WIFI_DISCONNECT -- the entry clean is conditional on the failed latch, not
 * unconditional. DISCONNECTED is the "nothing to clear" baseline. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_disconnected_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_DISCONNECTED;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "never confirmed -> TIMEOUT via poll-loop exhaustion");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "DISCONNECTED at entry -> no WIFI_DISCONNECT issued (#1435)");
}

/* CONNECTING at entry is a LIVE attempt, not a stale one -- must be left
 * alone. Today's behaviour (documented on cc3501e_wifi_connect(), not
 * exercised further here) is that the new submit bounces BUSY and the poll
 * loop below keeps tracking the OLD attempt; this test only proves the entry
 * clean itself does not fire on it. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_connecting_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_CONNECTING;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_equal(s, ALP_ERR_TIMEOUT, "still CONNECTING at the deadline -> TIMEOUT");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "CONNECTING at entry -> a live attempt, no WIFI_DISCONNECT issued (#1435)");
}

/* CONNECTED at entry is connect-while-connected -- a pre-existing, separate,
 * unowned semantic this fix does not expand into. Must be left alone. */
ZTEST(cc3501e_host_driver, test_wifi_connect_entry_skips_clean_when_connected_1435)
{
	slave.wifi_conn_state = ALP_CC3501E_WIFI_CONNECTED;
	alp_status_t s        = cc3501e_wifi_connect(&fw, "mynet", 1u, "pw", 100u);
	zassert_equal(s, ALP_OK, "already CONNECTED -> OK (out of scope for #1435 to change)");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "CONNECTED at entry -> no WIFI_DISCONNECT issued (#1435)");
}

/* Regression the rework undoes: a failure discovered DURING the poll loop
 * (not already latched at entry, so the entry clean does not fire -- one
 * busy poll delays the terminal read past the entry check) must NOT issue
 * WIFI_DISCONNECT from either post-submit error exit any more. The previous
 * round of this fix teared down here; this proves that shape is gone. */
ZTEST(cc3501e_host_driver, test_wifi_connect_failure_exit_no_longer_tears_down_1435)
{
	slave.wifi_conn_state              = ALP_CC3501E_WIFI_CONN_FAILED;
	slave.wifi_fail_reason             = ALP_CC3501E_WIFI_FAIL_REJECTED;
	slave.status_polls_before_terminal = 1u; /* entry sees CONNECTING, not the terminal state */
	alp_status_t s = cc3501e_wifi_connect(&fw, "securenet", 1u, "wrongpw", 5000u);
	zassert_equal(s, ALP_ERR_IO, "CONN_FAILED/REJECTED discovered mid-poll -> IO");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_WIFI_DISCONNECT),
	              slave.cmd_log_count,
	              "a failure exit must not itself issue WIFI_DISCONNECT any more (#1435 rework)");
}

/* #1378's own reproduction: force the mock's WIFI_CONNECT_STA submit ack to
 * read back a literal RESP_OK (0x00) -- "a valid header followed by an
 * all-zero payload phase", exactly what a dead bus phase clocks on real
 * silicon per this repo's own finding (hal/ti/cc3501e_hw_ti_wifi.c: "the
 * host then reads 0x00000000 from a dead link").  The association never
 * actually confirms (state stays DISCONNECTED).  cc3501e_wifi_connect() must
 * NOT report ALP_OK: it does not trust the submit's own ack in either
 * direction, only the independent WIFI_STATUS latch -- so this proves the
 * property the issue names: ALP_OK requires positive evidence the device
 * framed a reply, not merely the absence of evidence that it did not. */
ZTEST(cc3501e_host_driver, test_wifi_connect_ignores_dead_phase_ok_alias_1378)
{
	g_connect_submit_force_ok = true;
	slave.wifi_conn_state     = ALP_CC3501E_WIFI_DISCONNECTED;
	alp_status_t s            = cc3501e_wifi_connect(&fw, "ghostnet", 1u, "pw", 120u);
	zassert_not_equal(
	    s, ALP_OK, "a bare-OK submit ack alone must never make connect() report success");
}

/* #1378 at the transport layer directly: cc3501e_request_locked() itself
 * must refuse to hand back ALP_OK for a WIFI_CONNECT_STA submit whose reply
 * is a bare RESP_OK status byte (resp_payload_len == 1) -- the firmware's
 * WORKER_IDLE ack for this opcode is UNCONDITIONALLY RESP_ERR_BUSY, so a
 * synchronous OK is never a value this exchange can legitimately produce;
 * seeing one is self-evidently the dead-phase alias.  This is "the case
 * that matters": a valid reply HEADER (opcode echo + payload_len=1, both
 * staged normally) followed by an all-zero PAYLOAD phase (status byte 0x00)
 * must not yield ALP_OK. */
ZTEST(cc3501e_host_driver, test_connect_sta_dead_phase_alias_rejected_at_transport_1378)
{
	g_connect_submit_force_ok = true;
	uint8_t      req[4]       = { 5u, 0u, 1u, 0u }; /* minimal connect header, no SSID/PSK bytes */
	alp_status_t s            = cc3501e_request(
	    &fw, ALP_CC3501E_CMD_WIFI_CONNECT_STA, req, sizeof(req), NULL, 0, NULL, 100u);
	zassert_not_equal(s,
	                  ALP_OK,
	                  "a dead-phase 0x00 alias for CONNECT_STA's submit ack must not read as "
	                  "ALP_OK (#1378)");
	zassert_equal(s, ALP_ERR_IO, "rejected as a transport error, not silently accepted");
}

ZTEST(cc3501e_host_driver, test_wifi_ap_start_encodes_like_connect)
{
	/* The wire encoding is the assertion here; the RETURN is deliberately not
	 * ALP_OK.  AP_START's firmware handler acks every submit RESP_ERR_BUSY and
	 * the WORKER_DONE branch that would reply RESP_OK is wiped by
	 * worker_run_pending()'s worker_reset() before the host may clock again --
	 * so the opcode cannot synchronously succeed.  A retry loop around it is
	 * therefore provably unwinnable, so cc3501e_wifi_ap_start() submits exactly
	 * ONCE.  Since #1696 it then CONFIRMS that submit against GET_DIAG_INFO's
	 * radio role rather than reporting a blind timeout; g_diag_role is left at
	 * ROLE_WIFI_STA here, so the AP never comes up and the confirmation poll
	 * exhausts its budget -- which is what keeps ALP_ERR_TIMEOUT the expected
	 * outcome for THIS test.  test_wifi_ap_start_confirms_via_diag_role_1696
	 * covers the success direction. */
	zassert_equal(cc3501e_wifi_ap_start(&fw, "AP", 0u, "", 100u),
	              ALP_ERR_TIMEOUT,
	              "role never reaches ROLE_WIFI_AP -- the confirmation poll must exhaust "
	              "timeout_ms rather than inventing a success");
	/* NOT `slave.cmd`: the confirmation polls issue GET_DIAG_INFO after the
	 * submit, so the LAST opcode the mock saw is no longer AP_START.  The
	 * capture below is opcode-specific (the mock only fills
	 * ap_start_last_req_* from the AP_START arm), so it proves 0x14 went out
	 * without depending on it being the most recent frame. */
	zassert_true(slave.ap_start_last_req_len > 0u, "AP_START (0x14) reached the wire");
	zassert_equal(slave.ap_start_last_req_pl[0], 2u, "ssid_len");
	zassert_equal(slave.ap_start_last_req_pl[1], 0u, "psk_len (open)");
	zassert_mem_equal(&slave.ap_start_last_req_pl[4], "AP", 2u, "inline SSID");
	zassert_equal(slave.ap_start_submit_count,
	              1u,
	              "exactly one submit -- not the retry storm a poll-by-repeat wrapper would "
	              "cause, each re-issue of which would submit a BRAND NEW AP RoleUp");
}

/* #1696: the success direction.  Before this, cc3501e_wifi_ap_start() had no
 * reply it could frame as success and returned ALP_ERR_TIMEOUT even for an AP
 * that came up perfectly.  The firmware does publish the outcome -- ap_start
 * latches ROLE_WIFI_AP into the radio role, which GET_DIAG_INFO carries -- so
 * the host confirms against that.
 *
 * Drive the mock's role to AP and the same call must now report ALP_OK, while
 * STILL submitting exactly once (re-submitting would put a fresh Wlan_RoleUp on
 * live radio hardware -- the #1376 storm). */
ZTEST(cc3501e_host_driver, test_wifi_ap_start_confirms_via_diag_role_1696)
{
	g_diag_role = ALP_CC3501E_ROLE_WIFI_AP;

	zassert_equal(cc3501e_wifi_ap_start(&fw, "AP", 0u, "", 1000u),
	              ALP_OK,
	              "GET_DIAG_INFO reporting ROLE_WIFI_AP is what makes the submit confirmable");
	zassert_equal(slave.ap_start_submit_count,
	              1u,
	              "confirmation must poll a non-disturbing opcode, never re-submit AP_START");
}

/* #1385 at the transport layer, the direct analogue of
 * test_connect_sta_dead_phase_alias_rejected_at_transport_1378:
 * cc3501e_request_locked() must refuse to hand back ALP_OK for a
 * WIFI_AP_START submit whose reply is a bare RESP_OK status byte
 * (resp_payload_len == 1).  A valid reply HEADER (opcode echo +
 * payload_len=1) followed by an all-zero PAYLOAD phase is the dead-phase
 * alias this repo measured on silicon ("the host then reads 0x00000000 from a
 * dead link"), and RESP_OK is 0x00.  For this opcode a synchronous OK is not
 * a value the firmware can produce at all: handle_worker_routed_payload acks
 * WORKER_IDLE with RESP_ERR_BUSY, and worker_run_pending() resets the job
 * slot for CONNECT_STA/AP_START BEFORE cc3501e_bridge_ready() lets the host
 * clock again, so the WORKER_DONE -> RESP_OK branch can never be collected. */
ZTEST(cc3501e_host_driver, test_ap_start_dead_phase_alias_rejected_at_transport_1385)
{
	g_connect_submit_force_ok = true;
	uint8_t      req[4]       = { 2u, 0u, 0u, 0u }; /* minimal AP header, no SSID/PSK bytes */
	alp_status_t s =
	    cc3501e_request(&fw, ALP_CC3501E_CMD_WIFI_AP_START, req, sizeof(req), NULL, 0, NULL, 100u);
	zassert_not_equal(s,
	                  ALP_OK,
	                  "a dead-phase 0x00 alias for AP_START's submit ack must not read as "
	                  "ALP_OK (#1385)");
	zassert_equal(s, ALP_ERR_IO, "rejected as a transport error, not silently accepted");
}

/* #1385, the same property one level up: a dead payload phase on every
 * AP_START attempt must never surface from cc3501e_wifi_ap_start() as
 * success.  Before this fix the bare 0x00 was mapped straight through
 * resp_to_status() to ALP_OK and poll_by_repeat() returned it on the first
 * attempt -- a reported AP that never came up.
 *
 * Since #1385's submit-once restructure (77e258dc), cc3501e_wifi_ap_start()
 * squashes every outcome except ALP_ERR_INVAL/ALP_ERR_NOT_READY into
 * ALP_ERR_TIMEOUT unconditionally -- so `!= ALP_OK` on ITS return alone
 * cannot fail no matter what the dead-phase-alias check does; reverting the
 * WIFI_AP_START reject clause in cc3501e_request_locked()
 * (chips/cc3501e/cc3501e_core.c) still left this assertion passing.  Replay
 * the EXACT bytes cc3501e_wifi_ap_start() just staged on the wire (captured
 * by the mock in slave.ap_start_last_req_pl/len) straight through
 * cc3501e_request() -- the layer where the alias check actually runs -- so a
 * regression there fails this test. */
ZTEST(cc3501e_host_driver, test_wifi_ap_start_ignores_dead_phase_ok_alias_1385)
{
	g_connect_submit_force_ok = true;
	zassert_not_equal(cc3501e_wifi_ap_start(&fw, "ghostap", 1u, "pw", 100u),
	                  ALP_OK,
	                  "a bare-OK submit ack alone must never make ap_start() report success");
	alp_status_t raw = cc3501e_request(&fw,
	                                   ALP_CC3501E_CMD_WIFI_AP_START,
	                                   slave.ap_start_last_req_pl,
	                                   slave.ap_start_last_req_len,
	                                   NULL,
	                                   0,
	                                   NULL,
	                                   100u);
	zassert_equal(raw,
	              ALP_ERR_IO,
	              "the dead-phase 0x00 alias for AP_START's own wire payload must be rejected as a "
	              "transport error, not read back as ALP_OK");
}

/* #1385 fence in the OPPOSITE direction: OTA_PROMOTE (0x46) must stay OFF the
 * per-opcode dead-phase reject list.  A bare RESP_OK is that opcode's ONLY
 * success reply, so extending the alias check to it (as #1385's title invites)
 * would make cc3501e_ota_promote() always return ALP_ERR_IO and break firmware
 * promotion outright.  This test fails the moment someone adds
 * ALP_CC3501E_CMD_OTA_PROMOTE to that list.
 *
 * The bare ack is no longer trusted on its own, though -- since #1123 the
 * promote is gated on OTA_STATUS's flash-derived `pending` byte, which the mock
 * reports as STAGED here.  That is the guarantee the ack could never provide,
 * and it is why the alias check does not need to cover this opcode. */
ZTEST(cc3501e_host_driver, test_ota_promote_bare_ok_still_accepted_1385)
{
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_OK,
	              "a STAGED pending image plus the bare RESP_OK is a legitimate promote");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_OTA_PROMOTE, "opcode 0x46 was the last frame");
}

/* #1123: the commit must be refused when the image store has nothing
 * installable.  Before this, promote was an unconditional OK that armed a
 * swap-reboot regardless -- so an aborted or abandoned session's promote
 * "succeeded" and rebooted the device for nothing. */
ZTEST(cc3501e_host_driver, test_ota_promote_refused_when_nothing_pending_1123)
{
	g_ota_pending = ALP_CC3501E_OTA_PENDING_NONE;
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_ERR_NOT_READY,
	              "no installable image -> refuse to commit, do not reboot");
	zassert_not_equal(slave.cmd,
	                  ALP_CC3501E_CMD_OTA_PROMOTE,
	                  "the promote must never reach the wire when nothing is pending");
}

/* UNKNOWN is refused too: 'the store could not be queried' is not consent to
 * reboot.  Reading it as 'nothing pending' would be equally wrong in the other
 * direction -- it must not silently discard an image that may be installable. */
ZTEST(cc3501e_host_driver, test_ota_promote_refused_when_pending_unknown_1123)
{
	g_ota_pending = ALP_CC3501E_OTA_PENDING_UNKNOWN;
	zassert_equal(cc3501e_ota_promote(&fw, 100u),
	              ALP_ERR_NOT_READY,
	              "cannot-determine must refuse, not commit");
}

ZTEST(cc3501e_host_driver, test_wifi_disconnect_and_ap_stop_argless)
{
	zassert_equal(cc3501e_wifi_disconnect(&fw), ALP_OK, "DISCONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_DISCONNECT, "opcode 0x13");
	slave_reset();
	zassert_equal(cc3501e_wifi_ap_stop(&fw), ALP_OK, "AP_STOP -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_AP_STOP, "opcode 0x15");
}

/* =============================== SOCKETS =================================== */

ZTEST(cc3501e_host_driver, test_sock_open_encodes_and_decodes_handle)
{
	uint16_t h = 0u;
	zassert_equal(
	    cc3501e_sock_open(
	        &fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &h, 100u),
	    ALP_OK,
	    "SOCK_OPEN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_OPEN, "opcode 0x20");
	zassert_equal(slave.req_len, 4u, "open payload = {family,type,protocol,rsvd}");
	zassert_equal(slave.req_pl[0], (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "family");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_SOCK_TYPE_STREAM, "type");
	zassert_equal(h, 0x0034u, "decoded LE16 handle");
}

ZTEST(cc3501e_host_driver, test_sock_open_null_handle_invalid)
{
	zassert_equal(
	    cc3501e_sock_open(&fw, 0u, 0u, 0u, NULL, 100u), ALP_ERR_INVAL, "NULL handle_out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* CONNECT packs handle(LE16) | rsvd(2) | sock_addr{ family | rsvd | port(LE16) |
 * addr[16] }; the IPv4 octets land at addr[0..3]. */
ZTEST(cc3501e_host_driver, test_sock_connect_encodes_addr_and_port)
{
	const uint8_t ip[4] = { 93, 184, 216, 34 }; /* 93.184.216.34 */
	zassert_equal(cc3501e_sock_connect(&fw, 0x0034u, ip, 80u, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_CONNECT, "opcode 0x21");
	zassert_equal(slave.req_len, 24u, "connect payload is 24 bytes");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
	zassert_equal(slave.req_pl[4], (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "peer.family");
	zassert_equal(slave.req_pl[6], 80u, "peer.port lo (host order on the wire)");
	zassert_equal(slave.req_pl[7], 0u, "peer.port hi");
	zassert_mem_equal(&slave.req_pl[8], ip, 4u, "peer.addr[0..3] = the IPv4 octets");
}

/* SEND packs the 8-byte send header (handle | flags | rsvd | data_len | rsvd2)
 * then the inline data, and decodes the LE16 accepted count from the reply. */
ZTEST(cc3501e_host_driver, test_sock_send_encodes_header_and_data)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 0u;
	zassert_equal(
	    cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 100u), ALP_OK, "SEND -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_SEND, "opcode 0x22");
	zassert_equal(slave.req_len, 8u + 5u, "send payload = 8-byte header + data");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
	zassert_equal(slave.req_pl[4], 5u, "data_len lo");
	zassert_equal(slave.req_pl[5], 0u, "data_len hi");
	zassert_mem_equal(&slave.req_pl[8], data, 5u, "inline data after the header");
	zassert_equal(sent, 5u, "decoded accepted byte count");
}

/* cc3501e-bridge-firmware#107 (alp-sdk#2035): a slave that queues everything
 * on the first reply -- the pre-#107 case -- must still resolve in exactly
 * ONE SOCK_SEND dispatch. Guards against a remainder-retry rewrite that
 * always loops at least twice regardless of progress. */
ZTEST(cc3501e_host_driver, test_sock_send_full_queue_first_try_one_iteration_107)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 0u;

	zassert_equal(
	    cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 100u), ALP_OK, "SEND -> OK");
	zassert_equal(sent, 5u, "all 5 bytes reported queued");
	zassert_equal(slave.sock_send_log_count, 1u, "one dispatch -- no remainder to retry");
}

/* cc3501e-bridge-firmware#107 (alp-sdk#2035): a slave that queues a PARTIAL
 * count, then the rest on the next poll -- the MSG_DONTWAIT backpressure case
 * the firmware change (#107) introduces. cc3501e_sock_send() must loop,
 * report the FULL count once everything is queued, and -- the property
 * #1746's cache mechanism depends on -- each iteration must carry a DISTINCT
 * seq (payload byte [3]) with only the REMAINING bytes, not the original 5.
 *
 * Asserts the actual payload BYTES of iteration 2, not just its declared
 * data_len: a `remaining += queued` deletion (the pointer never advances, so
 * every iteration re-offers the buffer from byte 0) leaves data_len alone --
 * iteration 2 would still correctly declare "3 bytes remaining" -- but sends
 * the WRONG 3 bytes (the ORIGINAL data[0..3), "GET", not the unsent tail
 * data[2..5), "T /"). A data_len-only assertion cannot see that. */
ZTEST(cc3501e_host_driver, test_sock_send_partial_then_rest_retries_with_new_seq_107)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 0u;

	g_sock_send_queue_plan[0]  = 2u; /* 1st dispatch: queue 2 of 5 */
	g_sock_send_queue_plan[1]  = 3u; /* 2nd dispatch: queue the remaining 3 */
	g_sock_send_queue_plan_len = 2u;

	zassert_equal(
	    cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 1000u), ALP_OK, "SEND -> OK");
	zassert_equal(sent, 5u, "total queued across both iterations == len");
	zassert_equal(slave.sock_send_log_count, 2u, "exactly two iterations");
	zassert_equal(slave.sock_send_datalen_log[0], 5u, "1st iteration offers all 5 remaining");
	zassert_mem_equal(slave.sock_send_payload_log[0], data, 5u, "1st iteration sends bytes 0..5");
	zassert_equal(slave.sock_send_datalen_log[1], 3u, "2nd iteration offers only what's left (3)");
	zassert_mem_equal(slave.sock_send_payload_log[1],
	                  &data[2],
	                  3u,
	                  "2nd iteration sends the UNSENT TAIL data[2..5), not data[0..3)");
	zassert_not_equal(slave.sock_send_seq_log[0],
	                  slave.sock_send_seq_log[1],
	                  "each iteration is a NEW logical send -- distinct seq");
}

/* cc3501e-bridge-firmware#107: a slave that NEVER queues anything (a peer that never reads)
 * must not hang cc3501e_sock_send() -- it must give up within its OWN
 * timeout_ms budget, reporting ALP_ERR_TIMEOUT with whatever partial count
 * was queued (0 here), and it must back off between attempts rather than
 * hammering the link. The fake alp_delay_ms()/alp_uptime_ms() pair (see the
 * fixture above) makes this deterministic without any real sleeping: time
 * only ever advances by what the driver itself asks alp_delay_ms() to sleep. */
ZTEST(cc3501e_host_driver, test_sock_send_never_queued_times_out_and_backs_off_107)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 123u; /* poison -- must come back 0, not left untouched */

	g_sock_send_always_zero = true;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 100u),
	              ALP_ERR_TIMEOUT,
	              "a peer that never reads -> ALP_ERR_TIMEOUT within the budget");
	zassert_equal(sent, 0u, "partial (zero) progress reported, not left poisoned");
	/* Backed off, not hot-looped: at 20 ms/back-off and a 100 ms budget this is
	 * a handful of dispatches, not hundreds. */
	zassert_true(slave.sock_send_log_count >= 2u && slave.sock_send_log_count <= 10u,
	             "bounded, back-off-paced retry count (got %u)",
	             slave.sock_send_log_count);
}

/* alp-sdk#2035 review follow-up: a timeout_ms smaller than
 * CC3501E_SOCK_SEND_BACKOFF_MS(20) must not be overrun by an unconditional
 * full back-off sleep -- a 4 ms budget used to come back after ~23 ms
 * (20 ms back-off + change), not ~4 ms. slave.sock_send_log_count alone
 * (the bound test_sock_send_never_queued_times_out_and_backs_off_107 checks)
 * cannot catch this: an unconditional-sleep mutant still produces a
 * similarly-bounded dispatch count, just a late-returning one -- only the
 * fake clock (g_fake_now_ms) directly exposes the overrun. */
ZTEST(cc3501e_host_driver, test_sock_send_small_timeout_never_overruns_by_a_full_backoff_107)
{
	const uint8_t data[1] = { 'Z' };
	size_t        sent    = 123u;
	uint64_t      base    = g_fake_now_ms;

	g_sock_send_always_zero = true;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 4u),
	              ALP_ERR_TIMEOUT,
	              "a 4 ms budget against a peer that never reads");
	zassert_equal(sent, 0u, "no progress");

	uint64_t elapsed = g_fake_now_ms - base;
	zassert_true(elapsed <= 10u,
	             "must return close to the 4 ms budget, not ~23 ms from a full "
	             "unconditional back-off sleep (got %llu ms)",
	             (unsigned long long)elapsed);
}

/* cc3501e-bridge-firmware#107 (alp-sdk#2035): the first attempt of an
 * iteration can time out while the slave is genuinely BUSY (a job the
 * firmware accepted but has not finished), then the job completes shortly
 * after. cc3501e_sock_send() must not abandon it -- it must collect the
 * queued count into sent_out via its bounded post-timeout grace, returning
 * ALP_OK once that finishes the whole send, using the SAME seq the iteration
 * already assigned (a wrong-seq grace re-poll would look like a BRAND NEW
 * frame to the real worker-slot model below, overwriting the pending job
 * instead of collecting it -- see the mutation-verify note in the PR this
 * test belongs to), with the job's body having executed exactly ONCE (the
 * firmware calls lwip_send() once per accepted job, not once per poll). A
 * FOLLOWING, completely unrelated cc3501e_sock_send() call must not see the
 * stale collected reply as its own (proving no leftover state, either in the
 * driver or in this test's slave model, leaks across calls). timeout_ms(10)
 * is deliberately far shorter than the busy_step_ms(30) required to resolve,
 * so poll_by_repeat()'s own per-iteration call is GUARANTEED to time out
 * before the job clears; CC3501E_SOCK_SEND_COLLECT_GRACE_MS(250) is then more
 * than enough to collect it. */
ZTEST(cc3501e_host_driver, test_sock_send_busy_then_done_collected_via_grace_107)
{
	const uint8_t data1[1] = { 'X' };
	size_t        sent1    = 0u;

	g_sock_send_busy_step_ms[0] = 30u;
	g_sock_send_busy_step_count = 1u;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data1, sizeof(data1), &sent1, 10u),
	              ALP_OK,
	              "collected via the post-timeout grace -> the whole send still completes");
	zassert_equal(sent1, 1u, "the queued count from the collected reply, not 0 / abandoned");
	zassert_equal(slave.sock_send_log_count, 1u, "exactly one iteration was collected");
	zassert_equal(slave.sock_send_seq_log[0],
	              fw.sock_send_seq,
	              "the grace round collected THIS iteration's own seq, not a bumped/different one");
	zassert_equal(slave.sock_send_body_exec_count,
	              1u,
	              "the job body (the queued-count decision) ran exactly once, not once per poll");

	/* A fresh, unrelated call for DIFFERENT data must get its OWN correct
	 * result -- not whatever the grace round above collected. */
	g_sock_send_busy_step_count = 0u; /* back to instant-OK default */
	const uint8_t data2[4]      = { 'Y', 'Y', 'Y', 'Y' };
	size_t        sent2         = 0u;
	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data2, sizeof(data2), &sent2, 100u),
	              ALP_OK,
	              "the next call is unaffected by the previous call's collected job");
	zassert_equal(sent2, 4u, "the next call's OWN full count, not the stale collected one");
	zassert_equal(
	    slave.sock_send_body_exec_count, 2u, "the second call submitted its OWN fresh job body");
}

/* alp-sdk#2035 review follow-up: a genuinely NEW frame dispatched while a
 * DIFFERENT job is still QUEUED/RUNNING answers BUSY and submits NOTHING
 * (worker_submit_payload refuses a non-IDLE slot) -- the one case a bare
 * clock-threshold fake cannot even represent, since it has no notion of
 * "still running" independent of elapsed time. Call 1's job needs 1000 ms,
 * but its own budget (10 ms) plus the collection grace (250 ms) together
 * cover only ~260 ms, so it gives up with the job STILL pending -- a genuine
 * lower bound, not a bug (see the earlier grace tests for that path
 * verified in isolation). Call 2, moments later with fresh data, must not
 * see that still-running job resubmitted or overwritten: every one of its
 * own attempts finds the slot busy and gives up too, WITHOUT ever executing
 * a body of its own. */
ZTEST(cc3501e_host_driver,
      test_sock_send_new_frame_while_job_still_running_is_busy_not_resubmitted_107)
{
	const uint8_t data1[1] = { 'C' };
	size_t        sent1    = 0u;

	g_sock_send_busy_step_ms[0] = 1000u;
	g_sock_send_busy_step_count = 1u;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data1, sizeof(data1), &sent1, 10u),
	              ALP_ERR_TIMEOUT,
	              "call 1's job needs far longer than its budget + grace -- gives up, still "
	              "genuinely running");
	zassert_equal(sent1, 0u, "a lower bound -- nothing collected yet");
	zassert_equal(slave.sock_send_body_exec_count, 1u, "call 1 submitted its own job exactly once");

	const uint8_t data2[1] = { 'D' };
	size_t        sent2    = 0u;
	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data2, sizeof(data2), &sent2, 50u),
	              ALP_ERR_TIMEOUT,
	              "call 1's job is STILL running -- every attempt is BUSY, never resubmitted");
	zassert_equal(sent2, 0u, "nothing was ever queued for call 2's own data");
	zassert_equal(slave.sock_send_body_exec_count,
	              1u,
	              "still exactly one execution -- call 2 never got to submit its own body");
}

/* alp-sdk#2035 review follow-up: a bridge with cc3501e-bridge-firmware#107
 * (PR #134)'s per-seq reply cache is
 * invalidated the instant a DIFFERENT seq is dispatched (cache_valid=false
 * as soon as an incoming seq mismatches it -- BEFORE anything else), so a
 * host seq that later wraps back to a value the cache once held finds
 * nothing cached for it and EXECUTES instead of aliasing the stale reply.
 * Forces the wrap directly (fw.sock_send_seq) rather than spending 255 real
 * calls to reach it -- the SAME collision, reached the fast way: call 1
 * caches seq 1's own count (3); call 2 (seq 2, a DIFFERENT logical send)
 * both invalidates that entry and leaves its own count (2) cached in its
 * place; forcing the counter back to 0 makes call 3 reuse seq 1 -- which by
 * then matches NEITHER the (already-overwritten) cache nor any pending job,
 * so it submits and executes its OWN fresh body, returning its OWN count
 * (8), not call 1's stale 3. */
ZTEST(cc3501e_host_driver, test_sock_send_wrapped_seq_executes_after_invalidation_107)
{
	const uint8_t data1[3] = { '1', '1', '1' };
	size_t        sent1    = 0u;
	zassert_equal(
	    cc3501e_sock_send(&fw, 0x0034u, data1, sizeof(data1), &sent1, 100u), ALP_OK, "call 1");
	zassert_equal(sent1, 3u, "call 1's own count, now cached under seq 1");

	const uint8_t data2[2] = { '2', '2' };
	size_t        sent2    = 0u;
	zassert_equal(
	    cc3501e_sock_send(&fw, 0x0034u, data2, sizeof(data2), &sent2, 100u), ALP_OK, "call 2");
	zassert_equal(sent2, 2u, "call 2's own count, now cached under seq 2 instead");

	fw.sock_send_seq = 0u; /* force the wrap: the next assigned seq (1) reuses call 1's */

	const uint8_t data3[8]     = { '3', '3', '3', '3', '3', '3', '3', '3' };
	size_t        sent3        = 0u;
	g_sock_send_queue_plan[2]  = 8u; /* call 3 is the THIRD submission (exec_idx 2) */
	g_sock_send_queue_plan_len = 3u;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data3, sizeof(data3), &sent3, 100u),
	              ALP_OK,
	              "the reused seq executes -- neither the (already-overwritten) cache nor any "
	              "pending job matches it");
	zassert_equal(sent3, 8u, "call 3's OWN count, not call 1's stale cached count (3)");
	zassert_equal(slave.sock_send_body_exec_count, 3u, "three genuine executions, not an alias");
}

/* cc3501e-bridge-firmware#107 (alp-sdk#2035): cc3501e_sock_send() must
 * budget each iteration off the genuinely REMAINING time, not a fresh copy
 * of timeout_ms. byte 0 needs 700 ms to clear (resolves directly within the
 * full 1000 ms budget the first iteration always starts with -- no timing
 * difference from a bug here, so this alone would not catch it); byte 1 ALSO
 * needs 700 ms, but by then only ~300 ms of the 1000 ms budget is genuinely
 * left. A correctly-shrunk budget times out around 300 ms in, and the 250 ms
 * collection grace (exercised together here) still cannot reach the 700 ms
 * mark -- so byte 1 is never collected, total elapsed stays near timeout_ms +
 * one grace window, and cc3501e_sock_send() reports ALP_ERR_TIMEOUT with only
 * byte 0's count. A `budget = timeout_ms` regression hands byte 1's iteration
 * a fresh 1000 ms budget instead of the correct ~300 ms -- comfortably
 * enough to resolve the 700 ms requirement DIRECTLY, no timeout, no grace --
 * reporting ALP_OK with both bytes sent, but only after ~1400 ms of real
 * elapsed time: well past any bound a correctly-budgeted implementation could
 * ever produce. */
ZTEST(cc3501e_host_driver, test_sock_send_budgets_each_iteration_off_remaining_time_107)
{
	const uint8_t data[2] = { 'A', 'B' };
	size_t        sent    = 0u;
	uint64_t      base    = g_fake_now_ms;

	g_sock_send_busy_step_ms[0] = 700u;
	g_sock_send_busy_step_ms[1] = 700u;
	g_sock_send_busy_step_count = 2u;

	alp_status_t s = cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 1000u);

	uint64_t elapsed = g_fake_now_ms - base;
	zassert_true(elapsed <= 1350u,
	             "correct per-iteration budgeting bounds total elapsed time (got %llu ms)",
	             (unsigned long long)elapsed);
	zassert_equal(s, ALP_ERR_TIMEOUT, "byte 1's 700 ms requirement exceeds budget + grace");
	zassert_equal(sent, 1u, "only byte 0 (700 ms, fit the full first-iteration budget) collected");
}

/* cc3501e-bridge-firmware#107 (alp-sdk#2035): a decoded ALP_OK reply with
 * fewer than 2 data bytes is not a valid queued-byte count -- a firmware/wire
 * gap, not backpressure. Silently treating it as "0 queued" would fold it
 * into the zero-progress back-off path and spin against a malformed reply
 * for the whole budget instead of reporting the real problem immediately. */
ZTEST(cc3501e_host_driver, test_sock_send_short_ok_reply_is_io_not_zero_progress)
{
	const uint8_t data[5] = { 'G', 'E', 'T', ' ', '/' };
	size_t        sent    = 123u; /* poison -- must come back 0, not left untouched */

	g_sock_send_short_reply = true;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 1000u),
	              ALP_ERR_IO,
	              "a <2-byte OK reply is a wire gap, not backpressure -- ALP_ERR_IO immediately");
	zassert_equal(sent, 0u, "no bytes queued");
	zassert_equal(slave.sock_send_log_count,
	              0u,
	              "one dispatch and done -- not folded into the zero-progress retry loop");
}

/* alp-sdk#2035 review follow-up: the "do not start a new iteration once less
 * than one back-off's worth of budget remains" guard must re-read the clock
 * AFTER the zero-progress back-off sleep, not just before it -- otherwise a
 * remaining budget that looked sufficient right before a 20 ms sleep can be
 * far too small right after it, and the next iteration starts anyway.
 *
 * Combines an EXPLICIT zero-progress result (queue_plan[0] = 0, so the
 * back-off actually fires) with a real busy wait (busy_step_ms[0] = 2 ms) so
 * iteration 1 costs a few real ms before it resolves to "0 queued": iteration
 * 1 resolves comfortably inside the 30 ms budget, leaving ~8 ms after its 20
 * ms back-off -- correctly insufficient (< 20 ms) for iteration 2, so a
 * correct implementation stops there with ALP_ERR_TIMEOUT and 0 bytes sent.
 * A version that only checks BEFORE the sleep (using the ampler pre-sleep
 * remaining budget) proceeds to iteration 2 anyway; that iteration's own
 * busy_step_ms (reused from the same array's last entry) resolves well
 * within ITS remaining budget, queuing the whole rest of the buffer and
 * returning ALP_OK instead. */
ZTEST(cc3501e_host_driver, test_sock_send_rechecks_budget_after_backoff_sleep_107)
{
	const uint8_t data[1] = { 'Z' };
	size_t        sent    = 0u;

	g_sock_send_queue_plan[0]   = 0u;
	g_sock_send_queue_plan_len  = 1u;
	g_sock_send_busy_step_ms[0] = 2u;
	g_sock_send_busy_step_count = 1u;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 30u),
	              ALP_ERR_TIMEOUT,
	              "insufficient budget after the back-off's own sleep -- stop, don't start a "
	              "doomed iteration 2");
	zassert_equal(sent, 0u, "iteration 1 made zero progress; iteration 2 must not have run");
	zassert_equal(slave.sock_send_log_count, 1u, "exactly one iteration was collected");
}

/* alp-sdk#2035 review follow-up: a transport-lock-acquire timeout on
 * poll_by_repeat()'s very FIRST attempt (cc3501e_lock_acquire(),
 * cc3501e_core.c) means no frame ever reached the bridge -- unambiguous,
 * returned directly as ALP_ERR_BUSY, no collection grace needed (the grace
 * exists to collect a frame that MIGHT already be in flight; here, nothing
 * ever left the host). Holds the REAL lock seam (fw.request_lock) for the
 * whole call -- see g_lock_contention_arm_on_submit's comment for the OTHER
 * (later-attempt) half of this pair, just below. */
ZTEST(cc3501e_host_driver, test_sock_send_first_attempt_lock_timeout_returns_busy_directly_107)
{
	const uint8_t data[1] = { 'A' };
	size_t        sent    = 123u; /* poison -- must come back 0, not left untouched */

	fw.request_lock = true; /* held for the whole call -- a concurrent caller never lets go */

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 100u),
	              ALP_ERR_BUSY,
	              "a lock timeout on the very FIRST attempt is unambiguous -- BUSY, directly, "
	              "no grace");
	zassert_equal(sent, 0u, "nothing queued");
	zassert_equal(
	    slave.sock_send_body_exec_count, 0u, "no frame ever reached the bridge to execute");
}

/* alp-sdk#2035 review follow-up: a transport-lock-acquire timeout on a LATER
 * attempt is different -- an EARLIER attempt already reached the bridge (the
 * very first SOCK_SEND submission below succeeds and gets a genuine
 * RESP_ERR_BUSY from the firmware), so returning ALP_ERR_BUSY immediately
 * would misreport "nothing sent" for a job that may already be running.
 * poll_by_repeat() now retries such a timeout within its own deadline
 * instead: g_lock_contention_arm_on_submit forces the REAL lock
 * (fw.request_lock) externally held for LOCK_CONTENTION_WINDOW_MS starting
 * right after that first submission, comfortably exceeding
 * CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS's internal spin (so a
 * lock-acquire attempt landing in the window genuinely times out, not just
 * gets slow), then releases -- a later attempt then succeeds and collects
 * the job (which has long since become ready, using the default "ready on
 * its very next poll" -- no busy_step needed here). timeout_ms(400) is
 * generous against the ~150 ms this needs. */
ZTEST(cc3501e_host_driver, test_sock_send_later_attempt_lock_timeout_is_retried_and_collected_107)
{
	const uint8_t data[1] = { 'B' };
	size_t        sent    = 0u;

	g_lock_contention_arm_on_submit = true;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 400u),
	              ALP_OK,
	              "a LATER attempt's lock timeout is retried within budget, not returned "
	              "immediately -- the job still completes once the lock frees up again");
	zassert_equal(sent, 1u, "the send was collected once retried past the lock contention");
}

/* alp-sdk#2035 review follow-up: if the collection grace collects a genuine,
 * definitive non-OK status -- e.g. a decoded device-side error such as a
 * peer reset -- that status must be returned directly, not masked as
 * ALP_ERR_TIMEOUT: the frame is DONE (it will never resolve into a queued
 * count), so *sent_out is exact, not a lower bound. timeout_ms(10) is far
 * shorter than busy_step_ms(30), so the original attempt is guaranteed to
 * time out before the job resolves; the grace then collects the staged
 * ALP_CC3501E_RESP_ERR_RADIO instead of a queued count. */
ZTEST(cc3501e_host_driver, test_sock_send_grace_surfaces_genuine_device_error_107)
{
	const uint8_t data[1] = { 'R' };
	size_t        sent    = 123u; /* poison -- must come back 0, not left untouched */

	g_sock_send_busy_step_ms[0]  = 30u;
	g_sock_send_busy_step_count  = 1u;
	g_sock_send_resolve_as_error = ALP_CC3501E_RESP_ERR_RADIO;

	zassert_equal(cc3501e_sock_send(&fw, 0x0034u, data, sizeof(data), &sent, 10u),
	              ALP_ERR_IO,
	              "a genuine decoded device error collected via grace is surfaced directly, "
	              "not masked as ALP_ERR_TIMEOUT");
	zassert_equal(sent, 0u, "this frame is DONE, not merely timed out -- exact, not a lower bound");
}

/* RECV requests up to @cap bytes and decodes the 24-byte recv-resp header +
 * the inline received bytes that follow it. */
ZTEST(cc3501e_host_driver, test_sock_recv_encodes_maxlen_and_decodes_data)
{
	uint8_t buf[32] = { 0 };
	size_t  got     = 0u;
	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 100u), ALP_OK, "RECV -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_RECV, "opcode 0x23");
	zassert_equal(slave.req_len, 4u, "recv payload = {handle(LE16), max_len(LE16)}");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
	zassert_equal(slave.req_pl[2], 32u, "max_len lo (= cap, bounded)");
	zassert_equal(got, 5u, "decoded data_len from the 24-byte resp header");
	zassert_mem_equal(buf, "hello", 5u, "inline received bytes copied out");
}

/* ---- alp-sdk#2108: SOCK_RECV's dedicated retry-seq counter ----------------
 *
 * cc3501e_sock_recv() must draw its wire-header retry seq from its OWN
 * ctx->sock_recv_seq, never from the ctx->req_seq every other worker-routed
 * opcode shares -- see ctx->sock_recv_seq's comment in
 * <alp/chips/cc3501e/core.h> for the full aliasing mechanism this guards
 * against, and for why ONE counter shared across every socket handle is
 * sufficient (the two tests below prove that directly: the same-handle case
 * on a shared req_seq, and the different-handle case on the dedicated
 * counter itself).
 *
 * The pending firmware fix this counter is groundwork for
 * (cc3501e-bridge-firmware fix/sock-recv-retry-safe) is NOT modelled by
 * default -- test_sock_recv_encodes_maxlen_and_decodes_data above keeps the
 * suite's original fixed reply. The two "reuses seq" tests further below
 * (retry-within-one-call, CRC-fail-then-retry) pass on the UNFIXED parent
 * commit too, because poll_by_repeat()'s own internal retry loop already
 * held one seq constant across its retries before this issue -- they guard
 * that existing property still holds for the new dedicated-counter path,
 * they are not proof of the alias fix. The real proof is the two tests
 * immediately below, plus the advance-on-success test further down. */

/* Two DIFFERENT, both-successful recvs on the SAME handle must never share a
 * seq, no matter how many unrelated OTHER-OPCODE commands (each consuming
 * ctx->req_seq, which SOCK_RECV must NOT be drawing from) run in between. 30
 * intervening allocations is the exact step count that walks the SHARED
 * 31-wide counter (1..31, skipping 0) through one full cycle back to its
 * starting value: if cc3501e_sock_recv() regressed to sharing ctx->req_seq,
 * the first RECV would be that counter's allocation #1 and, with 30 more
 * allocations from the intervening commands, the second RECV would land on
 * allocation #32 -- the same value as allocation #1 -- aliasing the two
 * recvs. A dedicated counter is immune regardless of how many intervening
 * commands there are; this pins the exact count that would expose a
 * reversion. */
ZTEST(cc3501e_host_driver, test_sock_recv_seq_does_not_alias_across_intervening_commands)
{
	uint8_t buf[8] = { 0 };
	size_t  got    = 0u;

	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 100u), ALP_OK, "first RECV");
	const uint8_t first_seq = seq_of(slave.flags);
	zassert_not_equal(first_seq,
	                  ALP_CC3501E_REQ_SEQ_NONE,
	                  "a retryable command must carry a real seq, not the reserved 0");

	int8_t rssi = 0;
	for (uint32_t i = 0; i < 30u; i++) {
		zassert_equal(
		    cc3501e_wifi_rssi(&fw, &rssi), ALP_OK, "intervening command consumes ctx->req_seq");
	}

	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 100u), ALP_OK, "second RECV");
	const uint8_t second_seq = seq_of(slave.flags);
	zassert_not_equal(second_seq,
	                  first_seq,
	                  "two DIFFERENT recvs on the same handle must never share a seq, or the "
	                  "firmware's lazy-commit ring would replay the FIRST recv's bytes instead of "
	                  "advancing (alp-sdk#2108)");
}

/* Aliasing across DIFFERENT handles, on the dedicated counter itself
 * (alp-sdk#2108 review): ONE per-ctx counter shared across every handle is
 * enough, because the firmware records its (seq, handle) key on EVERY
 * SOCK_RECV dispatch and always compares the CURRENT dispatch only against
 * that single most recent entry, never against history.
 *
 * recv(A), 30 recvs on handle B, recv(A) again is the exact scenario that
 * argument has to hold for: with a plain per-call increment (1..31, skip 0),
 * the SECOND recv(A) numerically repeats the FIRST recv(A)'s seq -- a full
 * 31-count cycle -- and that repeat is EXPECTED, not a bug. What must never
 * happen is the second recv(A) being served the first recv(A)'s bytes back:
 * it is compared against B's last dispatch (a different handle), never
 * against A's own stale first entry, so it must commit A's NEXT chunk. */
ZTEST(cc3501e_host_driver, test_sock_recv_seq_does_not_alias_across_different_handles)
{
	static const uint8_t source[32] = { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
		                                11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
		                                22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
	memcpy(g_sock_recv_source, source, sizeof(source));
	g_sock_recv_source_len     = sizeof(source);
	g_sock_recv_chunk_len      = 1u; /* one byte committed per recv, to keep the two handles'
	                                * chunk sequences trivial to reason about */
	g_sock_recv_use_ring_model = true;

	uint8_t buf[4] = { 0 };
	size_t  got    = 0u;

	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x00AAu, buf, sizeof(buf), &got, 100u), ALP_OK, "recv(A) #1");
	const uint8_t seq_a1 = seq_of(slave.flags);
	zassert_equal(got, 1u, "one-byte chunk");
	zassert_equal(buf[0], source[0], "A's first chunk is source[0]");

	for (uint32_t i = 0; i < 30u; i++) {
		zassert_equal(
		    cc3501e_sock_recv(&fw, 0x00BBu, buf, sizeof(buf), &got, 100u), ALP_OK, "recv(B)");
	}

	zassert_equal(
	    cc3501e_sock_recv(&fw, 0x00AAu, buf, sizeof(buf), &got, 100u), ALP_OK, "recv(A) #2");
	const uint8_t seq_a2 = seq_of(slave.flags);
	zassert_equal(seq_a2,
	              seq_a1,
	              "the shared-across-handles counter DOES wrap back to the same numeric value "
	              "here -- expected, and harmless (see the comment above)");
	zassert_equal(got, 1u, "still a one-byte chunk");
	/* source[31], not source[0]: this simplified fake models ONE shared
	 * receive stream, not one ring per handle (see the g_sock_recv_* globals'
	 * own comment) -- A's own 1 byte plus B's 30 bytes have already consumed
	 * source[0..30], so the 32nd genuinely NEW commit (this call) is
	 * source[31]. The exact value is secondary; what matters is that it is
	 * NOT source[0] -- a replay of recv(A) #1's own reply -- despite the
	 * numeric seq coincidence confirmed above. */
	zassert_equal(buf[0],
	              source[31],
	              "A's SECOND commit continues the shared stream, not a replay of its first entry "
	              "(source[0]) -- proves the numeric seq coincidence with recv(A) #1 was never "
	              "mistaken for a replay");
	zassert_equal(g_sock_recv_commit_count,
	              32u,
	              "all 32 recvs committed -- none of them, including the second recv(A), replayed");
}

/* Regression guard, NOT proof of the alias fix (see this section's own intro
 * comment above): a retry of ONE cc3501e_sock_recv() call
 * (poll_by_repeat_seq()'s own internal BUSY retry loop) must re-send the
 * SAME seq on every attempt -- exactly the property
 * test_retry_seq_is_constant_across_one_commands_retries already pins for
 * the generic req_seq path. This passes on the unfixed parent commit too;
 * it only confirms the property still holds once SOCK_RECV supplies its
 * seq from a dedicated counter instead. */
ZTEST(cc3501e_host_driver, test_sock_recv_retry_within_one_call_reuses_seq)
{
	g_sock_recv_busy_polls_remaining = 3u; /* 3 BUSY acks, then the real reply */
	uint8_t buf[8]                   = { 0 };
	size_t  got                      = 0u;

	zassert_equal(cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 1000u),
	              ALP_OK,
	              "RECV -> OK after riding out BUSY");
	zassert_true(slave.flags_log_count >= 4u, "3 BUSY attempts + the collect were clocked");

	const uint8_t seq = seq_of(slave.flags_log[0]);
	zassert_not_equal(
	    seq, ALP_CC3501E_REQ_SEQ_NONE, "a retryable command must carry a real seq, not 0");
	for (uint32_t i = 1u; i < slave.flags_log_count && i < ARRAY_SIZE(slave.flags_log); i++) {
		zassert_equal(seq_of(slave.flags_log[i]),
		              seq,
		              "every retry of ONE cc3501e_sock_recv() call re-sends the SAME seq");
	}
}

/* Regression guard, NOT proof of the alias fix (see this section's own intro
 * comment above): a SOCK_RECV reply that fails CRC on the wire is re-issued
 * by poll_by_repeat_seq()'s OWN internal retry loop with the SAME seq, so
 * the ring model's (seq, handle) match replays the already-committed bytes
 * instead of committing again. This exercises the ring model end to end, but
 * the underlying "one seq across one call's internal retries" property
 * already held on the unfixed parent commit (poll_by_repeat() has always
 * held a single seq constant across ITS OWN retry loop, for every opcode) --
 * this pins that it still holds for the dedicated sock_recv_seq path, it
 * does not by itself prove #2108's cross-call alias is fixed. */
ZTEST(cc3501e_host_driver, test_sock_recv_crc_fail_then_retry_yields_correct_bytes_no_gap_no_dup)
{
	static const uint8_t source[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	memcpy(g_sock_recv_source, source, sizeof(source));
	g_sock_recv_source_len            = sizeof(source);
	g_sock_recv_chunk_len             = 5u;
	g_sock_recv_use_ring_model        = true;
	g_sock_recv_corrupt_crc_remaining = 1u; /* the FIRST reply's CRC fails on the wire */

	uint8_t buf[8] = { 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu };
	size_t  got    = 0u;
	zassert_equal(cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 1000u),
	              ALP_OK,
	              "the internal retry re-collects the committed reply");
	zassert_equal(got, 5u, "no gap: the full first chunk arrived");
	zassert_mem_equal(buf, &source[0], 5u, "no gap, no duplicate: exactly chunk 1's bytes");
	zassert_equal(g_sock_recv_commit_count,
	              1u,
	              "the ring advanced exactly ONCE for this one logical recv -- the retry replayed "
	              "the cache, it did not commit a second time");

	/* A second, genuinely NEW recv must get the NEXT chunk, not a replay of
	 * the first (proves the dedicated counter also keeps two back-to-back
	 * real recvs from aliasing under the ring model itself). */
	zassert_equal(cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 1000u),
	              ALP_OK,
	              "second RECV -> OK");
	zassert_equal(got, 5u, "second chunk is also 5 bytes");
	zassert_mem_equal(buf, &source[5], 5u, "no gap, no duplicate: exactly chunk 2's bytes");
	zassert_equal(g_sock_recv_commit_count, 2u, "the ring advanced a second time for the new recv");
}

/* THE real proof of alp-sdk#2108's advance-on-success fix (review item 4):
 * cc3501e_sock_recv() must NOT commit its candidate seq into
 * ctx->sock_recv_seq when its own call fails -- here, every reply on the
 * wire fails CRC for long enough that poll_by_repeat_seq()'s own retry
 * budget is exhausted and the WHOLE call reports ALP_ERR_TIMEOUT, even
 * though the firmware genuinely committed chunk 1 on its very first
 * (uncollected) attempt. Keeping the candidate seq lets the caller's own
 * retry re-issue the SAME (seq, handle) the lost dispatch used, so the ring
 * model replays chunk 1 instead of treating the retry as a request for
 * chunk 2. */
ZTEST(cc3501e_host_driver, test_sock_recv_timeout_keeps_seq_and_retry_recovers_the_chunk)
{
	static const uint8_t source[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	memcpy(g_sock_recv_source, source, sizeof(source));
	g_sock_recv_source_len     = sizeof(source);
	g_sock_recv_chunk_len      = 5u;
	g_sock_recv_use_ring_model = true;
	/* Large enough to outlast this call's whole internal retry budget (a
	 * handful of attempts inside a 5 ms deadline, see poll_by_repeat_seq()'s
	 * back-off schedule) -- every attempt this call makes fails CRC. */
	g_sock_recv_corrupt_crc_remaining = 1000u;

	uint8_t buf[8] = { 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu, 0xAAu };
	size_t  got    = 0u;
	zassert_equal(cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 5u),
	              ALP_ERR_TIMEOUT,
	              "every reply on the wire fails CRC -- the whole call times out");
	zassert_equal(g_sock_recv_commit_count,
	              1u,
	              "the firmware DID commit chunk 1 on its first attempt -- only the REPLY was "
	              "ever lost, never collected by the host");

	/* Let the retry's reply through undamaged. */
	g_sock_recv_corrupt_crc_remaining = 0u;
	zassert_equal(cc3501e_sock_recv(&fw, 0x0034u, buf, sizeof(buf), &got, 1000u),
	              ALP_OK,
	              "the retry recovers the lost reply");
	zassert_equal(got, 5u, "no gap: chunk 1's full 5 bytes arrived");
	zassert_mem_equal(buf, &source[0], 5u, "exactly chunk 1's bytes -- not chunk 2's");
	zassert_equal(g_sock_recv_commit_count,
	              1u,
	              "still exactly ONE commit -- the retry replayed the kept seq instead of "
	              "advancing past the still-unseen chunk 1");
}

ZTEST(cc3501e_host_driver, test_sock_close_encodes_handle)
{
	zassert_equal(cc3501e_sock_close(&fw, 0x0034u, 100u), ALP_OK, "CLOSE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_CLOSE, "opcode 0x24");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
}

/* BIND packs the SOCK_CONNECT layout with the LOCAL endpoint, so the two parse
 * identically firmware-side.  A NULL ip is INADDR_ANY -- the addr field must
 * come out all-zero, NOT uninitialised stack, because that is what a server on
 * the soft-AP binds. */
ZTEST(cc3501e_host_driver, test_sock_bind_null_ip_is_inaddr_any)
{
	static const uint8_t zeros[16] = { 0 };

	zassert_equal(cc3501e_sock_bind(&fw, 0x0034u, NULL, 80u, 100u), ALP_OK, "BIND -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_BIND, "opcode 0x25");
	zassert_equal(slave.req_len, 24u, "bind payload is 24 bytes, same as connect");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
	zassert_equal(slave.req_pl[4], (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "local.family");
	zassert_equal(slave.req_pl[6], 80u, "local.port lo (host order on the wire)");
	zassert_equal(slave.req_pl[7], 0u, "local.port hi");
	zassert_mem_equal(&slave.req_pl[8], zeros, 16u, "local.addr all-zero = INADDR_ANY");
}

ZTEST(cc3501e_host_driver, test_sock_bind_explicit_ip_encodes_octets)
{
	const uint8_t ip[4] = { 10, 0, 0, 3 };

	zassert_equal(cc3501e_sock_bind(&fw, 0x0001u, ip, 8080u, 100u), ALP_OK, "BIND -> OK");
	zassert_equal(slave.req_pl[6], (uint8_t)(8080u & 0xFFu), "local.port lo");
	zassert_equal(slave.req_pl[7], (uint8_t)(8080u >> 8), "local.port hi");
	zassert_mem_equal(&slave.req_pl[8], ip, 4u, "local.addr[0..3] = the IPv4 octets");
}

ZTEST(cc3501e_host_driver, test_sock_listen_encodes_backlog)
{
	zassert_equal(cc3501e_sock_listen(&fw, 0x0034u, 4u, 100u), ALP_OK, "LISTEN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SOCK_LISTEN, "opcode 0x26");
	zassert_equal(slave.req_len, 4u, "listen payload = {handle(LE16), backlog, rsvd}");
	zassert_equal(slave.req_pl[0], 0x34u, "handle lo");
	zassert_equal(
	    slave.req_pl[1],
	    0x00u,
	    "handle hi (wire is now always low-byte-only -- epoch lives host-side, not on the wire)");
	zassert_equal(slave.req_pl[2], 4u, "backlog");
	zassert_equal(slave.req_pl[3], 0u, "reserved stays 0");
}

/* The accepted-connection event decoder.  It exists so a callback never casts
 * its payload pointer, which aims into the driver's event buffer at whatever
 * offset the entry landed on and so carries no alignment guarantee. */
ZTEST(cc3501e_host_driver, test_sock_accepted_decode_fields)
{
	/* listen_handle=1 | handle=2 | peer_port=54321 (0xD431) | family=IPV4 |
	 * rsvd | peer_addr = 192.168.1.14 (network order, MSB first). */
	const uint8_t wire[12] = { 0x01, 0x00, 0x02, 0x00, 0x31, 0xD4, 0x00, 0x00, 192, 168, 1, 14 };
	alp_cc3501e_sock_accepted_evt_t ev = { 0 };

	zassert_equal(
	    cc3501e_sock_accepted_decode(wire, sizeof(wire), 0u, &ev), ALP_OK, "decode -> OK");
	zassert_equal(
	    ev.listen_handle, 1u, "listen_handle (epoch 0 -- byte-identical to the raw wire)");
	zassert_equal(ev.handle, 2u, "handle (epoch 0 -- byte-identical to the raw wire)");
	zassert_equal(ev.peer_port, 54321u, "peer_port (host order)");
	zassert_equal(ev.peer_family, (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4, "peer_family");
	zassert_mem_equal(ev.peer_addr, &wire[8], 4u, "peer_addr copied verbatim");
}

ZTEST(cc3501e_host_driver, test_sock_accepted_decode_rejects_short_and_null)
{
	const uint8_t                   wire[11] = { 0 };
	alp_cc3501e_sock_accepted_evt_t ev       = { 0 };

	/* A truncated entry must be REJECTED, not decoded from whatever follows:
	 * the handle it would invent is a firmware socket the host would then try
	 * to recv on and close. */
	zassert_equal(cc3501e_sock_accepted_decode(wire, sizeof(wire), 0u, &ev),
	              ALP_ERR_INVAL,
	              "11 bytes is short -> INVAL");
	zassert_equal(ev.handle, 0u, "out left untouched on a short payload");
	zassert_equal(cc3501e_sock_accepted_decode(NULL, 12u, 0u, &ev), ALP_ERR_INVAL, "NULL payload");
	zassert_equal(cc3501e_sock_accepted_decode(wire, 12u, 0u, NULL), ALP_ERR_INVAL, "NULL out");
}

/* ================================ BLE ====================================== */

ZTEST(cc3501e_host_driver, test_ble_enable_disable_argless)
{
	zassert_equal(cc3501e_ble_enable(&fw, 100u), ALP_OK, "BLE_ENABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_ENABLE, "opcode 0x30");
	slave_reset();
	zassert_equal(cc3501e_ble_disable(&fw, 100u), ALP_OK, "BLE_DISABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_DISABLE, "opcode 0x31");
}

/* BLE_SCAN_START reply is a packed sequence of advertising reports; the host
 * walks them, copying + NUL-terminating each length-prefixed device name. */
ZTEST(cc3501e_host_driver, test_ble_scan_walks_records)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 0u;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_SCAN_START, "opcode 0x34");
	zassert_equal(n, 2u, "two advertisers parsed");

	zassert_equal(recs[0].addr_type, 0u, "rec0 public addr");
	zassert_equal(recs[0].rssi_dbm, -55, "rec0 rssi (signed)");
	zassert_str_equal(recs[0].name, "MyBLE", "rec0 name copied + NUL-terminated");

	zassert_equal(recs[1].addr_type, 1u, "rec1 random addr");
	zassert_equal(recs[1].rssi_dbm, -88, "rec1 rssi");
	zassert_equal(recs[1].name_len, 0u, "rec1 nameless");
	zassert_str_equal(recs[1].name, "", "rec1 name empty");
}

/* #740: same non-aliasing + explicit-BUSY guarantees as
 * test_wifi_scan_buf_is_per_context_740 / test_wifi_scan_busy_rejects_reentrant_same_ctx_740,
 * for the BLE scan decode buffer (ctx->ble_scan_buf). See
 * test_wifi_scan_buf_is_per_context_740's comment for why this form (raw
 * wire-byte compare + a genuinely distinct ctx B reply) actually
 * discriminates the pre-#740 function-local `static` buffer, unlike the
 * original snapshot-vs-itself version of this test. */
ZTEST(cc3501e_host_driver, test_ble_scan_buf_is_per_context_740)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 0u;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN ctx A -> OK");
	zassert_equal(n, 2u, "two advertisers parsed on ctx A");

	uint8_t  wire_a[ALP_CC3501E_MAX_PAYLOAD];
	uint16_t wire_a_len = build_ble_scan(wire_a);
	zassert_mem_equal(fw.ble_scan_buf,
	                  wire_a,
	                  wire_a_len,
	                  "ctx A's cc3501e_ble_scan must decode into ctx->ble_scan_buf itself "
	                  "(#740) -- fails against the pre-fix function-local `static` buffer, "
	                  "which never touches this field");

	uint8_t snapshot[ALP_CC3501E_MAX_PAYLOAD];
	memcpy(snapshot, fw.ble_scan_buf, sizeof(snapshot));

	cc3501e_t ctx_b;
	zassert_equal(cc3501e_init(&ctx_b, fake_bus), ALP_OK, "init ctx B");
	slave_reset();
	g_scan_stage_ctx_b = true;
	zassert_equal(cc3501e_ble_scan(&ctx_b, recs, 8u, &n, 100u), ALP_OK, "BLE_SCAN ctx B -> OK");
	zassert_equal(n, 1u, "ctx B's distinct single-record reply parsed");
	zassert_str_equal(recs[0].name, "Ctx2Dev", "ctx B decoded ITS OWN staged name");

	zassert_mem_equal(fw.ble_scan_buf,
	                  snapshot,
	                  sizeof(snapshot),
	                  "ctx A's BLE scan buffer must be unaffected by ctx B's OWN real scan (#740)");
}

ZTEST(cc3501e_host_driver, test_ble_scan_busy_rejects_reentrant_same_ctx_740)
{
	cc3501e_ble_scan_record_t recs[8];
	size_t                    n = 123u;

	fw.ble_scan_busy = true;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_ERR_BUSY, "reentrant -> BUSY");
	zassert_equal(slave.cmd, 0u, "no transfer clocked while busy");
	zassert_equal(n, 0u, "count still reset to 0 before the busy check");

	fw.ble_scan_busy = false;
	zassert_equal(cc3501e_ble_scan(&fw, recs, 8u, &n, 100u), ALP_OK, "cleared -> scan proceeds");
	zassert_equal(n, 2u, "normal scan after the busy flag clears");
}

ZTEST(cc3501e_host_driver, test_ble_scan_null_ctx_not_ready)
{
	cc3501e_ble_scan_record_t recs[8];
	zassert_equal(
	    cc3501e_ble_scan(NULL, recs, 8u, NULL, 100u), ALP_ERR_NOT_READY, "NULL ctx -> NOT_READY");
}

/* ADV_START hand-packs the 7-byte header (the doc struct's 8th pad byte is
 * omitted on the wire) then the inline advertising data. */
ZTEST(cc3501e_host_driver, test_ble_adv_start_encodes_7byte_header)
{
	const uint8_t adv[3] = { 0x02, 0x01, 0x06 }; /* flags AD */
	zassert_equal(cc3501e_ble_adv_start(&fw, true, 100u, 200u, adv, sizeof(adv), 100u),
	              ALP_OK,
	              "ADV_START -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_ADV_START, "opcode 0x32");
	zassert_equal(slave.req_len, 7u + 3u, "payload = 7-byte header + adv data");
	zassert_equal(slave.req_pl[0], 1u, "connectable");
	zassert_equal(slave.req_pl[1], 0u, "reserved");
	zassert_equal(
	    (uint16_t)slave.req_pl[2] | ((uint16_t)slave.req_pl[3] << 8), 100u, "interval_min_ms LE16");
	zassert_equal(
	    (uint16_t)slave.req_pl[4] | ((uint16_t)slave.req_pl[5] << 8), 200u, "interval_max_ms LE16");
	zassert_equal(slave.req_pl[6], 3u, "adv_data_len");
	zassert_mem_equal(&slave.req_pl[7], adv, 3u, "inline adv data");
}

/* BLE_CONNECT packs addr_type FIRST, then the 6 address bytes. */
ZTEST(cc3501e_host_driver, test_ble_connect_encodes_addr_type_first)
{
	const uint8_t addr[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 };
	zassert_equal(cc3501e_ble_connect(&fw, addr, 1u, 100u), ALP_OK, "BLE_CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_CONNECT, "opcode 0x36");
	zassert_equal(slave.req_len, 7u, "payload = addr_type(1) + addr(6)");
	zassert_equal(slave.req_pl[0], 1u, "addr_type first");
	zassert_mem_equal(&slave.req_pl[1], addr, 6u, "addr[6] after addr_type");
}

/* GATT_WRITE packs handle(LE16) then the value bytes. */
ZTEST(cc3501e_host_driver, test_ble_gatt_write_encodes_handle_and_value)
{
	const uint8_t val[3] = { 0x11, 0x22, 0x33 };
	zassert_equal(
	    cc3501e_ble_gatt_write(&fw, 0x0042u, val, sizeof(val), 100u), ALP_OK, "GATT_WRITE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_GATT_WRITE, "opcode 0x3B");
	zassert_equal(slave.req_len, 2u + 3u, "payload = handle(LE16) + value");
	zassert_equal(slave.req_pl[0], 0x42u, "handle lo");
	zassert_equal(slave.req_pl[1], 0x00u, "handle hi");
	zassert_mem_equal(&slave.req_pl[2], val, 3u, "value bytes after handle");
}

/* GATT_READ requests handle(LE16); the reply DATA is the attribute value. */
ZTEST(cc3501e_host_driver, test_ble_gatt_read_decodes_value)
{
	uint8_t out[8] = { 0 };
	size_t  n      = 0u;
	zassert_equal(
	    cc3501e_ble_gatt_read(&fw, 0x0042u, out, sizeof(out), &n, 100u), ALP_OK, "GATT_READ -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_GATT_READ, "opcode 0x3A");
	zassert_equal(slave.req_len, 2u, "request = handle(LE16)");
	zassert_equal(slave.req_pl[0], 0x42u, "handle lo");
	zassert_equal(n, 2u, "decoded value length");
	zassert_equal(out[0], 0xABu, "value[0]");
	zassert_equal(out[1], 0xCDu, "value[1]");
}

/* ======================== GPIO PROXY + POWER =============================== */

/* Configure -> write-high -> read-back-high -> write-low -> read-back-low
 * round-trips through the real wire encode/decode against the pin model. */
ZTEST(cc3501e_host_driver, test_gpio_configure_write_read_roundtrip)
{
	zassert_equal(cc3501e_gpio_configure(
	                  &fw, 13u, ALP_CC3501E_GPIO_DIR_OUTPUT, ALP_CC3501E_GPIO_PULL_NONE, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_CONFIGURE, "opcode 0x50");
	zassert_equal(slave.req_pl[0], 13u, "pad index");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_GPIO_DIR_OUTPUT, "direction");

	zassert_equal(cc3501e_gpio_write(&fw, 13u, true, 100u), ALP_OK, "WRITE high -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_WRITE, "opcode 0x51");
	zassert_equal(slave.req_pl[0], 13u, "pad index");
	zassert_equal(slave.req_pl[1], 1u, "level high");

	bool level = false;
	zassert_equal(cc3501e_gpio_read(&fw, 13u, &level, 100u), ALP_OK, "READ -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_READ, "opcode 0x52");
	zassert_true(level, "read reflects the written-high level");

	zassert_equal(cc3501e_gpio_write(&fw, 13u, false, 100u), ALP_OK, "WRITE low -> OK");
	zassert_equal(cc3501e_gpio_read(&fw, 13u, &level, 100u), ALP_OK, "READ -> OK");
	zassert_false(level, "read reflects the written-low level");
}

ZTEST(cc3501e_host_driver, test_gpio_read_null_out_invalid)
{
	zassert_equal(cc3501e_gpio_read(&fw, 13u, NULL, 100u), ALP_ERR_INVAL, "NULL out -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

ZTEST(cc3501e_host_driver, test_gpio_set_interrupt_encodes_fields)
{
	zassert_equal(cc3501e_gpio_set_interrupt(&fw, 7u, ALP_CC3501E_GPIO_EDGE_RISING, true, 100u),
	              ALP_OK,
	              "SET_INTERRUPT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GPIO_SET_INTERRUPT, "opcode 0x53");
	zassert_equal(slave.req_pl[0], 7u, "pad index");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_GPIO_EDGE_RISING, "edge");
	zassert_equal(slave.req_pl[2], 1u, "enabled");
}

ZTEST(cc3501e_host_driver, test_cam_enable_disable_selects_opcode)
{
	zassert_equal(cc3501e_cam_enable(&fw, 1u, true, 100u), ALP_OK, "CAM_ENABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_CAM_ENABLE, "on -> opcode 0x60");
	zassert_equal(slave.req_pl[0], 1u, "which LDO");
	slave_reset();
	zassert_equal(cc3501e_cam_enable(&fw, 0u, false, 100u), ALP_OK, "CAM_DISABLE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_CAM_DISABLE, "off -> opcode 0x61");
}

/* POWER_POLICY hand-packs the 8-byte wire (policy | wake | rsvd(2) | idle(LE32)),
 * NOT the doc struct which carries alignment padding. */
ZTEST(cc3501e_host_driver, test_power_policy_encodes_8_bytes)
{
	const alp_cc3501e_power_policy_t pp = {
		.policy               = ALP_CC3501E_PP_BALANCED,
		.wake_events          = ALP_CC3501E_WAKE_HOST_SPI,
		.reserved             = 0u,
		.idle_ms_before_sleep = 1000u, /* 0x000003E8 */
	};
	zassert_equal(cc3501e_power_policy(&fw, &pp, NULL, 100u), ALP_OK, "POWER_POLICY -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_POWER_POLICY, "opcode 0x62");
	zassert_equal(slave.req_len, 8u, "hand-packed 8-byte wire");
	zassert_equal(slave.req_pl[0], (uint8_t)ALP_CC3501E_PP_BALANCED, "policy");
	zassert_equal(slave.req_pl[1], (uint8_t)ALP_CC3501E_WAKE_HOST_SPI, "wake_events");
	zassert_equal(slave.req_pl[2], 0u, "reserved lo");
	zassert_equal(slave.req_pl[3], 0u, "reserved hi");
	zassert_equal((uint32_t)slave.req_pl[4] | ((uint32_t)slave.req_pl[5] << 8) |
	                  ((uint32_t)slave.req_pl[6] << 16) | ((uint32_t)slave.req_pl[7] << 24),
	              1000u,
	              "idle_ms_before_sleep LE32");
}

ZTEST(cc3501e_host_driver, test_power_policy_null_invalid)
{
	zassert_equal(
	    cc3501e_power_policy(&fw, NULL, NULL, 100u), ALP_ERR_INVAL, "NULL policy -> INVAL");
	zassert_equal(slave.cmd, 0u, "no transfer clocked");
}

/* ------------------------------------------------------------------ */
/* #733: layout of the directly-serialized (struct-punned) payloads.   */
/*                                                                     */
/* gpio_configure / gpio_write / gpio_set_interrupt / wifi_connect are */
/* NOT hand-packed -- the host hands &struct straight to the SPI DMA   */
/* and the firmware casts the wire buffer back to the struct type, so  */
/* the struct's byte image IS the wire frame.  The _Static_asserts in  */
/* protocol/cc3501e.h fail the build if padding ever creeps in; this   */
/* test documents the intended byte representation at runtime and      */
/* proves the host toolchain lays these out with no interior padding.  */
ZTEST(cc3501e_host_driver, test_punned_payload_layout_733)
{
	zassert_equal(sizeof(alp_cc3501e_gpio_configure_t), 4u, "gpio_configure = 4 wire bytes");
	zassert_equal(sizeof(alp_cc3501e_gpio_write_t), 4u, "gpio_write = 4 wire bytes");
	zassert_equal(
	    sizeof(alp_cc3501e_gpio_set_interrupt_t), 4u, "gpio_set_interrupt = 4 wire bytes");
	zassert_equal(sizeof(alp_cc3501e_wifi_connect_t), 4u, "wifi_connect header = 4 wire bytes");

	const alp_cc3501e_gpio_configure_t c = {
		.cc3501e_gpio = 13u, .direction = 1u, .pull = 2u, .reserved = 0u
	};
	const uint8_t *cb = (const uint8_t *)&c;
	zassert_equal(cb[0], 13u, "byte0 = cc3501e_gpio");
	zassert_equal(cb[1], 1u, "byte1 = direction");
	zassert_equal(cb[2], 2u, "byte2 = pull");

	const alp_cc3501e_wifi_connect_t w = {
		.ssid_len = 5u, .psk_len = 8u, .security = 1u, .reserved = 0u
	};
	const uint8_t *wb = (const uint8_t *)&w;
	zassert_equal(wb[0], 5u, "byte0 = ssid_len");
	zassert_equal(wb[1], 8u, "byte1 = psk_len");
	zassert_equal(wb[2], 1u, "byte2 = security");

	/* The issue's canonical trap: this struct's sizeof is 8, but the wire
	 * header is 7 -- which is exactly why cc3501e_ble_adv_start hand-packs
	 * it (see test_ble_adv_start_encodes_7byte_header) instead of memcpy. */
	zassert_equal(sizeof(alp_cc3501e_ble_adv_start_t), 8u, "ble_adv_start struct = 8, wire = 7");
}

/* ==================== SPI1 HOST PASSTHROUGH (0x55..0x57) =================== */

ZTEST(cc3501e_host_driver, test_spi1_transfer_before_configure_is_not_ready)
{
	/* SESSION gate (the collision this closes): a freshly cc3501e_init()'d ctx
	 * must refuse TRANSFER locally -- never touching the wire -- until a real
	 * CONFIGURE has succeeded in THIS session.  See spi1_configured's comment
	 * in include/alp/chips/cc3501e/core.h. */
	const uint8_t tx[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
	uint8_t       rx[4] = { 0 };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, rx, 4u, 0u, false, 100u),
	              ALP_ERR_NOT_READY,
	              "TRANSFER before any CONFIGURE in this session -> NOT_READY");
	zassert_equal(slave.cmd, 0u, "rejected locally, never clocked the bus");
}

ZTEST(cc3501e_host_driver, test_spi1_configure_encodes_request_and_decodes_reply)
{
	uint32_t actual_freq_hz = 0u;
	uint16_t max_xfer       = 0u;
	zassert_equal(cc3501e_spi1_configure(
	                  &fw, 10000000u, 0u, ALP_CC3501E_SPI1_CS0, &actual_freq_hz, &max_xfer, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_CONFIGURE, "opcode 0x55");
	zassert_equal(slave.req_len, 8u, "payload = alp_cc3501e_spi1_configure_t (8 B)");
	zassert_equal(slave.req_pl[0], 0x80u, "freq_hz byte0");
	zassert_equal(slave.req_pl[1], 0x96u, "freq_hz byte1");
	zassert_equal(slave.req_pl[2], 0x98u, "freq_hz byte2");
	zassert_equal(slave.req_pl[3], 0x00u, "freq_hz byte3 (10000000 = 0x00989680 LE)");
	zassert_equal(slave.req_pl[4], 0x00u, "mode");
	zassert_equal(slave.req_pl[5], 0x08u, "bits_per_word is pinned at 8, not a caller parameter");
	zassert_equal(slave.req_pl[6], (uint8_t)ALP_CC3501E_SPI1_CS0, "cs");
	zassert_equal(actual_freq_hz, 10000000u, "decoded actual SCK");
	zassert_equal(max_xfer, CC3501E_SPI1_MAX_XFER_V4, "decoded peer chunk cap");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_encodes_request_matches_protocol_vector)
{
	/* tests/protocol_vectors.txt (firmware repo): spi1_transfer_request =
	 * 56000C000400000100000000DEADBEEF -- header {56 00 0C 00}, payload
	 * {04 00 | 00 | 01 | 00 | 00 00 00 | DE AD BE EF}.  seq 1 is what the
	 * FIRST TRANSFER on a freshly cc3501e_init()'d ctx always carries. */
	zassert_equal(
	    cc3501e_spi1_configure(&fw, 10000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	    ALP_OK,
	    "CONFIGURE -> OK");

	const uint8_t tx[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
	uint8_t       rx[4] = { 0 };
	zassert_equal(
	    cc3501e_spi1_transfer(&fw, tx, rx, 4u, 0u, false, 100u), ALP_OK, "TRANSFER -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_TRANSFER, "opcode 0x56");
	zassert_equal(slave.req_len, 12u, "8-byte header + 4 inline TX bytes");
	const uint8_t want[12] = {
		0x04u, 0x00u,               /* len LE16 = 4              */
		0x00u,                      /* flags = 0 (single-shot)   */
		0x01u,                      /* seq = 1, first transfer   */
		0x00u,                      /* tx_fill (ignored, tx set) */
		0x00u, 0x00u, 0x00u,        /* reserved                  */
		0xDEu, 0xADu, 0xBEu, 0xEFu, /* tx, packed inline         */
	};
	zassert_mem_equal(
	    slave.req_pl, want, sizeof(want), "emitted bytes match spi1_transfer_request");
	zassert_mem_equal(rx, tx, sizeof(tx), "the loopback model echoes MOSI onto MISO");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_no_tx_and_no_rx_flags)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");

	/* NO_TX: tx == NULL clocks tx_fill instead -- the model loops the fill
	 * byte back on rx so a decode proves the fill was what actually clocked,
	 * not leftover buffer content. */
	uint8_t rx[3] = { 0 };
	zassert_equal(
	    cc3501e_spi1_transfer(&fw, NULL, rx, 3u, 0xA5u, false, 100u), ALP_OK, "NO_TX -> OK");
	zassert_equal(slave.req_pl[2], (uint8_t)ALP_CC3501E_SPI1_XFER_NO_TX, "flags = NO_TX only");
	zassert_equal(slave.req_len, 8u, "NO_TX carries no inline TX bytes");
	zassert_equal(rx[0], 0xA5u, "rx[0] = the fill byte");
	zassert_equal(rx[1], 0xA5u, "rx[1] = the fill byte");
	zassert_equal(rx[2], 0xA5u, "rx[2] = the fill byte");

	/* NO_RX: rx == NULL discards MISO -- ALP_OK, the caller's own buffer never
	 * touched, and the request still carries its inline TX bytes. */
	const uint8_t tx[2] = { 0x11u, 0x22u };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, NULL, 2u, 0u, false, 100u), ALP_OK, "NO_RX -> OK");
	zassert_equal(slave.req_pl[2], (uint8_t)ALP_CC3501E_SPI1_XFER_NO_RX, "flags = NO_RX only");
	zassert_equal(slave.req_len, 10u, "8-byte header + 2 inline TX bytes");
}

ZTEST(cc3501e_host_driver, test_spi1_transfer_seq_mismatch_reply_is_io)
{
	zassert_equal(cc3501e_spi1_configure(&fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, NULL, NULL, 100u),
	              ALP_OK,
	              "CONFIGURE -> OK");
	g_spi1_reply_bad_seq = true;

	/* A reply that echoes a DIFFERENT seq than this transfer's is the answer
	 * to some OTHER request (the firmware's cache, or a desynced read) --
	 * spi1_take_rx() must report the desync, not hand back those bytes. */
	const uint8_t tx[2] = { 0xAAu, 0xBBu };
	uint8_t       rx[2] = { 0 };
	zassert_equal(cc3501e_spi1_transfer(&fw, tx, rx, 2u, 0u, false, 100u),
	              ALP_ERR_IO,
	              "a reply echoing the wrong seq is a desync, not this transfer's answer");
}

ZTEST(cc3501e_host_driver, test_spi1_release_argless)
{
	zassert_equal(cc3501e_spi1_release(&fw, 100u), ALP_OK, "RELEASE -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_SPI1_RELEASE, "opcode 0x57");
	zassert_equal(slave.req_len, 0u, "RELEASE carries no request payload");
}

/* ---- link auto-recovery (issue #2126, + #2126 review) ---------------------
 *
 * cc3501e_link_check_and_recover() is wired into poll_by_repeat_seq()'s own
 * terminal returns (cc3501e_core.c's cc3501e_poll_exit()) -- every
 * worker-routed wrapper this suite exercises through poll_by_repeat() goes
 * through it, so cc3501e_wifi_get_mac() below stands in for the whole class.
 * g_all_io_down (see its own doc comment above) models a genuinely dead
 * link; fw.reset_pin/fw.enable_pin must be populated for cc3501e_recover()
 * to actually pulse anything instead of reporting ALP_ERR_NOSUPPORT.
 * g_heal_disabled / g_ble_enable_busy / g_deaf_from_ms+g_deaf_until_ms /
 * g_reset_release_count (see their own comments above) cover the review's
 * follow-up findings. There is no Kconfig-off test in this file any more --
 * see CMakeLists.txt / testcase.yaml's own comments for why that needs a
 * real Kconfig-driven build this hermetic suite cannot host, and where the
 * real proof lives instead. */

ZTEST(cc3501e_host_driver, test_link_transient_single_op_failure_no_recovery_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;
	/* WIFI_STATUS alone is wedged -- every OTHER opcode, including the
	 * probe's own PING, answers normally.  This is the "transient" case:
	 * one op's own retry budget genuinely exhausts (a real ALP_ERR_TIMEOUT,
	 * same shape as the dead-link test below), but the link as a whole is
	 * fine, which the probe discovers on its very first PING. */
	g_status_io_down_remaining = UINT32_MAX;

	alp_cc3501e_wifi_status_t st;
	alp_status_t              s = cc3501e_wifi_status(&fw, &st);

	zassert_equal(s, ALP_ERR_TIMEOUT, "the wedged op itself still fails");
	zassert_equal(fw.recover_count,
	              0u,
	              "a single op's own transient failure must not warm-reset a link the probe's "
	              "own PING (a different, unaffected opcode) finds fine");
}

ZTEST(cc3501e_host_driver, test_link_ota_open_suppresses_recovery_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	alp_status_t enter_s = cc3501e_ota_update_mode(&fw, true, 1000u);

	g_all_io_down                     = true;
	uint8_t      mac[CC3501E_MAC_LEN] = { 0 };
	alp_status_t op_s                 = cc3501e_wifi_get_mac(&fw, mac, 100u);
	uint32_t     recovers_seen        = fw.recover_count;

	/* Restore state BEFORE asserting, not after: cc3501e_peer_is_polled()
	 * is a file-static in cc3501e_core.c, not reset by slave_reset()
	 * between tests, so a failed assertion here must not leave every LATER
	 * test in this binary silently running against a "polled peer". */
	g_all_io_down        = false;
	alp_status_t leave_s = cc3501e_ota_update_mode(&fw, false, 1000u);

	zassert_equal(leave_s, ALP_OK, "leave update mode (cleanup)");
	zassert_equal(enter_s, ALP_OK, "enter update mode (fake echoes the mode byte instantly)");
	zassert_equal(op_s, ALP_ERR_TIMEOUT, "the op itself still fails");
	zassert_equal(recovers_seen, 0u, "an open OTA/update session must suppress recovery entirely");
}

/* #2126 review (MAJOR: OTA guard was a transport flag, not a session
 * marker): cc3501e_recover() itself now refuses outright while
 * ctx->ota_session_active is set -- checked directly, independent of
 * whichever caller (auto or `alp companion recover`) reaches it. */
ZTEST(cc3501e_host_driver, test_recover_refuses_active_ota_session_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	fw.ota_session_active = true;
	g_reset_release_count = 0u;

	alp_status_t s = cc3501e_recover(&fw);

	fw.ota_session_active = false; /* restore before asserting -- see the OTA test's note above */
	zassert_equal(s, ALP_ERR_BUSY, "cc3501e_recover() must refuse outright with a session active");
	zassert_equal(g_reset_release_count, 0u, "no nRESET pulse while a session is active");
}

/* #2126 review (MAJOR: concurrency during recovery): the CAS on
 * ctx->recovering rejects a concurrent caller immediately, with NO reset
 * attempted -- simulated here without real threads by setting the flag by
 * hand, the same state a genuinely concurrent second caller would observe. */
ZTEST(cc3501e_host_driver, test_recover_cas_rejects_concurrent_caller_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	fw.recovering         = true; /* another "thread" already mid-recovery */
	g_reset_release_count = 0u;

	alp_status_t s = cc3501e_recover(&fw);

	fw.recovering = false;
	zassert_equal(s, ALP_ERR_BUSY, "a concurrent recovery attempt must be rejected immediately");
	zassert_equal(g_reset_release_count, 0u, "the loser must not pulse nRESET at all");
}

ZTEST(cc3501e_host_driver, test_link_dead_triggers_exactly_one_recovery_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;
	g_all_io_down = true;

	uint8_t      mac[CC3501E_MAC_LEN] = { 0 };
	alp_status_t s                    = cc3501e_wifi_get_mac(&fw, mac, 100u);

	zassert_equal(s, ALP_ERR_TIMEOUT, "a dead link still fails the op that discovered it");
	zassert_equal(fw.recover_count, 1u, "exactly one warm-reset recovery ran");

	/* And the recovery actually worked: alp_gpio_write()'s fake heals
	 * g_all_io_down the moment cc3501e_hard_reset() releases reset_pin, so
	 * cc3501e_link_check_and_recover()'s own confirming PING (inside
	 * cc3501e_recover()) already proved the link answers again before this
	 * function even returned -- a fresh op now succeeds with NO second
	 * recovery. */
	zassert_equal(cc3501e_ping(&fw), ALP_OK, "link answers again after the warm reset");
	zassert_equal(fw.recover_count, 1u, "the follow-up ping did not trigger a second recovery");
}

/* cc3501e_wifi_connect() does not go through poll_by_repeat() -- it drives
 * its own WIFI_STATUS poll loop (see cc3501e_wifi.c) -- so it gets its OWN
 * trigger at its own timeout exit, guarded by "did ANY status read ever
 * land" rather than ctx->rx_scratch[0] (a single non-retried
 * cc3501e_wifi_status_once() call per iteration means rx_scratch[0] only
 * ever reflects the LAST iteration by the time this function returns). This
 * proves that path independently of the poll_by_repeat_seq()-based tests
 * above. */
ZTEST(cc3501e_host_driver, test_wifi_connect_dead_link_triggers_recovery_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;
	g_all_io_down = true;

	alp_status_t s = cc3501e_wifi_connect(&fw, "deadlinknet", 1u, "pw", 120u);

	zassert_equal(s, ALP_ERR_TIMEOUT, "connect itself still times out");
	zassert_equal(fw.recover_count,
	              1u,
	              "cc3501e_wifi_connect()'s own timeout exit must trigger recovery when NO "
	              "WIFI_STATUS read ever succeeded across the whole attempt");
}

/* #2126 review (MAJOR: false trigger on a busy bridge, poll_by_repeat_seq
 * half): kills the surviving "(no_status || !no_status)" mutation in
 * cc3501e_poll_exit() -- a bridge that answers a genuine DECODED status
 * (BUSY, here) on EVERY attempt, never silent even once, must not be
 * warm-reset just because the op itself times out waiting for the worker to
 * actually finish. */
ZTEST(cc3501e_host_driver, test_poll_by_repeat_busy_persists_no_recovery_2126)
{
	fw.reset_pin      = FAKE_RESET_PIN;
	fw.enable_pin     = FAKE_ENABLE_PIN;
	g_ble_enable_busy = true; /* every CMD_BLE_ENABLE attempt decodes BUSY */
	/* The probe's own PING must ALSO fail outright -- if the trigger fires
	 * incorrectly (the mutation this test exists to kill) and the probe
	 * instead found a healthy link, cc3501e_recover() would never even be
	 * attempted and recover_attempt_count would stay 0 EITHER way,
	 * defeating this test's whole point (BLE_ENABLE is the only opcode
	 * forced busy; PING is a completely different, otherwise-unaffected
	 * opcode). Forcing PING to fail too closes that gap: an incorrectly
	 * fired trigger now has nowhere to go but all the way to a real
	 * (failing) recovery attempt. */
	g_ping_always_fails = true;

	alp_status_t s = cc3501e_ble_enable(&fw, 500u);

	g_ble_enable_busy   = false;
	g_ping_always_fails = false;
	zassert_equal(s, ALP_ERR_TIMEOUT, "still times out -- the worker never actually finishes");
	/* recover_attempt_count, NOT recover_count -- see the identical note on
	 * test_wifi_connect_any_status_ok_survives_late_silence_2126: with PING
	 * forced to fail too, even an INCORRECTLY triggered recovery attempt
	 * cannot succeed, so recover_count alone cannot distinguish "never
	 * attempted" from "attempted and failed". */
	zassert_equal(fw.recover_attempt_count,
	              0u,
	              "a bridge that decoded BUSY on every single attempt must not even ATTEMPT a "
	              "recovery");
	zassert_equal(cmd_log_index_of(ALP_CC3501E_CMD_PING),
	              slave.cmd_log_count,
	              "no probe PING was ever sent either -- the trigger must not even fire");
}

/* #2126 review (MAJOR: false trigger on a busy bridge, cc3501e_wifi_connect
 * half): kills the surviving "any_status_ok = true;" removal mutation in
 * cc3501e_wifi_connect(). At least one WIFI_STATUS read lands (CONNECTING)
 * before the link goes silent for the rest of the budget -- that one
 * successful read is proof enough the link is alive, so the eventual
 * timeout must not trigger a warm reset. */
ZTEST(cc3501e_host_driver, test_wifi_connect_any_status_ok_survives_late_silence_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	slave.wifi_conn_state = ALP_CC3501E_WIFI_CONNECTING; /* never resolves -- keeps polling */

	uint64_t t0     = alp_uptime_ms();
	g_deaf_from_ms  = t0 + 100u;    /* a status read or two land first */
	g_deaf_until_ms = t0 + 100000u; /* then silent well past connect's own budget */

	alp_status_t s = cc3501e_wifi_connect(&fw, "flakynet", 1u, "pw", 500u);

	g_deaf_from_ms = g_deaf_until_ms = 0u;
	zassert_equal(s, ALP_ERR_TIMEOUT, "connect times out -- CONNECTING never resolves");
	/* recover_attempt_count, NOT recover_count: the deaf window is still
	 * open past connect's own budget (deliberately -- it must outlast the
	 * probe too, or a healthy-looking probe could mask this exact
	 * mutation), so an INCORRECTLY triggered recovery attempt would also
	 * fail its own confirming PING and never reach recover_count -- the
	 * trigger firing AT ALL is what this test must catch, not just whether
	 * that attempt happened to succeed. */
	zassert_equal(fw.recover_attempt_count,
	              0u,
	              "at least one WIFI_STATUS read succeeded before the link went silent -- must "
	              "not even ATTEMPT a warm-reset");
}

/* #2126 review (MAJOR: cooldown based on successes, not attempts): the
 * reviewer's own reproducer. A wedge a warm reset does NOT cure
 * (g_heal_disabled) still recovers (attempts) exactly ONCE across 4
 * back-to-back failing ops -- the OLD gate (`recover_count > 0`) let every
 * one of the 4 re-trigger a fresh nRESET pulse, because a FAILED attempt
 * never advanced recover_count. */
ZTEST(cc3501e_host_driver, test_link_failed_recovery_still_gets_cooldown_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	g_all_io_down         = true;
	g_heal_disabled       = true; /* warm reset does NOT cure this wedge */
	g_reset_release_count = 0u;

	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	for (int i = 0; i < 4; i++) {
		(void)cc3501e_wifi_get_mac(&fw, mac, 100u);
	}

	g_heal_disabled = false;
	g_all_io_down   = false;
	zassert_equal(g_reset_release_count,
	              1u,
	              "4 back-to-back failing ops must still produce exactly 1 nRESET pulse (%u seen)",
	              g_reset_release_count);
	zassert_equal(fw.recover_attempt_count, 1u, "and exactly 1 recovery ATTEMPT, not 4");
	zassert_equal(fw.recover_count, 0u, "none of them succeeded -- the wedge never cured");
	zassert_true(fw.recover_fail_streak >= 1u, "the failure streak must have advanced");
}

ZTEST(cc3501e_host_driver, test_link_cooldown_suppresses_second_recovery_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;
	g_all_io_down = true;

	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	zassert_equal(cc3501e_wifi_get_mac(&fw, mac, 100u), ALP_ERR_TIMEOUT, "first op fails");
	zassert_equal(fw.recover_count, 1u, "first failure recovers once");

	/* Wedge the link again immediately -- CC3501E_RECOVER_COOLDOWN_MS
	 * (30 s, no back-off yet since the first attempt SUCCEEDED and reset
	 * recover_fail_streak to 0) has not elapsed on the fake clock (the
	 * first call only advanced it by the poll-exhaustion + probe +
	 * warm-reset delays, nowhere near 30 s -- see the file-level comment
	 * on alp_delay_ms()/alp_uptime_ms() sharing one fake counter). The
	 * cooldown must decline a second warm reset even though a fresh probe
	 * would otherwise see a dead link again. */
	g_all_io_down = true;
	zassert_equal(cc3501e_wifi_get_mac(&fw, mac, 100u), ALP_ERR_TIMEOUT, "second op also fails");
	zassert_equal(fw.recover_count, 1u, "cooldown suppressed the second recovery");
	zassert_equal(fw.recover_attempt_count, 1u, "and did not even ATTEMPT a second time");

	g_all_io_down = false; /* leave the fixture clean for the next test */
}

/* #2126 review (MAJOR: stale handles after reboot). link_epoch is bumped by
 * a successful cc3501e_recover(); a socket handle minted BEFORE that bump
 * must be refused (not silently addressed to whatever new socket now
 * happens to share its low byte), while a handle minted AFTER works
 * normally. */
ZTEST(cc3501e_host_driver, test_socket_handle_epoch_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	uint16_t old_handle = 0u;
	zassert_equal(cc3501e_sock_open(&fw,
	                                (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                (uint8_t)ALP_CC3501E_SOCK_TYPE_STREAM,
	                                0u,
	                                &old_handle,
	                                1000u),
	              ALP_OK,
	              "open a socket before any recovery");

	/* Force a real, successful recovery -- bumps fw.link_epoch. */
	g_all_io_down                = true;
	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	(void)cc3501e_wifi_get_mac(&fw, mac, 100u);
	zassert_equal(fw.recover_count, 1u, "test setup: the recovery must have actually run");

	uint8_t      rx[8] = { 0 };
	size_t       got   = 0;
	alp_status_t s     = cc3501e_sock_recv(&fw, old_handle, rx, sizeof(rx), &got, 100u);
	zassert_equal(
	    s, ALP_ERR_NOT_READY, "a handle from before the recovery must be refused, not reused");

	uint16_t new_handle = 0u;
	zassert_equal(cc3501e_sock_open(&fw,
	                                (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                (uint8_t)ALP_CC3501E_SOCK_TYPE_STREAM,
	                                0u,
	                                &new_handle,
	                                1000u),
	              ALP_OK,
	              "open a socket AFTER the recovery");
	zassert_equal(cc3501e_sock_close(&fw, new_handle, 1000u),
	              ALP_OK,
	              "a handle minted after the recovery works normally, not refused");
}

/* #2126 review (MAJOR: the concurrency fix was not mutation-proven). A
 * mutant that never clears ctx->recovering on cc3501e_recover()'s FINAL exit
 * left the whole suite green: every LATER recovery answered ALP_ERR_BUSY
 * forever, and no test ever ran a second one. Two back-to-back recoveries
 * here pin that -- the second must pulse nRESET again, not lose the CAS to
 * the first one's leftover flag. */
ZTEST(cc3501e_host_driver, test_recover_twice_each_pulses_reset_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	g_reset_release_count = 0u;

	zassert_equal(cc3501e_recover(&fw), ALP_OK, "first manual recovery");
	zassert_false(fw.recovering, "the CAS flag must be clear once a recovery has committed");
	zassert_equal(g_reset_release_count, 1u, "first recovery pulsed nRESET once");

	zassert_equal(cc3501e_recover(&fw), ALP_OK, "a second recovery must not lose the CAS");
	zassert_false(fw.recovering, "and must leave the flag clear again");
	zassert_equal(g_reset_release_count, 2u, "the second recovery pulsed nRESET too");
	zassert_equal(fw.recover_count, 2u, "both recoveries counted");
}

/* #2126 review (MAJOR, same gap, other exit): a mutant that skips the
 * ctx->recovering clear on the lock-acquire failure path also left the suite
 * green. Hold the lock by hand -- the same state a genuinely concurrent
 * request leaves -- and require the flag clear after the refusal, or the very
 * next recovery attempt is rejected by a flag nobody owns. */
ZTEST(cc3501e_host_driver, test_recover_lock_busy_still_clears_cas_flag_2126)
{
	fw.reset_pin          = FAKE_RESET_PIN;
	fw.enable_pin         = FAKE_ENABLE_PIN;
	g_reset_release_count = 0u;
	fw.request_lock       = true; /* somebody else owns the wire */

	alp_status_t s  = cc3501e_recover(&fw);
	fw.request_lock = false;

	zassert_not_equal(s, ALP_OK, "recovery cannot run while another caller holds the lock");
	zassert_false(fw.recovering, "and must not leave the CAS flag stuck true");
	zassert_equal(g_reset_release_count, 0u, "no nRESET pulse without the lock");

	/* The flag really is free: a recovery straight afterwards runs. */
	zassert_equal(cc3501e_recover(&fw), ALP_OK, "the next recovery attempt is not locked out");
	zassert_equal(g_reset_release_count, 1u, "and it pulsed nRESET");
}

/* #2126 review (MAJOR: only cc3501e_sock_recv() was covered -- a mutant that
 * dropped cc3501e_sock_send()'s own epoch check survived). Every socket op
 * that takes a handle must refuse one minted before the last recovery. */
ZTEST(cc3501e_host_driver, test_stale_handle_refused_by_every_socket_op_2126)
{
	fw.reset_pin  = FAKE_RESET_PIN;
	fw.enable_pin = FAKE_ENABLE_PIN;

	uint16_t stale = 0u;
	zassert_equal(cc3501e_sock_open(&fw,
	                                (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                (uint8_t)ALP_CC3501E_SOCK_TYPE_STREAM,
	                                0u,
	                                &stale,
	                                1000u),
	              ALP_OK,
	              "open a socket before the recovery");

	g_all_io_down                = true;
	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	(void)cc3501e_wifi_get_mac(&fw, mac, 100u);
	zassert_equal(fw.recover_count, 1u, "test setup: the recovery must have actually run");

	const uint8_t ip[4]   = { 192u, 168u, 1u, 10u };
	const uint8_t data[2] = { 0xAAu, 0xBBu };
	size_t        sent    = 0u;

	zassert_equal(cc3501e_sock_send(&fw, stale, data, sizeof(data), &sent, 100u),
	              ALP_ERR_NOT_READY,
	              "send must refuse a stale handle");
	zassert_equal(cc3501e_sock_connect(&fw, stale, ip, 80u, 100u),
	              ALP_ERR_NOT_READY,
	              "connect must refuse a stale handle");
	zassert_equal(cc3501e_sock_bind(&fw, stale, ip, 80u, 100u),
	              ALP_ERR_NOT_READY,
	              "bind must refuse a stale handle");
	zassert_equal(cc3501e_sock_listen(&fw, stale, 4u, 100u),
	              ALP_ERR_NOT_READY,
	              "listen must refuse a stale handle");
	zassert_equal(cc3501e_sock_close(&fw, stale, 100u),
	              ALP_ERR_NOT_READY,
	              "close must refuse a stale handle");

	/* An accepted-socket event decoded at the CURRENT epoch hands back a
	 * handle carrying that epoch -- the same bytes decoded at a stale one
	 * would hand back a handle every op above refuses. */
	uint8_t                         payload[sizeof(alp_cc3501e_sock_accepted_evt_t)] = { 0 };
	alp_cc3501e_sock_accepted_evt_t ev                                               = { 0 };
	payload[2]                                                                       = 0x02u;
	zassert_equal(cc3501e_sock_accepted_decode(payload, sizeof(payload), fw.link_epoch, &ev),
	              ALP_OK,
	              "decode at the current epoch");
	zassert_equal((uint8_t)(ev.handle >> 8),
	              fw.link_epoch,
	              "the decoded handle carries the epoch it was decoded at");
}

/* #2126 review (MINOR: cc3501e_set_recover_callback() had no test -- the
 * callback was never shown to fire at all). */
static uint32_t g_recover_cb_calls;
static uint32_t g_recover_cb_last_count;

static void test_recover_cb(cc3501e_t *ctx, uint32_t recover_count, void *user)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(user);
	g_recover_cb_calls++;
	g_recover_cb_last_count = recover_count;
}

ZTEST(cc3501e_host_driver, test_recover_callback_fires_once_per_recovery_2126)
{
	fw.reset_pin            = FAKE_RESET_PIN;
	fw.enable_pin           = FAKE_ENABLE_PIN;
	g_recover_cb_calls      = 0u;
	g_recover_cb_last_count = 0u;
	cc3501e_set_recover_callback(&fw, test_recover_cb, NULL);

	g_all_io_down                = true;
	uint8_t mac[CC3501E_MAC_LEN] = { 0 };
	(void)cc3501e_wifi_get_mac(&fw, mac, 100u);

	cc3501e_set_recover_callback(&fw, NULL, NULL);
	zassert_equal(fw.recover_count, 1u, "test setup: exactly one recovery ran");
	zassert_equal(g_recover_cb_calls, 1u, "the registered callback fired exactly once");
	zassert_equal(g_recover_cb_last_count, 1u, "and was handed the committed recover_count");
}

/* ========================= LINK-FAILURE RING (#2136) ======================= */

ZTEST(cc3501e_host_driver, test_link_log_records_pre_decode_failure_2136)
{
	g_all_io_down = true;
	zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, "io-down ping fails pre-decode");
	zassert_equal(cc3501e_link_log_count(&fw), 1u, "one ring entry recorded");

	cc3501e_link_log_entry_t e;
	zassert_equal(cc3501e_link_log_get(&fw, 0u, &e), ALP_OK, "entry 0 reads back");
	zassert_equal(e.cmd, (uint8_t)ALP_CC3501E_CMD_PING, "records the opcode used");
	zassert_equal(
	    e.phase, CC3501E_LINK_LOG_PHASE_REQUEST_HEADER, "bails at the request-header phase");
	zassert_equal(e.status, (int8_t)ALP_ERR_IO, "records the mapped status");

	/* A decoded reply -- success here -- must add NOTHING to the ring. */
	g_all_io_down = false;
	zassert_equal(cc3501e_ping(&fw), ALP_OK, "healthy ping succeeds");
	zassert_equal(cc3501e_link_log_count(&fw), 1u, "success adds no ring entry");
}

ZTEST(cc3501e_host_driver, test_link_log_fail_streak_resets_on_success_2136)
{
	g_all_io_down = true;
	zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, NULL);
	zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, NULL);
	zassert_equal(fw.link_log_fail_streak, 2u, "two consecutive pre-decode failures");

	g_all_io_down = false;
	zassert_equal(cc3501e_ping(&fw), ALP_OK, NULL);
	zassert_equal(fw.link_log_fail_streak, 0u, "a decoded reply resets the streak");
}

ZTEST(cc3501e_host_driver, test_link_log_wraps_keeps_newest_2136)
{
	/* g_fake_now_ms is a suite-wide fake clock, not reset per test (see its
	 * own doc comment) -- capture a baseline like the other tests that read
	 * it directly, rather than assuming it starts at 0. Real delays inside
	 * cc3501e_request_locked() only ever call alp_delay_us() on this NULL-
	 * ready_pin fixture, which does NOT advance g_fake_now_ms, so this test
	 * advances the fake clock itself between attempts -- the only way to
	 * give each ring entry a distinct, orderable ts_ms without a real
	 * sleep. */
	const uint64_t base     = g_fake_now_ms;
	const unsigned attempts = (unsigned)CC3501E_LINK_LOG_LEN + 3u;

	g_all_io_down = true;
	for (unsigned i = 1u; i <= attempts; i++) {
		g_fake_now_ms = base + (uint64_t)i * 1000u;
		zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, NULL);
	}
	zassert_equal(cc3501e_link_log_count(&fw), CC3501E_LINK_LOG_LEN, "ring saturates at LEN");

	cc3501e_link_log_entry_t oldest, newest;
	zassert_equal(cc3501e_link_log_get(&fw, 0u, &oldest), ALP_OK, NULL);
	zassert_equal(
	    cc3501e_link_log_get(&fw, (uint8_t)(CC3501E_LINK_LOG_LEN - 1u), &newest), ALP_OK, NULL);
	/* attempts - LEN attempts were evicted, so the oldest SURVIVING entry is
	 * attempt #(attempts - LEN + 1), and the newest is the very last one. */
	const uint32_t expect_oldest_ts =
	    (uint32_t)(base + (uint64_t)(attempts - CC3501E_LINK_LOG_LEN + 1u) * 1000u);
	const uint32_t expect_newest_ts = (uint32_t)(base + (uint64_t)attempts * 1000u);

	zassert_equal(oldest.ts_ms, expect_oldest_ts, "oldest entries were evicted, not kept");
	zassert_equal(newest.ts_ms, expect_newest_ts, "the newest attempt is always kept");
}

ZTEST(cc3501e_host_driver, test_link_log_distinguishes_deaf_armed_from_desynced_2136)
{
	/* Desynced: the request-header phase itself reads the wrong marker
	 * (0x00, not the armed idle pattern) -- the in-band armed check bails
	 * at the request-header phase before anything else is clocked. */
	g_req_hdr_desynced = true;
	zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, "desynced header fails the armed check");
	cc3501e_link_log_entry_t desynced;
	zassert_equal(cc3501e_link_log_get(&fw, 0u, &desynced), ALP_OK, NULL);
	zassert_equal(desynced.phase,
	              CC3501E_LINK_LOG_PHASE_REQUEST_HEADER,
	              "desynced bails at the request-header phase");
	zassert_equal(desynced.hdr_bytes[0], 0x00u, "desynced header reads the wrong marker");
	zassert_equal(desynced.hdr_bytes[3], 0x00u, NULL);
	g_req_hdr_desynced = false;
	/* The desynced attempt above left the FAKE SLAVE MODEL itself out of
	 * lockstep with the host (the host bailed at the armed check without
	 * ever clocking a reply phase, but the stub's own slave.phase already
	 * advanced past PH_REQ_HDR inside slave_dispatch()) -- reset just the
	 * model, not ctx->link_log, so the next attempt starts this SAME ctx's
	 * ring a clean 4-phase exchange instead of inheriting that skew. */
	slave_reset();

	/* Deaf-armed: the request-header phase is normal (armed, 0xA5 x4), but
	 * the reply header never echoes the request opcode -- the slave armed
	 * the link and then never dispatched anything. */
	g_reply_hdr_deaf = true;
	zassert_equal(cc3501e_ping(&fw), ALP_ERR_IO, "a reply that never echoes is IO");
	cc3501e_link_log_entry_t deaf;
	zassert_equal(cc3501e_link_log_get(&fw, 1u, &deaf), ALP_OK, NULL);
	zassert_equal(
	    deaf.phase, CC3501E_LINK_LOG_PHASE_REPLY_HEADER, "deaf-armed bails at the reply header");
	zassert_equal(deaf.hdr_bytes[0], ALP_CC3501E_SYNC_IDLE, "request header WAS armed (0xA5)");
	zassert_equal(deaf.reply_hdr[0], ALP_CC3501E_SYNC_IDLE, "reply header never echoed the cmd");
	g_reply_hdr_deaf = false;

	/* The two entries must actually be tellable apart -- the whole point. */
	zassert_true(desynced.phase != deaf.phase, "different phase");
	zassert_true(desynced.hdr_bytes[0] != deaf.hdr_bytes[0], "different request-header bytes");
}

ZTEST_SUITE(cc3501e_host_driver, NULL, NULL, reset_before, NULL, NULL);

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private declarations shared across the cc3501e_*.c translation units
 * (the subsystem split of the CC3501E host driver).  Not installed, not
 * part of the public API -- the headers under alp/chips/cc3501e/ are the
 * public surface.
 */

#ifndef CC3501E_INTERNAL_H
#define CC3501E_INTERNAL_H

#include "alp/chips/cc3501e.h"

/* Per-attempt reply-wait budget passed to cc3501e_request() -- both for ops
 * that issue a single request directly, and as the per-attempt budget inside
 * poll_by_repeat().  Shared across every cc3501e_<subsystem>.c file. */
#define CC3501E_REQ_TMO_MS 100u

/* Re-issue one request while the firmware is unavailable, until it resolves
 * (OK / hard error) or the budget elapses.  Two retryable conditions:
 *
 *   - ALP_ERR_BUSY : the firmware worker is still running the op (poll-by-repeat
 *     -- the host re-issues to collect the cached result once it lands).
 *   - ALP_ERR_IO   : the bridge link was DOWN for this transaction.  On this
 *     no-host-IRQ rev the CC35 cannot service the inter-chip SPI slave WHILE it
 *     runs a radio op (Wlan_Start at boot, or the worker's Wlan_* body), so a
 *     request that overlaps the op reads back desynced (cc3501e_request returns
 *     IO at the reply-header sanity check).  The firmware re-syncs the slave at
 *     a clean boundary right after the op, so a retry lands cleanly -- treat IO
 *     as transient here and keep polling for the whole budget.
 *
 * EXCEPTION 1: ALP_CC3501E_RESP_ERR_STATE (a deterministic firmware reject,
 * e.g. BLE_GATT_REGISTER's NimBLE ble_gatts_mutable() ordering guard) also
 * maps to ALP_ERR_BUSY, but is NOT retried -- it will not resolve without the
 * caller changing state (stop advertising / disconnect), so retrying it would
 * just burn the whole budget on the same answer.  See the ctx->rx_scratch[0]
 * peek in the implementation.
 *
 * EXCEPTION 2 (#2035): a DECODED ALP_CC3501E_RESP_ERR_RADIO,
 * ALP_CC3501E_RESP_ERR_PROTOCOL, or ALP_CC3501E_RESP_ERR_INTERNAL reply also
 * maps to ALP_ERR_IO, but is likewise NOT retried -- the firmware's per-seq
 * retry latch (proto v8) answers a repeat of this exact request from its
 * latch without re-executing the op, so a repeat can only ever replay the
 * SAME decoded failure. Retrying it to budget would burn the whole poll
 * window and report ALP_ERR_TIMEOUT, the wrong error class. Disambiguated
 * from a genuine transient transport ALP_ERR_IO (bad header, failed
 * transceive) by the same ctx->rx_scratch[0] peek mechanism -- see
 * ALP_CC3501E_RX_SCRATCH_NO_STATUS (<alp/chips/cc3501e/core.h>).
 *
 * EXCEPTION 3 (alp-sdk#2035 review follow-up): cc3501e_lock_acquire()'s OWN
 * timeout is NOT one of the two retryable conditions above -- it means no
 * request even got a chance to reach the bridge, and only on the FIRST
 * attempt: it is returned immediately (unambiguous ALP_ERR_BUSY, nothing
 * sent). A lock timeout on a LATER attempt is different -- an EARLIER
 * attempt already reached the bridge (this loop only revisits the lock
 * after a retryable BUSY/IO from that attempt) -- so it is folded into the
 * SAME retry-within-budget treatment as BUSY/IO above instead, and can
 * still end in ALP_ERR_TIMEOUT if the whole budget elapses contended.
 *
 * Returns the final cc3501e_request status; ALP_ERR_TIMEOUT if it never
 * resolved within the budget.  The caller's budget must therefore cover the
 * longest down-window (Wlan_Start/op, seconds) -- see cc3501e_wifi_get_mac.
 * Implemented in cc3501e_core.c (beside cc3501e_request, which it wraps). */
alp_status_t poll_by_repeat(cc3501e_t        *ctx,
                            alp_cc3501e_cmd_t cmd,
                            const uint8_t    *tx_payload,
                            size_t            tx_len,
                            uint8_t          *rx_buf,
                            size_t            rx_cap,
                            size_t           *rx_len,
                            uint32_t          timeout_ms);

/* Same retry loop as poll_by_repeat() above, but the caller supplies the
 * retry seq instead of it being allocated from the shared ctx->req_seq
 * (alp-sdk#2108) -- SOCK_RECV uses this with ctx->sock_recv_seq so its seq
 * space can never collide with any other opcode's. See ctx->sock_recv_seq's
 * comment in <alp/chips/cc3501e/core.h> and cc3501e_sock_recv()'s assignment
 * site in cc3501e_sockets.c. Implemented in cc3501e_core.c, beside
 * poll_by_repeat(), which now wraps it. */
/* #2126 review: @p check_epoch / @p handle add a PER-ATTEMPT re-check of
 * ctx->link_epoch, not just the one-shot check every cc3501e_sock_*() caller
 * already does before entering this loop. A retry loop can span a
 * recovery: cc3501e_lock_acquire() below blocks for the duration of a
 * concurrent cc3501e_recover() (same ctx->request_lock), so an attempt that
 * was already mid-retry when a recovery landed resumes holding a handle
 * whose epoch no longer matches -- sending it anyway would address whatever
 * NEW socket now happens to share its low byte on the rebooted firmware.
 * Checked under the SAME lock hold as the request itself, so there is no
 * window between "epoch looked fine" and "the send went out" for a
 * concurrent recovery to land in. Non-socket callers pass check_epoch =
 * false (handle then unused) and see no behaviour change -- this is
 * poll_by_repeat_handle()'s job, not a new burden on poll_by_repeat()'s
 * existing 50+ non-socket call sites. */
alp_status_t poll_by_repeat_seq(cc3501e_t        *ctx,
                                alp_cc3501e_cmd_t cmd,
                                const uint8_t    *tx_payload,
                                size_t            tx_len,
                                uint8_t          *rx_buf,
                                size_t            rx_cap,
                                size_t           *rx_len,
                                uint32_t          timeout_ms,
                                uint8_t           req_seq,
                                bool              check_epoch,
                                uint16_t          handle);

/* Same retry loop as poll_by_repeat() above (own ctx->req_seq allocation),
 * but with the per-attempt epoch re-check (see poll_by_repeat_seq()'s
 * comment) enabled against @p handle -- the socket ops that carry a
 * caller-epoch-encoded handle (connect/bind/listen/send/close) use this
 * instead of poll_by_repeat(). Returns ALP_ERR_NOT_READY, without ever
 * sending, the moment @p handle's epoch stops matching ctx->link_epoch.
 * Implemented in cc3501e_core.c beside poll_by_repeat(). */
alp_status_t poll_by_repeat_handle(cc3501e_t        *ctx,
                                   alp_cc3501e_cmd_t cmd,
                                   const uint8_t    *tx_payload,
                                   size_t            tx_len,
                                   uint8_t          *rx_buf,
                                   size_t            rx_cap,
                                   size_t           *rx_len,
                                   uint32_t          timeout_ms,
                                   uint16_t          handle);

/* Tell the transport the peer is a POLLED slave (OTA update mode).
 * cc3501e_ota_begin() reads this back via cc3501e_peer_is_polled() as a hard
 * precondition -- see that function's own comment for why BEGIN on the
 * ordinary callback/DMA bridge permanently wedges the device.  See
 * g_peer_polled's comment in cc3501e_core.c for why this no longer also
 * floors cc3501e_reply_gate()'s fallback settle. */
void cc3501e_set_peer_polled(bool on);

/* True when the host believes the peer is running the POLLED update-mode boot. */
bool cc3501e_peer_is_polled(void);

/* True once cc3501e_reply_gate() has ever given up waiting on a stuck-LOW
 * ready_pin (CC3501E_READY_STUCK_LOW_STREAK consecutive full-budget
 * timeouts) and latched g_ready_ignored, process-wide.  Production code never
 * resets this once it latches.  NOT a log call -- chips/cc3501e has no
 * logging facility of its own -- a caller (bench diagnostics, a bring-up
 * app) has to poll this and print it itself if it wants the degrade
 * reported anywhere.  See g_ready_line_was_stuck in cc3501e_core.c for the
 * full rationale. */
bool cc3501e_ready_line_was_stuck(void);

#ifdef CONFIG_ZTEST
/* TEST-ONLY: reset every cc3501e_reply_gate() READY-gate static (the
 * stuck-LOW ignore latch, its timeout streak, the was-stuck latch) to its
 * zero state.  Compiled only under CONFIG_ZTEST -- never linked into a
 * non-test build -- because production code never calls this: those statics
 * are meant to persist for the whole boot.  tests/zephyr/cc3501e_host_driver
 * drives multiple READY-line fixtures (stuck-high, stuck-low, noisy, a slow
 * arm) through the SAME test binary, and file-static state would otherwise
 * leak from one fixture into the next depending on run order. */
void cc3501e_ready_gate_reset_for_test(void);
#endif

/* #2035: whether opcode @p cmd is allowed to reply all-zero (status byte +
 * data) without cc3501e_request_locked() treating that as the #1378
 * dead-phase alias.  Not `static` -- and declared here, not just in
 * cc3501e_core.c -- purely so tests/zephyr/chips/src/test_cc3501e.c can
 * exercise the classification directly (see cc3501e_core.c for the full
 * rationale and the per-opcode justifications). */
bool cc3501e_reply_may_be_all_zero(alp_cc3501e_cmd_t cmd);

/* v4.0 (#2035): pure version-gate decision cc3501e_reset() defers to -- see
 * cc3501e_core.c for the full rationale.  Not `static`, same test-visibility
 * reason as its neighbours in this header. */
bool cc3501e_fw_major_is_acceptable(uint8_t fw_major);

/* v4.0 (#2035): the pure reply-decision cc3501e_request_locked() defers to --
 * major-aware status decode (legacy 0x00 vs 4.0 0x5A), the CRC-16/CCITT-FALSE
 * check on a fw_proto_major-4 wire, and the #1378 dead-phase guard, all in one
 * place.  Not `static`, for the same test-visibility reason as
 * cc3501e_reply_may_be_all_zero() above: it takes no ctx and does no I/O, so
 * tests/zephyr/chips/src/test_cc3501e.c can drive every wire shape (legacy,
 * 4.0-valid-CRC, 4.0-corrupted-CRC, an all-zero dead phase under each) with
 * fabricated frames and no SPI bus.  See cc3501e_core.c for the full
 * rationale. */
alp_status_t cc3501e_reply_verdict(alp_cc3501e_cmd_t cmd,
                                   uint8_t           fw_proto_major,
                                   const uint8_t     reply_hdr[ALP_CC3501E_HEADER_BYTES],
                                   const uint8_t    *payload,
                                   uint16_t          payload_len);

/* #2035: whether @p mac is a plausible station address -- rejects an
 * all-zero address and one with the IEEE group/multicast bit set (mac[0]
 * bit 0), neither of which a real CC3501E station MAC can be.  Not
 * `static` for the same test-visibility reason as
 * cc3501e_reply_may_be_all_zero() above; used by cc3501e_wifi_get_mac()
 * in cc3501e_wifi.c. */
bool cc3501e_mac_is_valid(const uint8_t mac[CC3501E_MAC_LEN]);

/* Cause-agnostic link recovery (issue #2126): probe with cc3501e_ping() up to
 * CC3501E_LINK_PROBE_TRIES times (>= CC3501E_WIFI_DOWN_WINDOW_MS, wide enough
 * to outlast a legitimate transport blackout); if every probe fails,
 * warm-reset via cc3501e_recover(), which re-establishes wire state itself.
 * Declared here rather than in the public <alp/chips/cc3501e/core.h> --
 * unlike cc3501e_recover() (which a caller invokes explicitly, e.g. `alp
 * companion recover`), nothing outside this driver's own wrapper files
 * decides WHEN to call this; it is wired into their own failure exits below,
 * exactly once per failed top-level op (poll_by_repeat_seq()'s terminal
 * returns in cc3501e_core.c, cc3501e_wifi_connect()'s timeout exit in
 * cc3501e_wifi.c), never from inside a retry loop. A caller that wants an
 * unconditional, hand-driven recovery already has cc3501e_recover() for
 * that. Guards internally against an open OTA/update session
 * (ctx->ota_session_active, cc3501e_ota.c -- NOT cc3501e_peer_is_polled(), a
 * transport-framing detail, not a session marker), a missing reset_pin, the
 * Kconfig switch (CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER), a recovery already
 * in flight (ctx->recovering), and an attempt-based, back-off cooldown since
 * cc3501e_recover()'s own last attempt (shared with a manual `alp companion
 * recover`) -- see cc3501e_core.c for the full rationale. Not `static` for
 * the same test-visibility shape as this header's other entries, though
 * tests/zephyr/cc3501e_host_driver exercises it only through the public
 * wrapper functions + ctx->recover_count / recover_attempt_count. */
alp_status_t cc3501e_link_check_and_recover(cc3501e_t *ctx);

#endif /* CC3501E_INTERNAL_H */

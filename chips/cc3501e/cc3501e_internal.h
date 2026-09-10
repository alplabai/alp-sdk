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

/* Tell the transport the peer is a POLLED slave (OTA update mode): the READY
 * gate then waits for a LOW->HIGH edge instead of a level. */
void cc3501e_set_peer_polled(bool on);

/* True when the host believes the peer is running the POLLED update-mode boot. */
bool cc3501e_peer_is_polled(void);

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

#endif /* CC3501E_INTERNAL_H */

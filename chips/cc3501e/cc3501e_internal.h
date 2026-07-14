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

#endif /* CC3501E_INTERNAL_H */

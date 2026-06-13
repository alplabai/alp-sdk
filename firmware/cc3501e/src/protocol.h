/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: internal protocol header.
 *
 * Declares the dispatcher entry that every transport (SPI default,
 * SDIO optional) feeds into.  Per the project-memory rule "don't fork
 * the protocol -- one framing format, one command set, one set of
 * reply codes; only the transport layer differs" (the same model the
 * gd32-bridge uses for its SPI + I2C transports), both transports call
 * protocol_dispatch().
 *
 * The wire contract -- opcodes, flags, payload structs, response codes
 * -- is the CANONICAL host-side header include/alp/protocol/cc3501e.h,
 * included DIRECTLY here.  Because the firmware tree is embedded in
 * alp-sdk (not a separate repo), there is no mirrored copy to drift
 * out of sync: a protocol change in that one header moves the host
 * driver, this firmware, and the wire-vector tests in a single commit.
 */

#ifndef CC3501E_BRIDGE_PROTOCOL_H
#define CC3501E_BRIDGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Canonical wire contract -- single source of truth, no mirror.
 * Resolved via the firmware CMake's include path onto the repo-root
 * include/ directory. */
#include "alp/protocol/cc3501e.h"

/* --------------------------------------------------------------- */
/* Reply framing contract (firmware side)                            */
/* --------------------------------------------------------------- */
/*
 * On-wire reply frame mirrors the request shape (see the frame
 * diagram in <alp/protocol/cc3501e.h>):
 *
 *   +--------+--------+--------+--------+========================+
 *   |  cmd   | flags  |  payload_len (LE)|  payload (status+data) |
 *   +--------+--------+--------+--------+========================+
 *
 * Per the protocol header's stated convention -- "Response status
 * codes carried in the first byte of every response payload" -- the
 * reply payload is [status:u8][data...].  protocol_dispatch() writes
 * only the DATA bytes (after the status) and RETURNS the status; the
 * transport prepends the status byte and builds the 4-byte header.
 *
 * CONTRACT NOTE (host reconciliation, tracked in DESIGN.md): the
 * Alif-side driver chips/cc3501e/cc3501e.c is marked [UNTESTED] and
 * currently (a) reads the reply inside the same full-duplex transceive
 * as the request -- impossible for a real slave, which only knows the
 * reply AFTER the request is fully clocked -- and (b) treats the whole
 * GET_VERSION payload as the version with no status byte.  This
 * firmware implements the staged two-transaction model + the
 * status-byte convention from the authoritative header; the host
 * driver's bring-up rework (busy/ready handshake + status parse) is
 * the #1 reconciliation follow-up.  Nothing is on silicon yet, so the
 * two sides are reconciled to the spec, not to each other's drafts.
 */

/* Maximum reply DATA bytes a handler may emit (after the status byte).
 * The reply payload is status(1) + data; the whole payload stays within
 * the protocol's ALP_CC3501E_MAX_PAYLOAD ceiling. */
#define CC3501E_REPLY_DATA_MAX (ALP_CC3501E_MAX_PAYLOAD - 1u)

/* Whole-frame sizes (header + max payload), shared by every transport. */
#define CC3501E_FRAME_MAX_BYTES (ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD)

/* Byte offsets within a reply frame: the status byte is the first
 * payload byte; reply DATA follows it. */
#define CC3501E_REPLY_STATUS_OFF (ALP_CC3501E_HEADER_BYTES)    /* index 4 */
#define CC3501E_REPLY_DATA_OFF (ALP_CC3501E_HEADER_BYTES + 1u) /* index 5 */

/* --------------------------------------------------------------- */
/* Dispatcher                                                        */
/* --------------------------------------------------------------- */
/*
 * protocol_dispatch -- called by a transport once a complete request
 * frame (header + payload) has been validated (framing OK).
 *
 * Inputs:
 *   cmd          -- opcode (one of ALP_CC3501E_CMD_* / EVT_*).
 *   flags        -- request flags byte (ALP_CC3501E_FLAG_*).
 *   req          -- pointer to req_len request payload bytes (may be
 *                   NULL when req_len == 0).
 *   req_len      -- request payload length.
 *
 * Outputs (caller-supplied):
 *   reply_data       -- buffer for the reply DATA bytes (the bytes that
 *                       follow the status byte in the reply payload).
 *   reply_cap        -- capacity of reply_data.
 *   reply_data_len   -- [out] number of DATA bytes written.
 *
 * Return: the response status (ALP_CC3501E_RESP_*) the transport
 *         emits as the first reply-payload byte.  Unknown or
 *         not-yet-implemented opcodes return ALP_CC3501E_RESP_ERR_INVALID
 *         (per the header: v1 firmware rejects opcodes it does not
 *         implement with RESP_ERR_INVALID).
 */
alp_cc3501e_resp_t protocol_dispatch(uint8_t cmd, uint8_t flags, const uint8_t *req, size_t req_len,
                                     uint8_t *reply_data, size_t reply_cap, size_t *reply_data_len);

/*
 * protocol_build_reply -- the transport-agnostic framing wrapper.
 *
 * Parses a complete request FRAME (4-byte header + payload), validates
 * the framing, runs protocol_dispatch(), and writes a complete reply
 * FRAME (header + status + data) into @p reply_frame.  Every transport
 * (SPI, SDIO) calls this so the on-wire framing is byte-identical
 * regardless of which link the customer selected.
 *
 *   req_frame / req_len  -- the received request frame.  A frame too
 *                           short to hold a header, or whose declared
 *                           payload_len doesn't match req_len, yields a
 *                           RESP_ERR_PROTOCOL reply frame.
 *   reply_frame          -- output buffer; MUST be at least
 *                           CC3501E_FRAME_MAX_BYTES.
 *   reply_cap            -- capacity of reply_frame.
 *
 * Returns the reply frame length in bytes (always >= header + 1).
 */
size_t protocol_build_reply(const uint8_t *req_frame, size_t req_len, uint8_t *reply_frame,
                            size_t reply_cap);

#endif /* CC3501E_BRIDGE_PROTOCOL_H */

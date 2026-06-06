/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal IPC envelope helpers for <alp/mproc.h>.  NOT part of the
 * public SDK surface -- consumers always go through alp_mbox_send /
 * mbox_rx_callback and the framing happens transparently inside the
 * SDK when CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y.
 *
 * Why a separate helper:
 *
 *   The v0.4-final mproc wire format is protobuf, encoded against
 *   `metadata/protos/alp_mproc.proto` using nanopb (vendors/nanopb/).
 *   Generating the .pb.c sources requires the nanopb generator
 *   (Python), which depends on the upstream MaJerle/lwrb +
 *   nanopb/nanopb packs landed via the `extras-v04` west group.
 *   Until that lands the SDK ships a *placeholder* binary framing
 *   here -- a fixed 12-byte header plus the caller's payload --
 *   that exercises the same code path inside `mproc_zephyr.c` so
 *   the wiring is shaken out before the real codec drops in.
 *
 *   Wire-format compatibility note: the placeholder envelope is
 *   NOT compatible with the v0.4-final protobuf wire.  Firmwares
 *   built against this header MUST NOT talk to firmwares built
 *   against the nanopb-generated codec.  The CONFIG_ flag stays
 *   off by default so v0.3 / v0.4-prep workspaces keep using the
 *   raw passthrough; flip it on when both ends of an IPC channel
 *   build against this same source.
 *
 * Header layout (little-endian):
 *
 *     offset 0: u32 magic    = 0x46504D41  ('A','M','P','F' LE)
 *     offset 4: u32 sequence (per-mbox monotonic)
 *     offset 8: u32 length   (payload bytes following the header)
 *     offset 12: payload bytes
 *
 *   Total framed size = ALP_MPROC_FRAME_HEADER_BYTES + payload_len.
 *
 * Caller responsibilities:
 *   - Allocate the scratch buffer at least
 *     ALP_MPROC_FRAME_HEADER_BYTES + payload_len bytes.
 *   - Maintain the per-mbox sequence counter and pass the
 *     pre-incremented value to encode().
 *   - Treat decode failures as drop-the-frame.  No partial decode.
 */

#ifndef ALP_INTERNAL_MPROC_FRAME_H_
#define ALP_INTERNAL_MPROC_FRAME_H_

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"  /* alp_status_t */

#ifdef __cplusplus
extern "C" {
#endif

#define ALP_MPROC_FRAME_MAGIC          0x46504D41u  /* 'AMPF' LE */
#define ALP_MPROC_FRAME_HEADER_BYTES   12u

/**
 * @brief Pack @p payload (length @p payload_len) into a framed
 *        envelope in @p out (capacity @p out_cap).
 *
 * @param sequence     Per-mbox monotonic counter the caller manages.
 * @param payload      Caller's raw bytes; may be NULL iff payload_len == 0.
 * @param payload_len  Bytes in @p payload.
 * @param out          Destination buffer.  Must have at least
 *                     ALP_MPROC_FRAME_HEADER_BYTES + payload_len bytes.
 * @param out_cap      Capacity of @p out.
 * @param out_len      [out] Total framed bytes written.  May be NULL.
 *
 * @return ALP_OK on success.
 *         ALP_ERR_INVAL on NULL out / NULL payload-with-len.
 *         ALP_ERR_NOMEM if @p out_cap is too small.
 */
alp_status_t alp_mproc_frame_encode(uint32_t       sequence,
                                    const void    *payload,
                                    size_t         payload_len,
                                    uint8_t       *out,
                                    size_t         out_cap,
                                    size_t        *out_len);

/**
 * @brief Decode a framed envelope, returning a pointer into @p frame
 *        for the payload.
 *
 * Zero-copy: @p payload_out is set to @p frame + header so the caller
 * inspects the payload without an intermediate buffer.  The pointer
 * is valid for as long as @p frame is.
 *
 * @param frame         Framed bytes received from the peer.
 * @param frame_len     Length of @p frame.
 * @param sequence_out  [out] Sender's sequence.  May be NULL.
 * @param payload_out   [out] Pointer into @p frame at the payload
 *                      start.  May be NULL.
 * @param payload_len_out [out] Payload byte count.  May be NULL.
 *
 * @return ALP_OK on success.
 *         ALP_ERR_INVAL on NULL frame.
 *         ALP_ERR_IO on magic mismatch / declared length exceeding
 *         frame_len / frame shorter than the header (treated as a
 *         line-level integrity failure -- caller should drop the
 *         frame).
 */
alp_status_t alp_mproc_frame_decode(const uint8_t *frame,
                                    size_t         frame_len,
                                    uint32_t      *sequence_out,
                                    const uint8_t **payload_out,
                                    size_t        *payload_len_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_INTERNAL_MPROC_FRAME_H_ */

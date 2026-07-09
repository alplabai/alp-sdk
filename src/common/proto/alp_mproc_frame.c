/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Placeholder binary framing for <alp/mproc.h>'s alp_mbox_send path.
 * See alp_mproc_frame.h for the wire layout + replacement plan.
 *
 * Hand-rolled little-endian put/get so the impl is portable across
 * every E1M target without pulling in endian.h or compiler intrinsics.
 * Replaced wholesale by the nanopb-generated codec when the
 * `extras-lwrb-nanopb` west group lands the real upstream + the
 * generator step at build time -- interim/deferred as of v0.9, no
 * committed release date.
 */

#include <string.h>

#include "proto/alp_mproc_frame.h"

static void put_u32_le(uint8_t *dst, uint32_t v)
{
	dst[0] = (uint8_t)(v & 0xFFu);
	dst[1] = (uint8_t)((v >> 8) & 0xFFu);
	dst[2] = (uint8_t)((v >> 16) & 0xFFu);
	dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32_le(const uint8_t *src)
{
	return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
	       ((uint32_t)src[3] << 24);
}

alp_status_t alp_mproc_frame_encode(uint32_t    sequence,
                                    const void *payload,
                                    size_t      payload_len,
                                    uint8_t    *out,
                                    size_t      out_cap,
                                    size_t     *out_len)
{
	if (out_len != NULL) {
		*out_len = 0;
	}
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (payload == NULL && payload_len > 0) {
		return ALP_ERR_INVAL;
	}
	if (payload_len > UINT32_MAX) {
		return ALP_ERR_INVAL;
	}
	size_t framed = (size_t)ALP_MPROC_FRAME_HEADER_BYTES + payload_len;
	if (framed > out_cap) {
		return ALP_ERR_NOMEM;
	}
	put_u32_le(out + 0, ALP_MPROC_FRAME_MAGIC);
	put_u32_le(out + 4, sequence);
	put_u32_le(out + 8, (uint32_t)payload_len);
	if (payload_len > 0) {
		memcpy(out + ALP_MPROC_FRAME_HEADER_BYTES, payload, payload_len);
	}
	if (out_len != NULL) {
		*out_len = framed;
	}
	return ALP_OK;
}

alp_status_t alp_mproc_frame_decode(const uint8_t  *frame,
                                    size_t          frame_len,
                                    uint32_t       *sequence_out,
                                    const uint8_t **payload_out,
                                    size_t         *payload_len_out)
{
	if (sequence_out != NULL) *sequence_out = 0;
	if (payload_out != NULL) *payload_out = NULL;
	if (payload_len_out != NULL) *payload_len_out = 0;

	if (frame == NULL) {
		return ALP_ERR_INVAL;
	}
	if (frame_len < ALP_MPROC_FRAME_HEADER_BYTES) {
		return ALP_ERR_IO;
	}
	uint32_t magic = get_u32_le(frame + 0);
	if (magic != ALP_MPROC_FRAME_MAGIC) {
		return ALP_ERR_IO;
	}
	uint32_t seq = get_u32_le(frame + 4);
	uint32_t len = get_u32_le(frame + 8);
	if ((size_t)len > frame_len - ALP_MPROC_FRAME_HEADER_BYTES) {
		return ALP_ERR_IO;
	}
	if (sequence_out != NULL) *sequence_out = seq;
	if (payload_out != NULL) *payload_out = frame + ALP_MPROC_FRAME_HEADER_BYTES;
	if (payload_len_out != NULL) *payload_len_out = (size_t)len;
	return ALP_OK;
}

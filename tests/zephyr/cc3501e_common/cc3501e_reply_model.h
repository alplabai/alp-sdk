/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared reply-framing helper for the CC3501E software-SPI-slave test
 * models under tests/zephyr/cc3501e_* (host_driver, host_ota, host_events,
 * transport_lock, ble_gatt_register, console_wifi, wifi_backend_security).
 * Each of those suites hermetically plays the firmware slave in its own
 * alp_spi_transceive() stub; this header is the ONE place that knows how a
 * real wire MAJOR 4 reply is shaped, so none of the seven duplicate it.
 *
 * See <alp/protocol/cc3501e.h> (the MAJOR-4 / #2035 block) for the rules
 * this reproduces:
 *
 *   - every reply's payload is zero-padded up to an ALP_CC3501E_REPLY_PAD
 *     (8 B) multiple, with the pad folded INTO the declared payload_len;
 *   - from wire MAJOR 4, the LAST ALP_CC3501E_CRC_BYTES (2) of that padded
 *     payload carry a CRC-16/CCITT-FALSE trailer covering the reply's own
 *     4-byte header (cmd | flags(0, solicited) | payload_len LE16) plus
 *     every payload byte except the trailer itself.
 *
 * cc3501e_reply_verdict() (chips/cc3501e/cc3501e_core.c) requires this shape
 * for ANY reply whose status byte is ALP_CC3501E_RESP_OK (0x5A) -- even
 * before fw_proto_major is negotiated (fw_proto_major == 0), because 0x5A
 * cannot be a legacy status byte at all (see that function's fw_proto_major
 * == 0 branch).  A model that stages a bare 0x5A with no trailer is not a
 * shape any real MAJOR-4 firmware would ever send, and the host correctly
 * rejects it -- that was the defect this header fixes (#2035 follow-up).
 *
 * cc3501e_model_stage_reply() builds that real shape; every RESP_OK reply
 * these test models stage should go through it.  A non-OK status
 * (ALP_CC3501E_RESP_ERR_*) is NOT required to carry the trailer at
 * fw_proto_major == 0 -- only 0x5A is self-evidently MAJOR-4 before the
 * major is known -- so those may keep using the plain, unpadded
 * cc3501e_model_stage_legacy_reply() shape, which also doubles as the
 * legacy (wire MAJOR 3) coverage some suites exercise deliberately.
 */

#ifndef CC3501E_TEST_REPLY_MODEL_H
#define CC3501E_TEST_REPLY_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/protocol/cc3501e.h"
#include "alp/protocol/crc16.h"

/**
 * @brief Build a MAJOR-4-shaped reply frame: status + data, zero-padded to
 *        an ALP_CC3501E_REPLY_PAD multiple, with a CRC-16/CCITT-FALSE
 *        trailer in the last ALP_CC3501E_CRC_BYTES bytes -- the only shape
 *        a real MAJOR-4 firmware ever sends (see the file header above).
 *
 * Sizing: the returned padded length never exceeds
 * `1 + data_len + ALP_CC3501E_REPLY_PAD` -- size @p pl for the caller's own
 * largest @p data_len plus that much headroom.
 *
 * @param pl       Destination buffer for the reply payload (status + data +
 *                 pad + CRC); the caller owns its sizing (see above).
 * @param cmd      The opcode this reply answers -- goes into the reply
 *                 header's cmd byte, which the CRC covers.  Pass the SAME
 *                 value the model's PH_REPLY_HDR phase will echo as rx[0]
 *                 (usually `slave.cmd`, already latched from the just-landed
 *                 request by the time a dispatch function runs).
 * @param status   The reply's own status byte, usually ALP_CC3501E_RESP_OK.
 * @param data     Reply DATA bytes following the status byte; may be NULL
 *                 iff @p data_len is 0.
 * @param data_len Byte count of @p data.
 * @return The padded reply length -- assign this to the caller's own
 *         `reply_len` (what the model's PH_REPLY_HDR phase declares in the
 *         header and PH_REPLY_PL then drains @p pl over).
 */
static inline uint16_t cc3501e_model_stage_reply(uint8_t       *pl,
                                                 uint8_t        cmd,
                                                 uint8_t        status,
                                                 const uint8_t *data,
                                                 uint16_t       data_len)
{
	const uint16_t real_len = (uint16_t)(1u + data_len);
	const uint16_t padded_len =
	    (uint16_t)(((uint32_t)real_len + ALP_CC3501E_CRC_BYTES + ALP_CC3501E_REPLY_PAD - 1u) /
	               ALP_CC3501E_REPLY_PAD * ALP_CC3501E_REPLY_PAD);

	pl[0] = status;
	if (data_len > 0u) {
		memcpy(&pl[1], data, data_len);
	}
	if (padded_len > real_len) {
		memset(&pl[real_len], 0, (size_t)(padded_len - real_len));
	}

	const uint8_t hdr[ALP_CC3501E_HEADER_BYTES] = {
		cmd,
		0x00u, /* solicited reply, matches every model's PH_REPLY_HDR rx[1] */
		(uint8_t)(padded_len & 0xFFu),
		(uint8_t)((padded_len >> 8) & 0xFFu),
	};
	uint16_t crc = alp_crc16_ccitt_false(hdr, ALP_CC3501E_HEADER_BYTES);
	crc = alp_crc16_ccitt_false_update(crc, pl, (size_t)(padded_len - ALP_CC3501E_CRC_BYTES));
	pl[padded_len - 2u] = (uint8_t)(crc & 0xFFu);
	pl[padded_len - 1u] = (uint8_t)((crc >> 8) & 0xFFu);

	return padded_len;
}

/**
 * @brief Build a legacy (wire MAJOR 3) reply frame: status + data, byte-
 *        exact, no padding, no CRC trailer -- what pre-4.0 firmware sends,
 *        and the shape cc3501e_reply_verdict() decodes a reply as whenever
 *        the status byte is not ALP_CC3501E_RESP_OK (0x5A) and fw_proto_major
 *        has not yet settled on MAJOR 4.  Also what a test drives
 *        deliberately to exercise the host's bilingual (MAJOR 3 / MAJOR 4)
 *        decode -- see <alp/protocol/cc3501e.h>'s "THE HOST IS BILINGUAL"
 *        migration note.
 *
 * @return The (unpadded) reply length -- 1 + @p data_len.
 */
static inline uint16_t cc3501e_model_stage_legacy_reply(uint8_t       *pl,
                                                        uint8_t        status,
                                                        const uint8_t *data,
                                                        uint16_t       data_len)
{
	pl[0] = status;
	if (data_len > 0u) {
		memcpy(&pl[1], data, data_len);
	}
	return (uint16_t)(1u + data_len);
}

#endif /* CC3501E_TEST_REPLY_MODEL_H */

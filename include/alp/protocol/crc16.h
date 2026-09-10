/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file crc16.h
 * @brief CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), shared by every
 *        ALP Lab bridge wire protocol that frames a trailer CRC.
 *
 * The gd32-bridge protocol (chips/gd32g553/gd32g553.c) was first to frame
 * `SOF | CMD | PAYLOAD | CRC` this way; the CC3501E wire protocol v4.0
 * (<alp/protocol/cc3501e.h>, ADR 0033) adopts the identical algorithm for
 * its own `HEADER | PAYLOAD | CRC` trailer.  This header exists so both
 * sides -- and both bridge protocols -- share ONE implementation instead of
 * two copies that could silently drift apart.  `cc3501e-bridge-firmware`
 * (a separate repo) is expected to vendor this same file; see the wire
 * v4.0 migration notes in cc3501e.h.
 *
 * Header-only `static inline`: no new translation unit to wire into every
 * backend's CMakeLists.txt, and it stays trivially includable from both the
 * OS-agnostic chip cores (chips/cc3501e/ and chips/gd32g553/) and any
 * host-side tooling that wants to compute the same CRC off-target.
 *
 * SOFTWARE BY CHOICE, NOT OVERSIGHT (CC3501E wire v4.0, #2035).  Neither side
 * of that link plumbs a CRC peripheral today -- no CRC device-tree node or
 * register library outside the BLE stack in hal_alif, no CRC block listed in
 * metadata/socs/alif/ensemble/e8.json, and no CRC reference anywhere in
 * cc3501e-bridge-firmware's hal/ or vendor/ trees -- and this file does not
 * claim either part's SILICON lacks one, only that nothing in either tree
 * exposes one today.  Two reasons this stays software regardless:
 *
 *   1. Cost is noise at the sizes that matter.  A typical CC3501E reply is
 *      8..16 bytes, so a bitwise CRC here costs on the order of 640 cycles --
 *      nothing against the 250 us inter-phase settle the transport already
 *      pays per phase (CC3501E_PHASE_SETTLE_US, cc3501e_core.c).  The one
 *      size where hardware would actually matter is the 4096-byte
 *      ALP_CC3501E_MAX_PAYLOAD ceiling, and the path that approaches it (OTA)
 *      moves in 256-byte chunks, not one 4 KB burst.
 *   2. The ISR hazard is the real argument against it.  The firmware builds
 *      replies inside the SPI callback.  A CRC peripheral shared with
 *      application code touched from that context needs a lock or a
 *      save/restore around every use, and getting that wrong yields a
 *      silently WRONG CRC -- strictly worse than a slower correct one, on a
 *      link whose entire problem is undetected corruption.
 *
 * UPGRADE PATH.  If a measurement ever justifies hardware, it goes behind
 * THIS call -- alp_crc16_ccitt_false() / alp_crc16_ccitt_false_update() --
 * following the SDK's existing portable-hardware-offload-with-software-
 * fallback pattern (ADR 0017).  One place to change, and both repos (this
 * one and cc3501e-bridge-firmware, once it vendors this file) pick it up.
 */

#ifndef ALP_PROTOCOL_CRC16_H
#define ALP_PROTOCOL_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-16/CCITT-FALSE initial register value. */
#define ALP_CRC16_CCITT_FALSE_INIT 0xFFFFu

/**
 * @brief Fold @p len bytes of @p buf into a running CRC-16/CCITT-FALSE.
 *
 * Lets a caller CRC a frame that is not contiguous in memory (e.g. a
 * 4-byte header followed separately by a payload buffer) by chaining two
 * calls: `crc = alp_crc16_ccitt_false_update(ALP_CRC16_CCITT_FALSE_INIT,
 * hdr, 4); crc = alp_crc16_ccitt_false_update(crc, payload, payload_len);`
 * -- identical to CRC-ing the two buffers concatenated.
 *
 * @param crc Running CRC register, @ref ALP_CRC16_CCITT_FALSE_INIT to
 *            start a fresh CRC.
 * @param buf Bytes to fold in; may be NULL iff @p len is 0.
 * @param len Byte count of @p buf.
 * @return    The updated CRC register.
 */
static inline uint16_t alp_crc16_ccitt_false_update(uint16_t crc, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		crc ^= (uint16_t)buf[i] << 8;
		for (unsigned b = 0; b < 8; ++b) {
			if (crc & 0x8000u) {
				crc = (uint16_t)((crc << 1) ^ 0x1021u);
			} else {
				crc <<= 1;
			}
		}
	}
	return crc;
}

/**
 * @brief CRC-16/CCITT-FALSE of one contiguous buffer.
 *
 * Equivalent to `alp_crc16_ccitt_false_update(ALP_CRC16_CCITT_FALSE_INIT, buf,
 * len)`; the common single-buffer case.
 */
static inline uint16_t alp_crc16_ccitt_false(const uint8_t *buf, size_t len)
{
	const uint16_t crc = alp_crc16_ccitt_false_update(ALP_CRC16_CCITT_FALSE_INIT, buf, len);
	return crc;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PROTOCOL_CRC16_H */

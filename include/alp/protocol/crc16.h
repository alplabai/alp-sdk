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
 *   1. Software is fast enough once it is byte-wise.  The first version ran
 *      an 8-step bit loop per byte; STREAM_WRITE and SOCK_SEND frames reach
 *      the 4096-byte ALP_CC3501E_MAX_PAYLOAD ceiling, and there that loop
 *      cost ~1.3 ms of M55-HE time per frame -- measured 561 -> 687 KB/s on
 *      4092-byte STREAM_WRITE frames when it was replaced by a byte-at-a-time
 *      shift form, and the 256-entry table below cut the rest to ~0.3 ms
 *      at -O2 on an M55-HE.  The table is 512 bytes of const data, emitted
 *      only in translation units that call the helper.
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
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      `static inline alp_crc16_ccitt_false[_update]()` -- API, though not
 *      linker ABI.  See docs/abi-markers.md.
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
	/* One table lookup per byte.  The bit loop this replaced cost ~1.3 ms
	 * per 4 KiB frame on an M55; the CC3501E bridge CRCs every request. */
	/* 0x1021 byte table: entry i is the CRC register after folding byte i
	 * into a zero register.  Generated, and checked against the bit loop by
	 * test_crc16_bytewise_matches_bitwise.  Function-local so the header
	 * declares nothing new at file scope. */
	/* clang-format off */
	static const uint16_t table[256] = {
		0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
		0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
		0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52B5u, 0x4294u, 0x72F7u, 0x62D6u,
		0x9339u, 0x8318u, 0xB37Bu, 0xA35Au, 0xD3BDu, 0xC39Cu, 0xF3FFu, 0xE3DEu,
		0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64E6u, 0x74C7u, 0x44A4u, 0x5485u,
		0xA56Au, 0xB54Bu, 0x8528u, 0x9509u, 0xE5EEu, 0xF5CFu, 0xC5ACu, 0xD58Du,
		0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76D7u, 0x66F6u, 0x5695u, 0x46B4u,
		0xB75Bu, 0xA77Au, 0x9719u, 0x8738u, 0xF7DFu, 0xE7FEu, 0xD79Du, 0xC7BCu,
		0x48C4u, 0x58E5u, 0x6886u, 0x78A7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
		0xC9CCu, 0xD9EDu, 0xE98Eu, 0xF9AFu, 0x8948u, 0x9969u, 0xA90Au, 0xB92Bu,
		0x5AF5u, 0x4AD4u, 0x7AB7u, 0x6A96u, 0x1A71u, 0x0A50u, 0x3A33u, 0x2A12u,
		0xDBFDu, 0xCBDCu, 0xFBBFu, 0xEB9Eu, 0x9B79u, 0x8B58u, 0xBB3Bu, 0xAB1Au,
		0x6CA6u, 0x7C87u, 0x4CE4u, 0x5CC5u, 0x2C22u, 0x3C03u, 0x0C60u, 0x1C41u,
		0xEDAEu, 0xFD8Fu, 0xCDECu, 0xDDCDu, 0xAD2Au, 0xBD0Bu, 0x8D68u, 0x9D49u,
		0x7E97u, 0x6EB6u, 0x5ED5u, 0x4EF4u, 0x3E13u, 0x2E32u, 0x1E51u, 0x0E70u,
		0xFF9Fu, 0xEFBEu, 0xDFDDu, 0xCFFCu, 0xBF1Bu, 0xAF3Au, 0x9F59u, 0x8F78u,
		0x9188u, 0x81A9u, 0xB1CAu, 0xA1EBu, 0xD10Cu, 0xC12Du, 0xF14Eu, 0xE16Fu,
		0x1080u, 0x00A1u, 0x30C2u, 0x20E3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
		0x83B9u, 0x9398u, 0xA3FBu, 0xB3DAu, 0xC33Du, 0xD31Cu, 0xE37Fu, 0xF35Eu,
		0x02B1u, 0x1290u, 0x22F3u, 0x32D2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
		0xB5EAu, 0xA5CBu, 0x95A8u, 0x8589u, 0xF56Eu, 0xE54Fu, 0xD52Cu, 0xC50Du,
		0x34E2u, 0x24C3u, 0x14A0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
		0xA7DBu, 0xB7FAu, 0x8799u, 0x97B8u, 0xE75Fu, 0xF77Eu, 0xC71Du, 0xD73Cu,
		0x26D3u, 0x36F2u, 0x0691u, 0x16B0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
		0xD94Cu, 0xC96Du, 0xF90Eu, 0xE92Fu, 0x99C8u, 0x89E9u, 0xB98Au, 0xA9ABu,
		0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18C0u, 0x08E1u, 0x3882u, 0x28A3u,
		0xCB7Du, 0xDB5Cu, 0xEB3Fu, 0xFB1Eu, 0x8BF9u, 0x9BD8u, 0xABBBu, 0xBB9Au,
		0x4A75u, 0x5A54u, 0x6A37u, 0x7A16u, 0x0AF1u, 0x1AD0u, 0x2AB3u, 0x3A92u,
		0xFD2Eu, 0xED0Fu, 0xDD6Cu, 0xCD4Du, 0xBDAAu, 0xAD8Bu, 0x9DE8u, 0x8DC9u,
		0x7C26u, 0x6C07u, 0x5C64u, 0x4C45u, 0x3CA2u, 0x2C83u, 0x1CE0u, 0x0CC1u,
		0xEF1Fu, 0xFF3Eu, 0xCF5Du, 0xDF7Cu, 0xAF9Bu, 0xBFBAu, 0x8FD9u, 0x9FF8u,
		0x6E17u, 0x7E36u, 0x4E55u, 0x5E74u, 0x2E93u, 0x3EB2u, 0x0ED1u, 0x1EF0u,
	};
	/* clang-format on */

	for (size_t i = 0; i < len; ++i) {
		crc = (uint16_t)((crc << 8) ^ table[(uint8_t)((crc >> 8) ^ buf[i])]);
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

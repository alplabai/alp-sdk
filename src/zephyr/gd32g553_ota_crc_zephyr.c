/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-side implementation of gd32g553_ota_image_crc32() (declared in
 * <alp/chips/gd32g553.h>) -- kept OUT of chips/gd32g553/gd32g553.c on
 * purpose: that file is deliberately Zephyr-agnostic (bus access only,
 * via <alp/peripheral.h>) so it keeps compiling for a future baremetal
 * backend.  This TU is free to reach for <zephyr/drivers/crc.h>.
 *
 * Hardware fast path: the Alif Ensemble E8 CRC engine (crc0@0x48107000,
 * compatible "alif,crc", ADR 0017 Tier-1.5, zephyr/drivers/crc/crc_alif.c).
 * Only CRC0 is used here -- HWRM Table 15-29 lists a second instance
 * (CRC1@0x48108000); its accumulator is independent per-engine global
 * state (one k_sem per device instance in crc_alif.c), so a second
 * concurrent CRC user in the same firmware image should bind CRC1, not
 * share this one.  Nothing in this file assumes CRC1 doesn't exist; it
 * simply never reaches for it.
 *
 * Software fallback: a portable, table-free CRC-32/ISO-HDLC (the
 * "zlib.crc32" variant -- poly 0xEDB88320 reflected, init/xor-out
 * 0xFFFFFFFF), used whenever the hardware engine is absent from this
 * build, not instantiated in the active devicetree, not
 * device_is_ready() (HWRM Table 15-26: CRC0/CRC1 live in power domain
 * PD-6, which HWRM Section 8 documents as unavailable in STANDBY/STOP --
 * this driver has no PD-6 status read, so it never blocks trying to
 * find out; it just falls back), or an image length the hardware's
 * 32-bit word path cannot consume whole (HWRM 15.2.5.3.6:
 * CRC_DATA_IN_32_n takes whole 4-byte words only).  Both paths compute
 * the IDENTICAL value -- docs/gd32-bridge-protocol.md's "CRC-32 is IEEE
 * 802.3 reflected (zlib-compatible)" wire contract, unconditionally.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/chips/gd32g553.h"

#if defined(CONFIG_ALP_SDK_CHIP_GD32G553)

/* DT_HAS_COMPAT_STATUS_OKAY() below needs this -- the only includes above it
 * are <stddef.h>/<stdint.h>/alp/chips/gd32g553.h, which are Zephyr-free by
 * design (see the file header), so nothing else pulls it in.  Without it the
 * preprocessor macro-expands DT_HAS_COMPAT_STATUS_OKAY(alif_crc) to nothing
 * (undefined identifiers evaluate to 0 in a #if, but the now-bare `(alif_crc)`
 * that's left behind is not a valid constant-expression token sequence) --
 * `#if defined(CONFIG_CRC_ALIF) && DT_HAS_COMPAT_STATUS_OKAY(alif_crc)` is a
 * hard `missing binary operator before token "("` error on EVERY Zephyr
 * build, `&&` short-circuiting notwithstanding: macro expansion runs over the
 * whole line before the expression is evaluated, so the right operand is
 * expanded (and breaks) even when CONFIG_CRC_ALIF is unset. */
#include <zephyr/devicetree.h>

#if defined(CONFIG_CRC_ALIF) && DT_HAS_COMPAT_STATUS_OKAY(alif_crc)
#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>
#define GD32G553_OTA_CRC_HW_AVAILABLE 1
#endif

/*
 * CRC-32/ISO-HDLC, table-free bitwise form (init 0xFFFFFFFF, poly
 * 0xEDB88320 reflected, xor-out 0xFFFFFFFF) -- matches Python's
 * `zlib.crc32`, the same variant docs/gd32-bridge-protocol.md specifies
 * for the OTA wire contract and src/zephyr/hw_info_zephyr.c uses for the
 * (unrelated) EEPROM manifest checksum.  Self-contained rather than
 * calling that internal helper: this file's Kconfig gate
 * (CONFIG_ALP_SDK_CHIP_GD32G553) is independent of hw_info's
 * (CONFIG_ALP_SDK_HW_INFO), so a build could enable one without the
 * other.  Unrelated to, and a different algorithm from, the per-frame
 * CRC-16/CCITT-FALSE trailer: chips/gd32g553/gd32g553.c and
 * chips/cc3501e/cc3501e_core.c both now share ONE implementation of that
 * (alp_crc16_ccitt_false(), <alp/protocol/crc16.h>) -- the unification
 * that header's own doc comment claims is real, not aspirational -- but
 * this file computes the OTA IMAGE's CRC-32/ISO-HDLC checksum, a
 * different algorithm entirely, and stays self-contained here for the
 * Kconfig-independence reason above, not because no shared helper
 * exists.
 */
static uint32_t gd32g553_ota_crc32_sw(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < len; ++i) {
		crc ^= (uint32_t)buf[i];
		for (unsigned b = 0; b < 8; ++b) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);

			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

alp_status_t gd32g553_ota_image_crc32(const uint8_t *image, size_t len, uint32_t *out_crc32)
{
	if (out_crc32 == NULL || (image == NULL && len != 0u)) {
		return ALP_ERR_INVAL;
	}

#if defined(GD32G553_OTA_CRC_HW_AVAILABLE)
	/* The 32-bit data register only ever consumes whole 4-byte words
	 * (HWRM 15.2.5.3.6); crc_alif_update() returns -ENOTSUP for any
	 * other length rather than silently truncating, so pre-filter it
	 * here and never even attempt the engine on a buffer it cannot
	 * eat whole. */
	if (len % 4u == 0u) {
		const struct device *const ota_crc_dev =
		    DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(alif_crc));

		if (device_is_ready(ota_crc_dev)) {
			/* CRC-32/IEEE (zlib-compatible): seed 0xFFFFFFFF, both
			 * reverse flags -- see crc_alif_begin()'s own citation of
			 * the DFP demo + fork conformance test for why REFLECT_
			 * CRC alone is not enough (input swap + final invert are
			 * both required for the canonical reflected result). */
			struct crc_ctx ctx = {
				.type       = CRC32_IEEE,
				.polynomial = CRC32_IEEE_POLY,
				.seed       = CRC32_IEEE_INIT_VAL,
				.reversed   = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
			};

			if (crc_begin(ota_crc_dev, &ctx) == 0) {
				int rc = crc_update(ota_crc_dev, &ctx, image, len);

				if (rc == 0) {
					rc = crc_finish(ota_crc_dev, &ctx);
				} else if (ctx.state == CRC_STATE_IN_PROGRESS) {
					/* update() failed without releasing the engine
					 * itself (the -ENOTSUP length-mismatch path in
					 * crc_alif_update() already would have, but this
					 * branch is unreachable given the %4 pre-filter
					 * above -- kept as a belt-and-suspenders release
					 * so a future engine failure mode can't wedge a
					 * later OTA attempt on the single-count k_sem). */
					(void)crc_finish(ota_crc_dev, &ctx);
				}

				if (rc == 0) {
					*out_crc32 = ctx.result;
					return ALP_OK;
				}
			}
			/* begin/update/finish failed (e.g. a bus error because
			 * PD-6 dropped mid-calculation) -- fall through to
			 * software rather than fail OTA verification outright. */
		}
	}
#endif /* GD32G553_OTA_CRC_HW_AVAILABLE */

	*out_crc32 = gd32g553_ota_crc32_sw(image, len);
	return ALP_OK;
}

#endif /* CONFIG_ALP_SDK_CHIP_GD32G553 */

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the EEPROM manifest classifier,
 * `alp_hw_info_classify_manifest()` and its CRC-32 in
 * `src/zephyr/hw_info_zephyr.c`: what `alp_hw_info_read()` runs on the 128
 * bytes it reads off the SoM's 24C128.
 *
 * The harness links the real file; nothing is stubbed.  Input layout:
 *
 *   byte[0]    bit 0 = stamp the ALPH magic, bit 1 = stamp the schema version,
 *              bit 2 = stamp a valid CRC32 (via the production
 *              alp_hw_info_crc32), so the deeper checks stay reachable
 *   byte[1..]  the manifest, zero-padded / truncated to sizeof(alp_hw_info_eeprom_t)
 *
 * Checked on every input: a non-OK verdict leaves the output untouched, and
 * an OK one carries NUL-terminated SoM strings and a manifest that really has
 * the magic, schema version and CRC the classifier claims to have verified.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_eeprom_manifest
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hw_info_manifest.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1u) return 0;

	/* A struct, not a cast of `data`: libFuzzer gives no alignment guarantee. */
	alp_hw_info_eeprom_t m;
	memset(&m, 0, sizeof(m));
	const size_t n = (size - 1u) < sizeof(m) ? (size - 1u) : sizeof(m);
	memcpy(&m, data + 1, n);

	if ((data[0] & 1u) != 0u) m.magic = ALP_HW_INFO_MAGIC;
	if ((data[0] & 2u) != 0u) m.schema_version = ALP_HW_INFO_SCHEMA_VERSION;
	if ((data[0] & 4u) != 0u) {
		m.crc32 = alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
	}

	alp_hw_info_t out;
	memset(&out, 0xA5, sizeof(out));
	alp_hw_info_t before = out;

	const alp_status_t s = alp_hw_info_classify_manifest(&m, &out);

	if (s != ALP_OK) {
		if (memcmp(&out, &before, sizeof(out)) != 0) __builtin_trap();
		return 0;
	}
	if (m.magic != ALP_HW_INFO_MAGIC || m.schema_version != ALP_HW_INFO_SCHEMA_VERSION) {
		__builtin_trap();
	}
	if (m.crc32 != alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32))) {
		__builtin_trap();
	}
	if (memchr(out.som_family, '\0', sizeof(out.som_family)) == NULL ||
	    memchr(out.som_sku, '\0', sizeof(out.som_sku)) == NULL ||
	    memchr(out.som_hw_rev, '\0', sizeof(out.som_hw_rev)) == NULL ||
	    memchr(out.som_serial, '\0', sizeof(out.som_serial)) == NULL) {
		__builtin_trap();
	}
	return 0;
}

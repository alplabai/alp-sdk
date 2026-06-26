/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-eeprom-manifest -- read + decode the 128-byte Alp hardware-info
 * manifest from the on-module 24C128 EEPROM on the E1M-AEN (Alif Ensemble) SoM.
 *
 * The src is the AEN sibling of examples/v2n/v2n-eeprom-manifest-dump -- the
 * read goes through the SoM-portable <alp/...> API, so the only AEN-specific fact
 * is the bus: the EEPROM's interface is selected by bridge/DNP resistors onto
 * the **SoC I2C2** DesignWare master bus (P5_6 SCL_C / P5_7 SDA_C), surfaced as
 * portable bus 0 -- NOT the slave-only LPI2C0 (which carries the RTC/TMP this
 * board rev; a master-capable respin moves those off LPI2C0).  I2C2 is driven by
 * upstream Zephyr's i2c_dw (full master read+write), per ADR 0017 (alp-sdk over
 * the vendor SDK -- Tier-1 upstream-native).
 *
 * Run this after receiving a module from the line: if the manifest is malformed,
 * the firmware build's expected-SKU assertion will halt the boot path later.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/eeprom_24c128.h"
#include "alp/hw_info.h"

/* CRC-32 ISO-3309 (poly 0xEDB88320, init/xor-out 0xFFFFFFFF) -- matches
 * zlib.crc32, the algorithm scripts/program_eeprom.py uses. */
static uint32_t crc32_iso3309(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) {
		crc ^= (uint32_t)buf[i];
		for (unsigned b = 0; b < 8; ++b) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
			crc           = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

/* xxd-style 16-byte-per-line hex dump. */
static void hex_dump(const uint8_t *buf, size_t len)
{
	for (size_t off = 0; off < len; off += 16u) {
		printf("  %04zx  ", off);
		for (size_t i = 0; i < 16u; ++i) {
			if (off + i < len) {
				printf("%02x ", buf[off + i]);
			} else {
				printf("   ");
			}
		}
		printf(" |");
		for (size_t i = 0; i < 16u; ++i) {
			if (off + i < len) {
				uint8_t c = buf[off + i];
				printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
			}
		}
		printf("|\n");
	}
}

int main(void)
{
	printf("[manifest] aen-eeprom-manifest\n");

	/* The 24C128 is on portable bus 0 -> SoC I2C2 (the board overlay aliases
	 * alp-i2c0 to &i2c2 + supplies pinctrl_i2c2 on P5_6/P5_7).  0x50 is the
	 * 24C128's standard 7-bit address. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 0u,
	    .bitrate_hz = 400000u,
	});
	if (bus == NULL) {
		printf("[manifest] alp_i2c_open failed: err=%d\n", (int)alp_last_error());
		return 0;
	}

	eeprom_24c128_t ee;
	alp_status_t    s = eeprom_24c128_init(&ee, bus, 0x50u);
	if (s != ALP_OK) {
		printf("[manifest] eeprom_24c128_init -> %d "
		       "(EEPROM populated?  bridge/DNP selecting I2C2?  bus right?)\n",
		       (int)s);
		alp_i2c_close(bus);
		return 0;
	}

	/* One shot: the EEPROM auto-increments its address pointer, so the
	 * underlying I2C op is a single write-then-read (repeated-START). */
	uint8_t raw[128];
	s = eeprom_24c128_read(&ee, /* offset */ 0x0000u, raw, sizeof(raw));
	if (s != ALP_OK) {
		printf("[manifest] eeprom_24c128_read -> %d (bus error?)\n", (int)s);
		eeprom_24c128_deinit(&ee);
		alp_i2c_close(bus);
		return 0;
	}

	printf("[manifest] raw bytes:\n");
	hex_dump(raw, sizeof(raw));

	const alp_hw_info_eeprom_t *m = (const alp_hw_info_eeprom_t *)raw;

	printf("\n[manifest] magic         = 0x%08x", m->magic);
	if (m->magic == ALP_HW_INFO_MAGIC) {
		printf("  (OK -- ASCII 'ALPH')\n");
	} else {
		printf("  (FAIL -- expected 0x%08x; module not programmed?)\n", ALP_HW_INFO_MAGIC);
	}

	printf("[manifest] schema_version= %u", (unsigned)m->schema_version);
	printf(m->schema_version == ALP_HW_INFO_SCHEMA_VERSION ? "  (OK)\n" : "  (FAIL)\n");

	printf("[manifest] family        = %.*s\n", ALP_HW_INFO_FAMILY_LEN, m->family);
	printf("[manifest] sku           = %.*s\n", ALP_HW_INFO_SKU_LEN, m->sku);
	printf("[manifest] hw_rev        = %.*s\n", ALP_HW_INFO_HW_REV_LEN, m->hw_rev);
	printf("[manifest] serial        = %.*s\n", ALP_HW_INFO_SERIAL_LEN, m->serial);
	printf("[manifest] mfg date      = %04u-%02u-%02u\n",
	       (unsigned)m->mfg_year,
	       (unsigned)m->mfg_month,
	       (unsigned)m->mfg_day);

	const size_t crc_covered_len = sizeof(*m) - sizeof(m->crc32);
	uint32_t     calc            = crc32_iso3309(raw, crc_covered_len);
	printf("[manifest] crc32         = 0x%08x (stored) vs 0x%08x (computed)", m->crc32, calc);
	printf(calc == m->crc32 ? "  (OK)\n" : "  (FAIL -- partial program or corruption)\n");

	/* Production apps call alp_hw_info_read() instead of decoding by hand;
	 * that path resolves the EEPROM bus from the SoM metadata once the
	 * E1M-AEN801 i2c_devices mapping (e1m_i2c0 -> i2c2) lands. */

	eeprom_24c128_deinit(&ee);
	alp_i2c_close(bus);
	printf("[manifest] done\n");
	return 0;
}

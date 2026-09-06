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
 * portable bus 0 -- NOT BRD_I2C (SoC I2C0, which carries the RTC/TMP/OPTIGA
 * instead -- see examples/aen/aen-secure-element-sign).  I2C2 is driven by
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
 * zlib.crc32, the algorithm scripts/program_eeprom.py uses. Table-free
 * bit-at-a-time form: the manifest is <128 bytes, so a lookup table would
 * only add flash footprint without a measurable speed win here. */
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
	    .bitrate_hz = 100000u,
	});
	/* Unlike the AEN Trust M probe (which SKIPs on ALP_ERR_NOT_READY because
	 * OPTIGA population on BRD_I2C varies by assembly), I2C2 backs the manifest
	 * EEPROM that every E1M-AEN801 module ships with populated -- so any
	 * open failure here is a real fault, not an expected-absent-part SKIP. */
	if (bus == NULL) {
		printf("[manifest] alp_i2c_open failed: err=%d\n", (int)alp_last_error());
		printf("RESULT FAIL: alp_i2c_open -> NULL, err=%d\n", (int)alp_last_error());
		return 0;
	}

	eeprom_24c128_t ee;
	alp_status_t    s = eeprom_24c128_init(&ee, bus, 0x50u);
	if (s != ALP_OK) {
		printf("[manifest] eeprom_24c128_init -> %d "
		       "(EEPROM populated?  bridge/DNP selecting I2C2?  bus right?)\n",
		       (int)s);
		printf("RESULT FAIL: eeprom_24c128_init -> %d\n", (int)s);
		alp_i2c_close(bus);
		return 0;
	}

	/* One shot: the EEPROM auto-increments its address pointer, so the
	 * underlying I2C op is a single write-then-read (repeated-START). */
	uint8_t raw[128];
	s = eeprom_24c128_read(&ee, /* offset */ 0x0000u, raw, sizeof(raw));
	if (s != ALP_OK) {
		printf("[manifest] eeprom_24c128_read -> %d (bus error?)\n", (int)s);
		printf("RESULT FAIL: eeprom_24c128_read -> %d\n", (int)s);
		eeprom_24c128_deinit(&ee);
		alp_i2c_close(bus);
		return 0;
	}

	printf("[manifest] raw bytes:\n");
	hex_dump(raw, sizeof(raw));

	/* Reinterpret the raw bytes as the manifest struct rather than copying
	 * field-by-field: alp_hw_info_eeprom_t is a fixed, packed on-wire layout
	 * (the same one scripts/program_eeprom.py writes), so this is safe as
	 * long as that layout and this decode stay in lock-step. */
	const alp_hw_info_eeprom_t *m = (const alp_hw_info_eeprom_t *)raw;

	/* Each field check's OK/FAIL folds into `magic_ok` / `schema_ok` /
	 * `crc_ok` below (crc_ok is set where it's computed, further down) --
	 * those three are what the RESULT verdict at the end of main() gates
	 * on, so a malformed or unprogrammed manifest cannot read as PASS. */
	bool magic_ok = (m->magic == ALP_HW_INFO_MAGIC);
	printf("\n[manifest] magic         = 0x%08x", m->magic);
	if (magic_ok) {
		printf("  (OK -- ASCII 'ALPH')\n");
	} else {
		printf("  (FAIL -- expected 0x%08x; module not programmed?)\n", ALP_HW_INFO_MAGIC);
	}

	bool schema_ok = (m->schema_version == ALP_HW_INFO_SCHEMA_VERSION);
	printf("[manifest] schema_version= %u", (unsigned)m->schema_version);
	printf(schema_ok ? "  (OK)\n" : "  (FAIL)\n");

	printf("[manifest] family        = %.*s\n", ALP_HW_INFO_FAMILY_LEN, m->family);
	printf("[manifest] sku           = %.*s\n", ALP_HW_INFO_SKU_LEN, m->sku);
	printf("[manifest] hw_rev        = %.*s\n", ALP_HW_INFO_HW_REV_LEN, m->hw_rev);
	printf("[manifest] serial        = %.*s\n", ALP_HW_INFO_SERIAL_LEN, m->serial);
	printf("[manifest] mfg date      = %04u-%02u-%02u\n",
	       (unsigned)m->mfg_year,
	       (unsigned)m->mfg_month,
	       (unsigned)m->mfg_day);

	/* The CRC is stored as the manifest's last field and covers every byte
	 * before it -- excluding the CRC field itself, since it can't cover its
	 * own value. A mismatch here means either a partial/interrupted EEPROM
	 * program or a bit-rot/corruption event, distinct from "never programmed"
	 * (which the magic check above already catches). */
	const size_t crc_covered_len = sizeof(*m) - sizeof(m->crc32);
	uint32_t     calc            = crc32_iso3309(raw, crc_covered_len);
	bool         crc_ok          = (calc == m->crc32);
	printf("[manifest] crc32         = 0x%08x (stored) vs 0x%08x (computed)", m->crc32, calc);
	printf(crc_ok ? "  (OK)\n" : "  (FAIL -- partial program or corruption)\n");

	/* --- second device-select header (0x58) --------------------------------
	 * This is the SAME physical N24S128 part answering a second, read-only
	 * identity space -- not a second chip on the bus.  Its `1011` header sits
	 * at ctx's array address (0x50) + EEPROM_24C128_ALT_ADDR_OFFSET (0x08) =
	 * 0x58, and exposes four objects instead of the 16 KB array:
	 * Secure Data Page, Unique ID, Lock Status, and Device Configuration
	 * Register. eeprom_24c128_read_identity() reads all four in one call.
	 *
	 * This example deliberately never calls eeprom_24c128_write() -- here or
	 * anywhere else: a stray write at selector 0x06 lands in the Device
	 * Configuration Register.  Per the N24S128 datasheet Table 9 that
	 * register is `b7 b6 b5 = A2 A1 A0` and `b1 = SWP` -- the device-address
	 * bits are the HIGH three, which is why the bench-measured 0x1D
	 * (0b0001_1101) reads as address 000 with SWP clear.  Writing that
	 * register would move the EEPROM off 0x50, and its SWP bit permanently
	 * write-protects the array, the Secure Data Page, and the register
	 * together -- there is no way back from that on this part, so this
	 * read-only example won't go near it. */
	eeprom_24c128_identity_t id;
	s = eeprom_24c128_read_identity(&ee, &id);
	if (s != ALP_OK) {
		/* Only a NULL out/ctx or an uninitialised ctx return non-OK here -- a
		 * NACK from the silicon itself still returns ALP_OK with the
		 * per-field _valid flags clear (handled below). So landing in this
		 * branch means a programming error in this example, not a hardware
		 * condition, and it is deliberately kept out of the RESULT verdict:
		 * it can't happen without a code bug, unlike the identity objects'
		 * _valid flags below, which legitimately vary by populated part. */
		printf("[manifest] eeprom_24c128_read_identity -> %d (programming error, not "
		       "a hardware condition)\n",
		       (int)s);
	} else {
		printf("\n[manifest] second device-select header (0x58, N24S128 only):\n");

		/* A _valid flag reading false here, with read_identity() itself still
		 * ALP_OK, is NOT a failure -- it is the expected, documented result on
		 * a board populated with the footprint-compatible M24128-BFMH6TG
		 * alternate part, which has no second device-select header to answer
		 * at all. None of the four checks below feed the RESULT verdict at
		 * the end of main(): an absent second header is an absent capability,
		 * not a fault. */
		if (id.secure_page_valid) {
			printf("[manifest]   Secure Data Page (%u bytes, erased = all 0xff):\n",
			       (unsigned)sizeof(id.secure_page));
			hex_dump(id.secure_page, sizeof(id.secure_page));
		} else {
			printf("[manifest]   Secure Data Page        : NACK (expected on the "
			       "M24128-BFMH6TG alternate -- no second header to answer)\n");
		}

		if (id.unique_id_valid) {
			/* Factory-set, but NOT uniform-random: bytes 8..14 are a
			 * printable ASCII lot code (bench-observed "7321261" on the
			 * 2026W36 unit) -- callers must not treat this as an entropy
			 * source. */
			printf("[manifest]   Unique ID (factory-set, not uniform-random -- a "
			       "trailing run is a printable lot code):\n");
			hex_dump(id.unique_id, sizeof(id.unique_id));
		} else {
			printf("[manifest]   Unique ID               : NACK (expected on the "
			       "M24128-BFMH6TG alternate -- no second header to answer)\n");
		}

		if (id.lock_valid) {
			printf("[manifest]   Secure Page Lock Status : %s\n",
			       id.secure_page_locked ? "LOCKED (permanent)" : "UNLOCKED");
		} else {
			printf("[manifest]   Secure Page Lock Status : NACK (expected on the "
			       "M24128-BFMH6TG alternate -- no second header to answer)\n");
		}

		if (id.device_config_valid) {
			printf("[manifest]   Device Configuration Reg: 0x%02x\n", (unsigned)id.device_config);
		} else {
			printf("[manifest]   Device Configuration Reg: NACK (expected on the "
			       "M24128-BFMH6TG alternate -- no second header to answer)\n");
		}
	}

	/* Production apps call alp_hw_info_read() instead of decoding by hand;
	 * that path is enabled by setting CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
	 * >= 0 in prj.conf (0 selects `e1m_i2c0`, agreeing with the EEPROM's
	 * `e1m_i2c0` -> SoC I2C2 placement in
	 * metadata/e1m_modules/E1M-AEN801.yaml's `i2c_devices:` block). This
	 * example leaves the Kconfig at its `-1` (disabled) default and
	 * decodes the manifest by hand instead, so alp_hw_info_read() is not
	 * called above. */

	eeprom_24c128_deinit(&ee);
	alp_i2c_close(bus);
	printf("[manifest] done\n");

	/* RESULT verdict -- gated on the three field checks actually decoded
	 * above, not on merely reaching this line: a torn/unprogrammed EEPROM
	 * still lets the I2C read succeed (I2C doesn't know the payload is
	 * garbage), so only magic_ok/schema_ok/crc_ok tell us the manifest is
	 * real. */
	if (magic_ok && schema_ok && crc_ok) {
		printf("RESULT PASS: manifest magic/schema/crc32 all OK\n");
	} else {
		printf("RESULT FAIL: manifest %s%s%s\n",
		       magic_ok ? "" : "magic mismatch ",
		       schema_ok ? "" : "schema_version mismatch ",
		       crc_ok ? "" : "crc32 mismatch");
	}
	return 0;
}

/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-eeprom-manifest-dump -- hexdump the 128-byte hardware-info
 * manifest at offset 0x0000 of the on-module 24C128 EEPROM and
 * decode every field.
 *
 * Useful during bring-up to confirm the production-test fixture
 * programmed the module correctly.  Run this AFTER you receive a
 * new module from the line and BEFORE you do anything else: if the
 * manifest is malformed, alp_hw_info_read() will fail later and
 * the firmware build's expected-SKU assertion will halt the boot
 * path.
 *
 * The output format is intentionally chatty: every field decoded
 * line-by-line so the source doubles as a tutorial.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/eeprom_24c128.h"
#include "alp/hw_info.h"

/* CRC-32 ISO-3309 (poly 0xEDB88320, init 0xFFFFFFFF, xor-out
 * 0xFFFFFFFF).  Matches zlib.crc32 -- same algorithm the
 * production-test fixture uses. */
static uint32_t crc32_iso3309(const uint8_t *buf, size_t len) {
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

/* xxd-style 16-byte-per-line hex dump.  Useful when you want to
 * compare against the production-fixture's reference binary. */
static void hex_dump(const uint8_t *buf, size_t len) {
    for (size_t off = 0; off < len; off += 16u) {
        printf("  %04zx  ", off);
        for (size_t i = 0; i < 16u; ++i) {
            if (off + i < len) printf("%02x ", buf[off + i]);
            else                printf("   ");
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

int main(void) {
    printf("[manifest] v2n-eeprom-manifest-dump\n");

    /* On V2N the on-module 24C128 EEPROM lives on E1M_I2C0 at
     * 7-bit address 0x50.  Customise the bus_id below for your
     * board overlay. */
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,
        .bitrate_hz = 400000u,
    });
    if (bus == NULL) {
        printf("[manifest] alp_i2c_open failed: err=%d\n",
               (int)alp_last_error());
        return 0;
    }

    eeprom_24c128_t ee;
    alp_status_t    s = eeprom_24c128_init(&ee, bus, 0x50u);
    if (s != ALP_OK) {
        printf("[manifest] eeprom_24c128_init -> %d "
               "(is the EEPROM populated?  Is the bus right?)\n",
               (int)s);
        alp_i2c_close(bus);
        return 0;
    }

    /* Read the full 128-byte manifest in one shot.  The EEPROM
     * auto-increments its internal address pointer, so the
     * underlying I2C call is a single write-then-read transaction. */
    uint8_t raw[128];
    s = eeprom_24c128_read(&ee, /* offset */ 0x0000u, raw, sizeof(raw));
    if (s != ALP_OK) {
        printf("[manifest] eeprom_24c128_read -> %d (bus error?)\n",
               (int)s);
        eeprom_24c128_deinit(&ee);
        alp_i2c_close(bus);
        return 0;
    }

    printf("[manifest] raw bytes:\n");
    hex_dump(raw, sizeof(raw));

    /* Validate the manifest in the same order as
     * src/zephyr/hw_info_zephyr.c does at boot.  Each failure
     * mode prints a diagnostic the production line can act on. */
    const alp_hw_info_eeprom_t *m = (const alp_hw_info_eeprom_t *)raw;

    /* Magic byte: ASCII "ALPH" in little-endian.  An erased EEPROM
     * reads 0xFF or 0x00 across the board; either is unambiguously
     * not the magic. */
    printf("\n[manifest] magic         = 0x%08x", m->magic);
    if (m->magic == ALP_HW_INFO_MAGIC) {
        printf("  (OK -- ASCII 'ALPH')\n");
    } else {
        printf("  (FAIL -- expected 0x%08x.  Production fixture didn't program this module.)\n",
               ALP_HW_INFO_MAGIC);
    }

    /* Schema version: 1 today.  Future schema bumps may reclaim
     * the reserved[] bytes; readers MUST refuse a schema they
     * don't understand. */
    printf("[manifest] schema_version= %u", (unsigned)m->schema_version);
    if (m->schema_version == ALP_HW_INFO_SCHEMA_VERSION) {
        printf("  (OK)\n");
    } else {
        printf("  (FAIL -- this firmware understands schema %u)\n",
               (unsigned)ALP_HW_INFO_SCHEMA_VERSION);
    }

    printf("[manifest] family        = %.*s\n",
           ALP_HW_INFO_FAMILY_LEN, m->family);
    printf("[manifest] sku           = %.*s\n",
           ALP_HW_INFO_SKU_LEN, m->sku);
    printf("[manifest] hw_rev        = %.*s\n",
           ALP_HW_INFO_HW_REV_LEN, m->hw_rev);
    printf("[manifest] serial        = %.*s\n",
           ALP_HW_INFO_SERIAL_LEN, m->serial);
    printf("[manifest] mfg date      = %04u-%02u-%02u\n",
           (unsigned)m->mfg_year,
           (unsigned)m->mfg_month,
           (unsigned)m->mfg_day);

    /* CRC covers bytes [0x00, 0x7C) -- everything except the CRC
     * field itself.  Mismatch indicates the manifest was partially
     * programmed or corrupted in transit. */
    const size_t crc_covered_len = sizeof(*m) - sizeof(m->crc32);
    uint32_t     calc            = crc32_iso3309(raw, crc_covered_len);
    printf("[manifest] crc32         = 0x%08x (stored)  vs 0x%08x (computed)",
           m->crc32, calc);
    if (calc == m->crc32) {
        printf("  (OK)\n");
    } else {
        printf("  (FAIL -- partial program or corruption)\n");
    }

    /* Also exercise the production reader so the customer sees the
     * exact same status they'd get from their app. */
    alp_hw_info_t info;
    s = alp_hw_info_read(&info);
    printf("\n[manifest] alp_hw_info_read() -> status=%d\n", (int)s);
    if (s == ALP_OK) {
        printf("[manifest]   som_family=%s som_sku=%s som_hw_rev=%s som_serial=%s\n",
               info.som_family, info.som_sku, info.som_hw_rev, info.som_serial);
    }

    eeprom_24c128_deinit(&ee);
    alp_i2c_close(bus);
    printf("[manifest] done\n");
    return 0;
}

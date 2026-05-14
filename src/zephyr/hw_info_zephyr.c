/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-backed implementation of <alp/hw_info.h>.
 *
 * EEPROM-side reader: opens the configured I2C bus, reads the
 * 128-byte manifest via the 24C128 driver, validates magic +
 * schema_version + CRC32, and copies the SoM identifiers out.
 *
 * BOARD_ID ADC cross-check is TODO pending the per-family generated
 * header that maps `hw_rev` strings to expected mV bins -- those
 * tables live in metadata/e1m_modules/<family>/hw-revisions.yaml
 * today and need a `scripts/alp_project.py` pass to emit the
 * runtime-readable form.  Once available, plug into the
 * adc_cross_check() helper below.
 */

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"

#if defined(CONFIG_ALP_SDK_CHIP_EEPROM_24C128)
#include "alp/chips/eeprom_24c128.h"
#endif

/* ---------------------------------------------------------------- */
/* CRC32 / ISO-3309 (polynomial 0xEDB88320; init 0xFFFFFFFF;          */
/*   xor-out 0xFFFFFFFF).  Matches Python's `zlib.crc32` so the       */
/*   production-test programmer at scripts/program_eeprom.py and the    */
/*   runtime here cannot disagree on the manifest's checksum.        */
/* ---------------------------------------------------------------- */

__attribute__((unused)) static uint32_t crc32_iso3309(const uint8_t *buf, size_t len)
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

/* ---------------------------------------------------------------- */
/* Bounded string copy that always null-terminates the destination.   */
/* The EEPROM manifest fields are spec'd as null-terminated but a     */
/* corrupt or partially-programmed EEPROM might omit the terminator.  */
/* ---------------------------------------------------------------- */
__attribute__((unused)) static void copy_field(char *dst, size_t dst_len, const char *src,
                                               size_t src_len)
{
    size_t n = src_len;
    if (n >= dst_len) n = dst_len - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
    /* If the source carried an embedded NUL, terminate there for
     * tidiness even though memcpy above already covered the bytes. */
    for (size_t i = 0; i < n; ++i) {
        if (dst[i] == '\0') return;
    }
}

#if defined(CONFIG_ALP_SDK_HW_INFO) && \
    defined(CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID) && \
    (CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID >= 0) && \
    defined(CONFIG_ALP_SDK_CHIP_EEPROM_24C128)
#define ALP_HW_INFO_EEPROM_ENABLED 1
#else
#define ALP_HW_INFO_EEPROM_ENABLED 0
#endif

#if ALP_HW_INFO_EEPROM_ENABLED

/* Open the bus, init the EEPROM driver, read 128 bytes at the
 * configured offset.  On any failure: returns the underlying status
 * without populating @p out beyond the zero-fill performed by the
 * caller. */
static alp_status_t read_manifest(alp_hw_info_eeprom_t *manifest)
{
    alp_i2c_config_t cfg = {
        .bus_id     = (uint32_t)CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID,
        .bitrate_hz = (uint32_t)CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BITRATE_HZ,
    };
    alp_i2c_t *bus = alp_i2c_open(&cfg);
    if (bus == NULL) return alp_last_error();

    eeprom_24c128_t ee;
    alp_status_t    s = eeprom_24c128_init(&ee, bus, (uint8_t)CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT);
    if (s != ALP_OK) { alp_i2c_close(bus); return s; }

    s = eeprom_24c128_read(&ee, (uint16_t)CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET,
                           (uint8_t *)manifest, sizeof(*manifest));

    eeprom_24c128_deinit(&ee);
    alp_i2c_close(bus);
    return s;
}

#endif /* ALP_HW_INFO_EEPROM_ENABLED */

/* TODO(hw_info ADC cross-check): once scripts/alp_project.py emits
 * a per-board generated header listing expected_mv / bin_radius_mv
 * per hw_rev, call alp_adc_open on the SoM BOARD_ID channel here,
 * sample, look up the rev, and cross-check against
 * manifest->hw_rev.  Mismatch -> ALP_ERR_IO with manifest left as
 * read (caller can log for diagnostics). */
__attribute__((unused)) static alp_status_t adc_cross_check(const alp_hw_info_eeprom_t *manifest,
                                                            alp_hw_info_t              *out)
{
    (void)manifest;
    (void)out;
    return ALP_OK; /* No-op until the generated header lands. */
}

alp_status_t alp_hw_info_read(alp_hw_info_t *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    memset(out, 0, sizeof(*out));

#if !ALP_HW_INFO_EEPROM_ENABLED
    /* No EEPROM bus configured -- return NOSUPPORT.  Callers wanting
     * to run on a board without the hw_info path enabled should
     * configure ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID >= 0 in their
     * prj.conf, or accept the NOSUPPORT return and continue. */
    return ALP_ERR_NOSUPPORT;
#else
    alp_hw_info_eeprom_t manifest;
    memset(&manifest, 0, sizeof(manifest));

    alp_status_t s = read_manifest(&manifest);
    if (s != ALP_OK) return s == ALP_ERR_NOSUPPORT ? ALP_ERR_NOT_READY : s;

    if (manifest.magic != ALP_HW_INFO_MAGIC) return ALP_ERR_IO;
    if (manifest.schema_version != ALP_HW_INFO_SCHEMA_VERSION) return ALP_ERR_IO;

    /* CRC covers every byte from `magic` (offset 0) through the
     * final byte of `reserved[]` (offset 124).  The CRC field
     * itself sits at offset 124..127 and is excluded. */
    const size_t crc_covered_len = sizeof(manifest) - sizeof(manifest.crc32);
    uint32_t     calc_crc        = crc32_iso3309((const uint8_t *)&manifest, crc_covered_len);
    if (calc_crc != manifest.crc32) return ALP_ERR_IO;

    /* Populate the SoM half of alp_hw_info_t.  The manifest's
     * char-array fields are bounded by the chip-side sizes (see
     * ALP_HW_INFO_FAMILY_LEN etc. in <alp/hw_info.h>); copy with a
     * NUL guarantee. */
    copy_field(out->som_family, sizeof(out->som_family),
               manifest.family, sizeof(manifest.family));
    copy_field(out->som_sku, sizeof(out->som_sku),
               manifest.sku, sizeof(manifest.sku));
    copy_field(out->som_hw_rev, sizeof(out->som_hw_rev),
               manifest.hw_rev, sizeof(manifest.hw_rev));
    copy_field(out->som_serial, sizeof(out->som_serial),
               manifest.serial, sizeof(manifest.serial));
    out->som_mfg_year  = manifest.mfg_year;
    out->som_mfg_month = manifest.mfg_month;
    out->som_mfg_day   = manifest.mfg_day;

    /* SoM BOARD_ID ADC sample + cross-check.  No-op stub for now;
     * see comment on adc_cross_check() above. */
    s = adc_cross_check(&manifest, out);
    if (s != ALP_OK) return s;

    /* TODO: carrier-side BOARD_ID + carrier_name once the per-carrier
     * board.yaml -> generated-header pipeline lands. */
    return ALP_OK;
#endif
}

alp_status_t alp_hw_info_assert_matches_build(const alp_hw_info_t *info, const char *expected_sku,
                                              const char *expected_hw_rev)
{
    if (info == NULL) return ALP_ERR_INVAL;

#if !ALP_HW_INFO_EEPROM_ENABLED
    (void)expected_sku;
    (void)expected_hw_rev;
    return ALP_ERR_NOSUPPORT;
#else
    /* NULL expected_* means "don't check this field".  Empty strings
     * in info-> imply alp_hw_info_read() wasn't called or returned
     * early; that's treated as a mismatch. */
    if (expected_sku != NULL) {
        if (info->som_sku[0] == '\0') return ALP_ERR_IO;
        if (strncmp(info->som_sku, expected_sku, sizeof(info->som_sku)) != 0) return ALP_ERR_IO;
    }
    if (expected_hw_rev != NULL) {
        if (info->som_hw_rev[0] == '\0') return ALP_ERR_IO;
        if (strncmp(info->som_hw_rev, expected_hw_rev, sizeof(info->som_hw_rev)) != 0) return ALP_ERR_IO;
    }
    return ALP_OK;
#endif
}

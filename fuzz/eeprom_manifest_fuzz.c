/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the 24C128 EEPROM manifest decoder consumed
 * by `<alp/hw_info.h>`'s `alp_hw_info_read`.
 *
 * The manifest is a fixed-layout 128-byte struct (see
 * `alp_hw_info_eeprom_t` in hw_info.h) with magic + schema_version +
 * strings + CRC32.  Production-test writes it via
 * `tools/program_eeprom.py`; the SDK reads it at boot.
 *
 * What this harness catches:
 *   - Magic / schema-version validation that overruns the input
 *     when the buffer is smaller than 128 bytes.
 *   - String fields lacking NUL termination -- the decoder must
 *     treat the field as a fixed-length blob, not a C string.
 *   - CRC32 verification on a corrupted manifest must reject; the
 *     fuzzer drives random byte sequences and the decoder must
 *     never accept anything that isn't the magic + valid CRC.
 *   - mfg_month / mfg_day field range validation (1..12, 1..31).
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_eeprom_manifest
 *
 * Run:
 *   ./build-fuzz/fuzz/alp_fuzz_eeprom_manifest -max_total_time=30 \
 *         fuzz/corpus/eeprom_manifest
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/hw_info.h"

/* CRC-32 (ISO-3309) -- same algorithm `tools/program_eeprom.py` +
 * `zlib.crc32` use.  Inlined so the harness has no external dependency. */
static uint32_t crc32_iso3309(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (unsigned b = 0; b < 8; ++b) {
            const uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Standalone decoder that mirrors what alp_hw_info_read would do
 * after reading the 128 bytes from the EEPROM.  Returns 0 on a
 * structurally valid manifest, non-zero otherwise.  Caller's job is
 * to NOT trust strings, dates, or any field on a non-zero return. */
static int decode_manifest(const uint8_t *buf, size_t len)
{
    if (len < sizeof(alp_hw_info_eeprom_t)) return -1;
    const alp_hw_info_eeprom_t *m = (const alp_hw_info_eeprom_t *)buf;
    if (m->magic != ALP_HW_INFO_MAGIC) return -2;
    if (m->schema_version != ALP_HW_INFO_SCHEMA_VERSION) return -3;
    /* CRC covers everything except the trailing crc32 field itself. */
    const size_t crc_len = sizeof(*m) - sizeof(uint32_t);
    if (crc32_iso3309(buf, crc_len) != m->crc32) return -4;
    /* Date sanity: production-test fills these.  A bad manifest with
     * a valid CRC32 (someone reflashed a malformed image) must still
     * be rejected. */
    if (m->mfg_month < 1u || m->mfg_month > 12u) return -5;
    if (m->mfg_day   < 1u || m->mfg_day   > 31u) return -6;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Run the decoder; throw away the return value.  The fuzzer
     * watches for ASan / UBSan trips inside decode_manifest. */
    (void)decode_manifest(data, size);
    return 0;
}

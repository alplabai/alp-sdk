/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-backed implementation of <alp/hw_info.h>.
 *
 * v0.3 ships the API contract only; both entry points return
 * ALP_ERR_NOSUPPORT.  The runtime impl lands in v0.3.x once the
 * per-family BOARD_ID ADC channels + resistor values are filled
 * in by the user-supplied HW writeups (TBD in
 * metadata/e1m_modules/<family>/hw-revisions.yaml) and the
 * production-test EEPROM programming flow is wired.
 *
 * The intended v0.3.x flow:
 *
 *   1. alp_i2c_open(<som EEPROM bus>) + 24c128 read at offset 0
 *      using chips/eeprom_24c128.  Parse into alp_hw_info_eeprom_t.
 *      Verify magic + schema_version + CRC32.
 *   2. alp_adc_open(<som BOARD_ID channel>) and sample to mV.
 *      Look up the rev row in the family hw-revisions table
 *      (baked in at compile time via a v0.3.x-emitted header).
 *      Compare to the EEPROM's hw_rev string; mismatch -> IO err.
 *   3. Same for the carrier BOARD_ID channel when declared.
 */

#include <stddef.h>
#include <string.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"

alp_status_t alp_hw_info_read(alp_hw_info_t *out)
{
    if (out == NULL) return ALP_ERR_INVAL;
    /* Zero the struct so callers see deterministic defaults
     * even when we return NOSUPPORT. */
    memset(out, 0, sizeof(*out));
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hw_info_assert_matches_build(const alp_hw_info_t *info, const char *expected_sku,
                                              const char *expected_hw_rev)
{
    if (info == NULL) return ALP_ERR_INVAL;
    (void)expected_sku;
    (void)expected_hw_rev;
    return ALP_ERR_NOSUPPORT;
}

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- PSA-FWU seam implementation (issue
 * #1123).  Thin pass-through from cc3501e_hw_ti_ota_psa.h's plain-C
 * prototypes to TI's <ti/utils/FWU/psa_fwu.h>.  Built ONLY for
 * CC3501E_HAL_BACKEND=ti (the bench build), picked up automatically by
 * ti/build_ti.sh's `hal/ti/cc3501e_hw_ti_*.c` glob.  See
 * cc3501e_hw_ti_ota_psa.h for why this split exists (host-testing the
 * abort-vs-FINISH state machine in cc3501e_hw_ti_ota.c without the
 * vendored SimpleLink SDK).
 *
 * Every psa_fwu_* return here collapses PSA_SUCCESS / PSA_SUCCESS_REBOOT
 * into `true` -- cc3501e_hw_ti_ota.c never distinguished them either (both
 * are documented as success; PSA_SUCCESS_REBOOT just additionally means
 * "and a reboot completes the swap", which the state machine already
 * handles by arming the separate ota_reboot_pending latch).
 */

#include <stdbool.h>
#include <stdint.h>

#include <ti/utils/FWU/psa_fwu.h> /* PSA Firmware Update: stream + install the vendor image */

#include "cc3501e_hw_ti_ota_psa.h"

static psa_fwu_component_t slot_component(uint8_t slot)
{
	return (slot == CC3501E_OTA_PSA_SLOT_1) ? (psa_fwu_component_t)Vendor_Image_Slot_1
	                                        : (psa_fwu_component_t)Vendor_Image_Slot_2;
}

void cc3501e_ota_psa_init(void)
{
	psa_fwu_init(); /* idempotent */
}

uint32_t cc3501e_ota_psa_manifest_size(void)
{
	return (uint32_t)TI_FWU_MANIFEST_SIZE;
}

bool cc3501e_ota_psa_query_primary(uint8_t slot, bool *out_primary)
{
	psa_fwu_component_info_t info = { 0 };
	if (psa_fwu_query(slot_component(slot), &info) != PSA_SUCCESS) {
		return false;
	}
	*out_primary = info.impl.Primary;
	return true;
}

bool cc3501e_ota_psa_cancel(uint8_t slot)
{
	return psa_fwu_cancel(slot_component(slot)) == PSA_SUCCESS;
}

bool cc3501e_ota_psa_reject(void)
{
	return psa_fwu_reject(PSA_ERROR_GENERIC_ERROR) == PSA_SUCCESS;
}

bool cc3501e_ota_psa_clean(uint8_t slot)
{
	return psa_fwu_clean(slot_component(slot)) == PSA_SUCCESS;
}

bool cc3501e_ota_psa_start(uint8_t slot, const uint8_t *manifest, uint32_t manifest_len)
{
	return psa_fwu_start(slot_component(slot), manifest, manifest_len) == PSA_SUCCESS;
}

bool cc3501e_ota_psa_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint32_t len)
{
	return psa_fwu_write(slot_component(slot), offset, data, len) == PSA_SUCCESS;
}

bool cc3501e_ota_psa_finish(uint8_t slot)
{
	const psa_status_t pf = psa_fwu_finish(slot_component(slot));
	return pf == PSA_SUCCESS || pf == PSA_SUCCESS_REBOOT;
}

bool cc3501e_ota_psa_install(void)
{
	const psa_status_t pi = psa_fwu_install();
	return pi == PSA_SUCCESS || pi == PSA_SUCCESS_REBOOT;
}

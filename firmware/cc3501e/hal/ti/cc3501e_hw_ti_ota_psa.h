/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge OTA -- PSA-FWU HW seam (issue #1123).
 *
 * cc3501e_hw_ti_ota.c's abort-vs-FINISH state machine used to call TI's
 * <ti/utils/FWU/psa_fwu.h> directly, which pulls in the vendored SimpleLink
 * CC33xx SDK (including psa_fwu_component_info_t) -- so the state machine
 * could only ever be exercised on the bench, never on host.  This is the
 * same seam gd32-bridge's OTA has used from day one
 * (firmware/gd32-bridge/src/fmc_ota.h): plain-C-typed wrapper prototypes the
 * state machine calls instead of the vendor API directly.
 *
 *   - cc3501e_hw_ti_ota_psa.c: the real ti-backend implementation, a thin
 *     pass-through to psa_fwu_* (built ONLY for CC3501E_HAL_BACKEND=ti --
 *     picked up automatically by ti/build_ti.sh's `hal/ti/cc3501e_hw_ti_*.c`
 *     glob, same as every other split TU in this directory).
 *   - tests/unit/cc3501e_ota_abort_race/src/test_ota_abort_race.c: a host
 *     double that redirects every call into an in-memory model, so the REAL
 *     cc3501e_hw_ti_ota.c state machine (abort/pump/begin/finish) links and
 *     runs on native_sim without the vendor SDK.
 *
 * Exactly one implementation is ever linked into a given target (the real
 * ti backend, or the test double) -- unlike transport.h's
 * bridge_transport_spi_hw_reinit() (a __weak__ generic no-op alongside the
 * ti override coexist in the SAME stub-vs-ti link), so these prototypes
 * carry no default body and need no __weak__ annotation.
 */
#ifndef CC3501E_HAL_TI_CC3501E_HW_TI_OTA_PSA_H
#define CC3501E_HAL_TI_CC3501E_HW_TI_OTA_PSA_H

#include <stdbool.h>
#include <stdint.h>

/* The two vendor OTA slots (Vendor_Image_Slot_1/2). A bare index keeps the
 * vendor psa_fwu_component_t enum out of the seam -- the real implementation
 * maps it back before calling psa_fwu_*. */
#define CC3501E_OTA_PSA_SLOT_1 0u
#define CC3501E_OTA_PSA_SLOT_2 1u

/* One-time PSA-FWU init; idempotent, cannot fail (mirrors psa_fwu_init()). */
void cc3501e_ota_psa_init(void);

/* Manifest header size (vendor TI_FWU_MANIFEST_SIZE). The FINISH state
 * machine needs this as a real value (image-too-small check, WRITE start
 * offset), so it is a call rather than a magic number duplicated at the
 * seam boundary -- only cc3501e_hw_ti_ota_psa.c's real body needs to agree
 * with the vendor header. */
uint32_t cc3501e_ota_psa_manifest_size(void);

/* True + *out_primary on success; false on a query failure
 * (-> CC3501E_HW_ERR_IO upstream). */
bool cc3501e_ota_psa_query_primary(uint8_t slot, bool *out_primary);

/* Stuck-state walk-back (see ota_do_begin() / ota_finish_step()'s START
 * phase): each no-ops when the slot isn't in the matching PSA-FWU state --
 * callers ignore the return the same way the original psa_fwu_cancel/
 * reject/clean call sites did. */
bool cc3501e_ota_psa_cancel(uint8_t slot);
bool cc3501e_ota_psa_reject(void);
bool cc3501e_ota_psa_clean(uint8_t slot);

/* FINISH's flash burst -- start the manifest, write one block, finalize,
 * install.  See ota_finish_step().  Only cc3501e_ota_psa_finish() and
 * cc3501e_ota_psa_install() can return PSA_SUCCESS_REBOOT from the vendor
 * API; both collapse it into `true` alongside plain PSA_SUCCESS (never
 * distinguished elsewhere in this file).  start()/write() only ever see
 * PSA_SUCCESS on success. */
bool cc3501e_ota_psa_start(uint8_t slot, const uint8_t *manifest, uint32_t manifest_len);
bool cc3501e_ota_psa_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint32_t len);
bool cc3501e_ota_psa_finish(uint8_t slot);
bool cc3501e_ota_psa_install(void);

#endif /* CC3501E_HAL_TI_CC3501E_HW_TI_OTA_PSA_H */

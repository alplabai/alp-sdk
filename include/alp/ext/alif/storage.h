/**
 * @file ext/alif/storage.h
 * @brief Alif Ensemble OSPI SecAES vendor-specific surface.
 *
 * Non-portable.  Include only when you've committed to Alif
 * silicon for the gated feature.  Every function in this header
 * verifies the handle's backend is Alif before touching hardware;
 * calls on a non-Alif handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers the SecAES block that the Ensemble E4 / E6 / E8 OSPI
 * controllers expose.  Key provisioning binds an AES key to a
 * hardware slot so the key material never traces back to RAM
 * after the call returns.  Status read-back lets callers verify
 * the engine engaged before relying on encrypted XIP.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *      (E3 / E5 / E7 do not expose the SecAES fabric; vendor
 *      packs may extend this list in a follow-up release.)
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Header lands ahead of the vendor pack body; every function
 *      returns @ref ALP_ERR_NOSUPPORT until Alif HAL integration
 *      lands.  Promotes to [ABI-STABLE] when three vendor families
 *      ship extensions.
 */

#ifndef ALP_EXT_ALIF_STORAGE_H
#define ALP_EXT_ALIF_STORAGE_H

#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_ALIF_STORAGE_AVAILABLE 1

/** OSPI SecAES engine runtime status flags. */
typedef enum {
	ALP_ALIF_STORAGE_SECAES_STATUS_IDLE         = 0u,
	ALP_ALIF_STORAGE_SECAES_STATUS_ARMED        = 1u << 0,
	ALP_ALIF_STORAGE_SECAES_STATUS_ENGAGED      = 1u << 1,
	ALP_ALIF_STORAGE_SECAES_STATUS_KEY_LOAD_ERR = 1u << 2,
	ALP_ALIF_STORAGE_SECAES_STATUS_BUS_ERR      = 1u << 3,
} alp_alif_storage_secaes_status_t;

/**
 * @brief Provision an inline-AES key for the OSPI SecAES engine.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *
 * Binds the supplied key into a hardware slot.  After the call
 * returns the host RAM no longer holds the key -- callers may
 * (and should) zeroise the source buffer.  The cipher mode +
 * IV / tweak passed via @ref alp_storage_configure_inline_aes
 * are honoured by the same hardware slot.
 *
 * @param[in] s       Storage handle from @ref alp_storage_open
 *                    opened against Alif silicon.
 * @param[in] key     Key bytes.  Must be non-NULL.
 * @param[in] key_bytes  16, 24, or 32 -- selects AES-128 / 192 / 256.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC if s was opened on
 *               non-Alif silicon.
 *          @ref ALP_ERR_INVAL on NULL key or wrong key_bytes.
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands.
 *          @ref ALP_ERR_IO on hardware bus / key-load fault.
 */
alp_status_t
alp_alif_storage_secaes_key_provision(alp_storage_t *s, const uint8_t *key, uint8_t key_bytes);

/**
 * @brief Read the SecAES engine status flags.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *
 * Lets callers verify the engine engaged before relying on
 * encrypted XIP execution -- useful in secure-boot flows where
 * the application must abort if the encryption fabric is not
 * confirmed live.
 *
 * @param[in]  s         Storage handle from @ref alp_storage_open
 *                       opened against Alif silicon.
 * @param[out] status_out  Receives the OR'd flag mask from
 *                         @ref alp_alif_storage_secaes_status_t.
 *                         Must be non-NULL.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (NULL status_out) /
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands.
 */
alp_status_t alp_alif_storage_secaes_get_status(alp_storage_t *s, uint32_t *status_out);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_ALIF_STORAGE_H */

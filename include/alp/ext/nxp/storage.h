/**
 * @file ext/nxp/storage.h
 * @brief NXP FlexSPI OTFAD vendor-specific surface.
 *
 * Non-portable.  Include only when you've committed to NXP
 * silicon for the gated feature.  Every function in this header
 * verifies the handle's backend is NXP before touching hardware;
 * calls on a non-NXP handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers the On-The-Fly AES Decryption (OTFAD) block that the
 * i.MX 93 and RT11xx FlexSPI controllers expose.  Up to four
 * independent address windows can be live concurrently, each
 * bound to its own key + counter; pick a window via the
 * @c window_id argument.
 *
 * @par Supported silicon: nxp:imx9:imx93, nxp:rt11xx:rt1170
 *      (Other FlexSPI-OTFAD parts -- RT10xx, RT106x, RT117x --
 *      may be added in a follow-up release once tested.)
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Header lands ahead of the vendor pack body; every function
 *      returns @ref ALP_ERR_NOSUPPORT until MCUXpresso FlexSPI
 *      integration lands.  Promotes to [ABI-STABLE] when three
 *      vendor families ship extensions.
 */

#ifndef ALP_EXT_NXP_STORAGE_H
#define ALP_EXT_NXP_STORAGE_H

#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_NXP_STORAGE_AVAILABLE 1

/** OTFAD supports up to four concurrent address windows on i.MX 93
 *  / RT11xx; @c window_id selects which slot is being programmed. */
#define ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT 4u

/** Forward-declared enum so the ABI snapshot parser sees a
 *  semicolon-terminated statement between the macro above and the
 *  first function declaration below. */
typedef enum {
	ALP_NXP_STORAGE_OTFAD_SLOT_0 = 0, /**< OTFAD address-window slot 0. */
	ALP_NXP_STORAGE_OTFAD_SLOT_1 = 1, /**< OTFAD address-window slot 1. */
	ALP_NXP_STORAGE_OTFAD_SLOT_2 = 2, /**< OTFAD address-window slot 2. */
	ALP_NXP_STORAGE_OTFAD_SLOT_3 = 3, /**< OTFAD address-window slot 3. */
} alp_nxp_storage_otfad_slot_t;

/**
 * @brief Provision an OTFAD key + counter for a FlexSPI address window.
 *
 * @par Supported silicon: nxp:imx9:imx93, nxp:rt11xx:rt1170
 *
 * Binds the supplied key + counter / IV into a hardware OTFAD
 * window slot.  After the call returns the host RAM no longer
 * holds the key.  @ref alp_nxp_storage_otfad_set_window then
 * defines the address range the slot decrypts.
 *
 * @param[in] s          Storage handle from @ref alp_storage_open
 *                       opened against NXP silicon.
 * @param[in] window_id  Slot index; must be < @ref ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT.
 * @param[in] key        Key bytes.  Must be non-NULL and 16 bytes
 *                       (OTFAD is AES-128-CTR fixed).
 * @param[in] counter    Initial counter / IV bytes.  Must be non-NULL
 *                       and 16 bytes.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (bad window_id or NULL key / counter) /
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands /
 *          @ref ALP_ERR_IO on hardware bus / slot-program fault.
 */
alp_status_t alp_nxp_storage_otfad_provision(alp_storage_t *s,
                                             uint8_t        window_id,
                                             const uint8_t *key,
                                             const uint8_t *counter);

/**
 * @brief Define the FlexSPI address window that an OTFAD slot decrypts.
 *
 * @par Supported silicon: nxp:imx9:imx93, nxp:rt11xx:rt1170
 *
 * Address bounds are byte offsets from the FlexSPI base; both
 * MUST be 1 KiB-aligned (OTFAD's hardware granularity).
 * Misaligned bounds reject with ALP_ERR_INVAL rather than
 * silently rounding.
 *
 * @param[in] s          Storage handle from @ref alp_storage_open
 *                       opened against NXP silicon.
 * @param[in] window_id  Slot index; must be < @ref ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT.
 * @param[in] start_addr Window start address; 1 KiB-aligned.
 * @param[in] end_addr   Window end address (inclusive); 1 KiB-aligned.
 *                       end_addr MUST be > start_addr.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (bad window_id, misaligned bounds,
 *          end <= start) / @ref ALP_ERR_NOSUPPORT until the vendor
 *          pack body lands.
 */
alp_status_t alp_nxp_storage_otfad_set_window(alp_storage_t *s,
                                              uint8_t        window_id,
                                              uint32_t       start_addr,
                                              uint32_t       end_addr);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_NXP_STORAGE_H */

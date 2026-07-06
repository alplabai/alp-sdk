/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file storage.h
 * @brief Alp SDK persistent-storage abstraction (QSPI / OSPI / SD).
 *
 * v0.4 deliverable.  v0.1 ships the public surface as a stub;
 * every entry point returns ALP_ERR_NOSUPPORT and `*_open` returns
 * NULL.  Apps that target `<alp/storage.h>` can compile today and
 * the implementations slot in per-OS:
 *
 *   - **Zephyr**   : `flash_*` driver class for QSPI/OSPI; `disk_access_*`
 *                    for SD/MMC.  Lands v0.4 alongside the V2N + i.MX 93
 *                    Yocto first-class work (the same Yocto cycle bumps
 *                    Zephyr's own flash story).
 *   - **Yocto**    : `/dev/mtd*` for raw flash; `/dev/mmcblk*p*` for SD.
 *   - **Baremetal**: Vendor HAL flash drivers (Alif HAL OSPI, Renesas
 *                    FSP QSPI, NXP MCUXpresso flash).
 *
 * The shape is deliberately small — the SDK's storage surface is
 * "block-oriented read / write / erase," not a filesystem.  Apps
 * stack ZephyrFS, LittleFS, or ext4 on top per their own choice.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 added alp_storage_configure_inline_aes (SecAES on OSPI / HexSPI) -- surface tentative.  Base storage placeholders are still stubs.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_STORAGE_H
#define ALP_STORAGE_H

#include <stdint.h>
#include <stddef.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Storage class.  Selects which backend the open routes to. */
typedef enum {
	ALP_STORAGE_KIND_INTERNAL_FLASH = 0, /**< On-die flash / MRAM. */
	ALP_STORAGE_KIND_QSPI_FLASH     = 1, /**< External QSPI NOR. */
	ALP_STORAGE_KIND_OSPI_FLASH     = 2, /**< External OSPI NOR / HyperBus. */
	ALP_STORAGE_KIND_SD_MMC         = 3  /**< SD card or eMMC. */
} alp_storage_kind_t;

typedef struct alp_storage alp_storage_t;

typedef struct {
	alp_storage_kind_t kind;
	uint32_t           instance_id; /**< 0 for the primary device. */
	uint32_t           freq_hz;     /**< Bus clock; 0 = backend default. */
	bool               read_only;   /**< Refuses writes / erases. */
} alp_storage_config_t;

/** Block geometry, populated by @ref alp_storage_get_info. */
typedef struct {
	uint64_t total_bytes;
	uint32_t block_size; /**< Min unit for read/write. */
	uint32_t erase_size; /**< Min unit for erase (often a multiple of block_size). */
} alp_storage_info_t;

/**
 * @brief Acquire a storage handle.
 *
 * @param[in] cfg  Configuration; @c kind selects the backend.
 * @return Open handle, or NULL with `alp_last_error()` set to the
 *         specific failure reason.
 */
alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg);

/**
 * @brief Get geometry + total size for the device.
 *
 * @param[in]  storage  Handle from @ref alp_storage_open.
 * @param[out] info     Receives total_bytes / block_size / erase_size.
 *                      Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_storage_get_info(alp_storage_t *storage, alp_storage_info_t *info);

/**
 * @brief Read @p len bytes starting at @p offset into @p data.
 *
 * @p offset and @p len need not align to block_size for backends
 * that read-modify-write internally; for raw flash, callers should
 * align both to the device's block_size.
 *
 * @param[in]  storage  Handle from @ref alp_storage_open.
 * @param[in]  offset   Byte offset from device start.
 * @param[out] data     Destination buffer.  Must be non-NULL when
 *                      @p len > 0.
 * @param[in]  len      Number of bytes to read.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_OUT_OF_RANGE (offset + len past device end) /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_storage_read(alp_storage_t *storage, uint64_t offset, void *data, size_t len);

/**
 * @brief Write @p len bytes from @p data starting at @p offset.
 *
 * NOR flash requires the target region to be erased first; SD/MMC
 * and the Yocto backend handle this transparently, so app code that
 * doesn't care about the underlying medium can ignore the
 * write-after-erase rule.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 * @param[in] offset   Byte offset from device start.
 * @param[in] data     Source buffer.  Must be non-NULL when
 *                     @p len > 0.
 * @param[in] len      Number of bytes to write.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY (handle is
 *         read_only or device not present) / ALP_ERR_OUT_OF_RANGE
 *         (offset + len past device end) / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_IO.
 */
alp_status_t
alp_storage_write(alp_storage_t *storage, uint64_t offset, const void *data, size_t len);

/**
 * @brief Erase the region [@p offset, @p offset + @p len).
 *
 * Both bounds MUST align to the device's @c erase_size (from
 * @ref alp_storage_get_info); misaligned bounds reject with
 * ALP_ERR_INVAL rather than partially erasing.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 * @param[in] offset   Byte offset from device start; @c erase_size-aligned.
 * @param[in] len      Region length; @c erase_size-aligned.
 *
 * @return ALP_OK / ALP_ERR_INVAL (alignment or read_only) /
 *         ALP_ERR_NOT_READY / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_storage_erase(alp_storage_t *storage, uint64_t offset, uint64_t len);

/**
 * @brief Flush any backend-side write cache to media.
 *
 * Implicit on @ref alp_storage_close; callers needing
 * "write-then-power-off" durability should sync first.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT /
 *         ALP_ERR_IO.
 */
alp_status_t alp_storage_sync(alp_storage_t *storage);

/**
 * @brief Release the handle.  Implicitly syncs.
 *
 * NULL is a no-op.  After this call @p storage is invalid.
 *
 * @param[in] storage  Handle from @ref alp_storage_open, or NULL.
 */
void alp_storage_close(alp_storage_t *storage);

/**
 * @brief Query the capabilities of an opened storage handle.
 *
 * @param storage  Handle from @ref alp_storage_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p storage is NULL.
 */
const alp_capabilities_t *alp_storage_capabilities(const alp_storage_t *storage);

/* ================================================================== */
/* Inline AES (on-the-fly XIP encryption / decryption)                 */
/*                                                                     */
/* Wave-2 audit (internal AEN feature audit, §4.3) flagged       */
/* on-the-fly inline AES for external flash as a NEEDS-PORTABLE-       */
/* SURFACE gap.  AEN-family OSPI / HexSPI controllers can transparently */
/* encrypt + decrypt data between the host bus and the external chip   */
/* (XIP code stays AES-protected; the host sees plaintext, the flash   */
/* sees ciphertext).  Customers migrating from V2N to AEN otherwise    */
/* lose the secure-XIP capability silently.                            */
/*                                                                     */
/* The minimal v0.5 surface declared below covers the cipher mode +    */
/* key + IV needed to set up inline-AES on the open device.  Re-       */
/* keying mid-session is supported by calling configure_inline_aes()  */
/* again; passing mode == ALP_STORAGE_AES_OFF disables it.            */
/* ================================================================== */

/** Inline-AES cipher mode.  Backends honour the subset their HW
 *  supports; unsupported modes return ALP_ERR_NOSUPPORT.
 *  Field-level meanings:
 *   - OFF: bypass (plaintext both ways).
 *   - CTR: AES-CTR (sequential streaming; IV is the starting counter).
 *   - XTS: AES-XTS (block-cipher mode; IV is the tweak.  Standard
 *     for storage encryption at flash-block granularity). */
typedef enum {
	ALP_STORAGE_AES_OFF = 0,
	ALP_STORAGE_AES_CTR = 1,
	ALP_STORAGE_AES_XTS = 2,
} alp_storage_aes_mode_t;

/** Inline-AES configuration.  Caller-owned memory; backend reads
 *  the key / IV at configure() time and may bind to a HW key slot
 *  (the HW key never traces back to RAM after that point).
 *  Field-level meanings:
 *   - mode: one of @ref alp_storage_aes_mode_t.
 *   - key: pointer to the key bytes (length per @c key_bytes).
 *   - key_bytes: 16, 24, or 32 -- selects AES-128 / 192 / 256.
 *   - iv: pointer to the IV / tweak bytes (length per @c iv_bytes).
 *     16 bytes for both CTR + XTS in the standard modes.
 *   - iv_bytes: typically 16. */
typedef struct {
	alp_storage_aes_mode_t mode;
	const uint8_t         *key;
	uint8_t                key_bytes;
	const uint8_t         *iv;
	uint8_t                iv_bytes;
	uint16_t               reserved;
} alp_storage_aes_config_t;

/**
 * @brief Configure on-the-fly inline AES for an open storage device.
 *
 * Backends with an inline-AES capable controller (AEN-family
 * OSPI / HexSPI with the SecAES block; future i.MX 93 FlexSPI
 * with the OTFAD module) program the controller's key / IV
 * registers and enable the inline path before this function
 * returns.  Subsequent @ref alp_storage_read / @ref alp_storage_write
 * calls transparently encrypt + decrypt; XIP code execution
 * benefits without additional API changes.
 *
 * Calling with @c cfg->mode == @ref ALP_STORAGE_AES_OFF disables
 * inline AES.  Re-keying mid-session is supported by calling
 * again with a new key / IV.  The key material is read only
 * during the call -- the caller may free / zeroise immediately
 * on return.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 * @param[in] cfg      Configuration.  Must be non-NULL.  When
 *                     mode != OFF, key + iv MUST be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL (bad
 *         mode / NULL key when mode != OFF / invalid key_bytes) /
 *         ALP_ERR_NOSUPPORT (backend lacks an inline-AES path --
 *         V2N today, AEN-family E3 / E5 / E7 silicon without the
 *         optional SecAES fabric) / ALP_ERR_IO.
 */
alp_status_t alp_storage_configure_inline_aes(alp_storage_t                  *storage,
                                              const alp_storage_aes_config_t *cfg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STORAGE_H */

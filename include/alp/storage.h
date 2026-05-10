/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file storage.h
 * @brief ALP SDK persistent-storage abstraction (QSPI / OSPI / SD).
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
 */

#ifndef ALP_STORAGE_H
#define ALP_STORAGE_H

#include <stdint.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Storage class.  Selects which backend the open routes to. */
typedef enum {
    ALP_STORAGE_KIND_INTERNAL_FLASH = 0,    /**< On-die flash / MRAM. */
    ALP_STORAGE_KIND_QSPI_FLASH     = 1,    /**< External QSPI NOR. */
    ALP_STORAGE_KIND_OSPI_FLASH     = 2,    /**< External OSPI NOR / HyperBus. */
    ALP_STORAGE_KIND_SD_MMC         = 3     /**< SD card or eMMC. */
} alp_storage_kind_t;

typedef struct alp_storage alp_storage_t;

typedef struct {
    alp_storage_kind_t kind;
    uint32_t           instance_id;     /**< 0 for the primary device. */
    uint32_t           freq_hz;         /**< Bus clock; 0 = backend default. */
    bool               read_only;       /**< Refuses writes / erases. */
} alp_storage_config_t;

/** Block geometry, populated by @ref alp_storage_get_info. */
typedef struct {
    uint64_t total_bytes;
    uint32_t block_size;       /**< Min unit for read/write. */
    uint32_t erase_size;       /**< Min unit for erase (often a multiple of block_size). */
} alp_storage_info_t;

/**
 * @brief Acquire a storage handle.
 *
 * @param[in] cfg  Configuration; @c kind selects the backend.
 * @return Open handle, or NULL with `alp_last_error()` set to the
 *         specific failure reason.
 */
alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg);

/** Get geometry + total size for the device. */
alp_status_t   alp_storage_get_info(alp_storage_t *s, alp_storage_info_t *info);

/**
 * @brief Read @p len bytes starting at @p offset into @p data.
 *
 * @p offset and @p len need not align to block_size for backends
 * that read-modify-write internally; for raw flash, callers should
 * align both to the device's block_size.
 */
alp_status_t   alp_storage_read(alp_storage_t *s,
                                uint64_t offset,
                                void *data, size_t len);

/**
 * @brief Write @p len bytes from @p data starting at @p offset.
 *
 * NOR flash requires the target region to be erased first; SD/MMC
 * and the Yocto backend handle this transparently, so app code that
 * doesn't care about the underlying medium can ignore the
 * write-after-erase rule.
 */
alp_status_t   alp_storage_write(alp_storage_t *s,
                                 uint64_t offset,
                                 const void *data, size_t len);

/** Erase the region [@p offset, @p offset + @p len).  Both must
 *  align to the device's `erase_size`. */
alp_status_t   alp_storage_erase(alp_storage_t *s,
                                 uint64_t offset, uint64_t len);

/** Flush any backend-side write cache to media. */
alp_status_t   alp_storage_sync(alp_storage_t *s);

/** Release the handle.  Implicitly syncs. */
void           alp_storage_close(alp_storage_t *s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_STORAGE_H */

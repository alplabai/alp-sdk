/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file storage.h
 * @brief Alp SDK persistent-storage abstraction (QSPI / OSPI / SD).
 *
 * Backends registered on Zephyr:
 *   - **zephyr_flash** (silicon_ref `"*"`, priority 100): real
 *     `<zephyr/storage/flash_map.h>` `flash_area_*` backing for
 *     INTERNAL_FLASH/QSPI_FLASH/OSPI_FLASH (`kind`); `instance_id`
 *     maps to a flash-area ID.  Returns ALP_ERR_NOSUPPORT for
 *     `kind == ALP_STORAGE_KIND_SD_MMC` (no flash_area backing for
 *     SD/MMC; `alp_storage_open` fails outright -- there is no
 *     fallback to a second backend) and for `configure_inline_aes`
 *     (no inline-AES path on plain Zephyr flash -- a higher-priority
 *     vendor backend implements it once one lands).
 *   - **zephyr_littlefs** (silicon_ref `"*"`, priority 90): real
 *     littlefs-file-backed device
 *     (`/lfs<instance_id>/alp_storage.bin`) for
 *     INTERNAL_FLASH/QSPI_FLASH/OSPI_FLASH; erase is emulated by
 *     overwriting the region with 0xFF.  Same SD_MMC /
 *     `configure_inline_aes` ALP_ERR_NOSUPPORT posture as
 *     zephyr_flash.
 *   - **sw_fallback** (priority 0): every op past open/close
 *     returns ALP_ERR_NOSUPPORT; wins only when neither Zephyr
 *     backend links (e.g. a trimmed `native_sim` image).
 *   - **alp_testing** (file `testing_drv.c`; priority 255,
 *     `CONFIG_ALP_SDK_TESTING_STORAGE`): fault-injectable test
 *     double for host/unit tests -- not present in a shipping
 *     build.
 *   - **yocto_drv** (silicon_ref `"*"`, priority 100, vendor
 *     `linux`, #1140): real backend -- `/dev/mmcblk<instance_id>`
 *     (BLKGETSIZE64/BLKSSZGET geometry, pread/pwrite I/O, BLKDISCARD
 *     as the closest thing to erase) for `ALP_STORAGE_KIND_SD_MMC`;
 *     `/dev/mtd<instance_id>` (MEMGETINFO geometry, pread/pwrite I/O,
 *     MEMERASE for a real NOR erase) for
 *     INTERNAL_FLASH/QSPI_FLASH/OSPI_FLASH.
 *     `alp_storage_write`/`alp_storage_erase` refuse with
 *     ALP_ERR_INVAL unless `alp_storage_config_t.allow_unsafe_write`
 *     is explicitly set at open() -- `/dev/mmcblk<N>` is the
 *     WHOLE-DISK node and, on V2N, `/dev/mtd0` is itself the
 *     FIP/bootloader partition; see that field's doxygen.
 *     `configure_inline_aes` is ALP_ERR_NOSUPPORT (no Linux
 *     userspace ABI programs inline XIP-AES key/IV).  See
 *     `src/backends/storage/yocto_drv.c`.
 *   - **Baremetal**: no backend has landed yet -- the class is an
 *     unconditional stub (`alp_storage_open` returns NULL, every
 *     other call returns ALP_ERR_NOSUPPORT).
 *
 * The shape is deliberately small — the SDK's storage surface is
 * "block-oriented read / write / erase," not a filesystem.  Apps
 * stack ZephyrFS, LittleFS, or ext4 on top per their own choice.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 added alp_storage_configure_inline_aes (SecAES on OSPI / HexSPI) -- surface tentative, ALP_ERR_NOSUPPORT until a vendor pack implements it.  Base read/write/erase are real on Zephyr (zephyr_flash / zephyr_littlefs).
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
	/**
	 * Must be explicitly set `true` to permit @ref alp_storage_write /
	 * @ref alp_storage_erase on the Yocto backend (ignored elsewhere).
	 * `instance_id` alone does not tell that backend whether it is
	 * addressing user data or the boot medium: `/dev/mmcblk<N>` is the
	 * WHOLE-DISK node (GPT/MBR + bootloader at offset 0), and on some
	 * boards `/dev/mtd0` IS the bootloader/FIP partition.  Left `false`
	 * (the default from @ref ALP_STORAGE_CONFIG_DEFAULT),
	 * `alp_storage_write`/`alp_storage_erase` return `ALP_ERR_INVAL`
	 * before touching the device, on every instance_id, no exceptions.
	 * `alp_storage_read`/`alp_storage_get_info` are unaffected -- a
	 * read cannot damage the boot medium.
	 */
	bool allow_unsafe_write;
} alp_storage_config_t;

/**
 * @brief Default-initialize an @ref alp_storage_config_t for storage
 *        class @p id.
 *
 * Identity from @p id (the @c kind -- there is no universally-safe
 * kind to default to across INTERNAL_FLASH / QSPI / OSPI / SD_MMC, so
 * the caller must always name one); canonical defaults for the rest:
 * @c instance_id = 0 (documented as "the primary device"), @c freq_hz
 * = 0 (documented as "backend default"), @c read_only = false (the
 * common writable case), @c allow_unsafe_write = false (the SAFE
 * default -- see that field's doxygen; a caller using this macro
 * verbatim cannot write or erase through the Yocto backend).
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_STORAGE_CONFIG_DEFAULT(id) \
	((alp_storage_config_t){ .kind               = (id), \
	                         .instance_id        = 0u, \
	                         .freq_hz            = 0u, \
	                         .read_only          = false, \
	                         .allow_unsafe_write = false })

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
 * @return Open handle, or NULL with `alp_last_error()` set to one of
 *         ALP_ERR_INVAL (NULL cfg), ALP_ERR_NOT_PRESENT_ON_THIS_SOC
 *         (no backend registered for the active silicon),
 *         ALP_ERR_NOT_IMPLEMENTED (registered backend has no open
 *         hook), ALP_ERR_NOMEM (handle-pool or backend-state
 *         allocation failure), ALP_ERR_NOSUPPORT (@c kind ==
 *         ALP_STORAGE_KIND_SD_MMC -- neither zephyr_flash nor
 *         zephyr_littlefs backs SD/MMC today), or a backend I/O
 *         failure (ALP_ERR_NOT_READY / ALP_ERR_BUSY /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO / ALP_ERR_OUT_OF_RANGE)
 *         translated from the underlying flash-area / littlefs
 *         error.
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
 * NOR flash requires the target region to be erased first.  SD/MMC's
 * block layer handles this transparently, so app code targeting
 * SD/MMC can ignore the write-after-erase rule -- but NOT every
 * backend does: the Yocto backend's MTD (NOR) path issues a raw
 * write with no read-erase-modify-write, so a NOR target on Yocto
 * still needs an explicit @ref alp_storage_erase first, exactly like
 * bare-metal NOR.  Writing over an unerased NOR region here silently
 * only clears bits and still returns ALP_OK -- this is real NOR
 * physics, not a bug, so verify erase state yourself for NOR targets.
 *
 * @par Yocto safety gate
 *      Returns ALP_ERR_INVAL immediately, before touching the device,
 *      unless @ref alp_storage_config_t.allow_unsafe_write was set
 *      `true` at @ref alp_storage_open -- see that field's doxygen.
 *      `ALP_STORAGE_CONFIG_DEFAULT` leaves it `false`.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 * @param[in] offset   Byte offset from device start.
 * @param[in] data     Source buffer.  Must be non-NULL when
 *                     @p len > 0.
 * @param[in] len      Number of bytes to write.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (device not present, or the handle
 *         was opened @c read_only) / ALP_ERR_INVAL (the Yocto safety gate
 *         above) / ALP_ERR_OUT_OF_RANGE (offset + len past device end) /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
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
 * @par Post-erase state is medium-specific
 *      A true NOR erase (MTD/MEMERASE) leaves the region reading as
 *      all-0xFF.  The Yocto backend's SD/MMC path has no NOR-style
 *      erase primitive on the generic block layer; it issues
 *      BLKDISCARD, a best-effort TRIM hint that the range is no
 *      longer needed.  BLKDISCARD is NOT guaranteed to make the
 *      region read back as zeroed or any other fixed pattern -- do
 *      not assume 0xFF (or 0x00) after erase() on an SD/MMC target;
 *      only NOR targets get that guarantee.
 *
 * @par Yocto safety gate
 *      Returns ALP_ERR_INVAL immediately, before touching the device,
 *      unless @ref alp_storage_config_t.allow_unsafe_write was set
 *      `true` at @ref alp_storage_open -- see that field's doxygen.
 *      `ALP_STORAGE_CONFIG_DEFAULT` leaves it `false`.
 *
 * @param[in] storage  Handle from @ref alp_storage_open.
 * @param[in] offset   Byte offset from device start; @c erase_size-aligned.
 * @param[in] len      Region length; @c erase_size-aligned.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (device not present, or the handle
 *         was opened @c read_only) / ALP_ERR_INVAL (alignment, or the
 *         Yocto safety gate above) / ALP_ERR_OUT_OF_RANGE /
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
/* AEN-family OSPI / HexSPI controllers can transparently */
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

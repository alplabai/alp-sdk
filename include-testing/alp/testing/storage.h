/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file storage.h
 * @brief Injection API for the Storage test double (priority-255 backend).
 *
 * `src/backends/storage/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_STORAGE=y` it wins @ref alp_backend_select
 * for every storage instance id and the portable `<alp/storage.h>`
 * `alp_storage_*` API rides on it transparently -- no real Zephyr
 * flash_area / littlefs backend needed. This header is the test-side
 * control surface: it models a small fixed-capacity backing device --
 * a real, byte-addressable RAM buffer a test can read/write/erase
 * through the portable API, plus injectable capacity limits, region
 * corruption, one-shot op faults, and mid-write power loss.
 *
 * Every function keys off the same @p storage_id the app passes as
 * `alp_storage_config_t.instance_id` to @ref alp_storage_open. All
 * injectors are create-on-first-touch -- a test may set a capacity,
 * mark a region corrupt, arm a fault, or arm a power-loss cut BEFORE
 * the app ever opens the device, so power-on / pre-corrupted-media
 * scenarios are expressible. @ref alp_testing_storage_read_back is the
 * exception: it is a pure lookup against a @p storage_id that has been
 * touched at least once (fails @ref ALP_ERR_INVAL otherwise) -- there
 * is nothing yet to read back.
 *
 * @par The backing-store model.
 *      Each @p storage_id gets its own fixed
 *      `ALP_TESTING_STORAGE_BACKING_BYTES` (see `testing_drv.c`)
 *      RAM buffer, zero-initialised. @ref alp_storage_get_info reports
 *      `total_bytes` = the LOGICAL capacity set via
 *      @ref alp_testing_storage_set_capacity (defaulting to the full
 *      backing size), `block_size` = 1 (byte-addressable read/write),
 *      and `erase_size` = the double's fixed NOR-like erase granule --
 *      @ref alp_storage_erase requires both bounds aligned to it,
 *      exactly as `<alp/storage.h>` documents. A read/write/erase
 *      whose range falls outside the logical capacity fails
 *      @ref ALP_ERR_OUT_OF_RANGE, matching `alp_storage_read` /
 *      `alp_storage_write` / `alp_storage_erase`'s documented
 *      "offset + len past device end" case. The buffer PERSISTS across
 *      @ref alp_storage_close / re-open on the same @p storage_id
 *      (mirrors every other `alp/testing` double leaving its
 *      side-state intact across a close/re-open) -- modelling
 *      non-volatile media, not RAM that a real close would discard.
 *      Only @ref alp_testing_reset_all wipes it (back to all-zero, full
 *      capacity, no corruption, no armed faults).
 *
 * @par The corruption model.
 *      @ref alp_testing_storage_inject_corruption marks a byte range
 *      "corrupt" in a per-@p storage_id bitmap, independent of the
 *      backing bytes underneath it (which are left untouched -- this
 *      models a media defect / ECC failure the device DETECTS on
 *      readback, not bit-flipped data). A subsequent
 *      @ref alp_storage_read that overlaps ANY corrupt byte fails
 *      @ref ALP_ERR_IO without copying any data. A byte's corrupt mark
 *      is cleared the moment it is next written (@ref alp_storage_write,
 *      including the surviving prefix of a power-loss-torn write) or
 *      erased (@ref alp_storage_erase) -- "until overwritten", exactly
 *      as fresh media replacing a bad block would behave. Marking a
 *      region does not require @p storage_id to have a logical capacity
 *      covering it; the mark is bounds-checked against the double's
 *      fixed physical backing size instead, so a test may pre-corrupt
 *      a region before raising the capacity to cover it.
 *
 * @par Power loss (the storage-specific must).
 *      @ref alp_testing_storage_inject_power_loss_after arms the NEXT
 *      @ref alp_storage_write on @p storage_id to persist only the
 *      first `bytes_written` bytes of its payload -- clearing any
 *      corruption mark on exactly that persisted prefix -- and then
 *      return @ref ALP_ERR_IO instead of @ref ALP_OK, leaving the
 *      remainder of the target range holding whatever was there
 *      before the call (a genuine TORN write: a real NOR/eMMC write
 *      that loses power mid-program does not roll back the bytes
 *      already latched). @ref alp_testing_storage_read_back -- which
 *      bypasses corruption marks and never consumes a one-shot fault,
 *      reading the raw persisted bytes exactly as a bench flash-dump
 *      tool would -- is how a test proves the torn shape: read back
 *      `[offset, offset + bytes_written)` and it matches the payload's
 *      prefix; read back the rest and it does NOT match the payload's
 *      tail. This is the scenario a real app's journaling / power-fail
 *      recovery logic must survive.
 *
 * @note `alp_storage_open()` on this double ALWAYS succeeds (a
 *       deliberate ergonomic choice, mirroring the GPIO/UART/I2C/SPI/
 *       ADC/CAN doubles -- see `testing_drv.c`'s open()), so an app
 *       under test that opens a genuinely-invalid instance id still
 *       gets back a live handle: this double cannot catch a
 *       wrong-instance application bug. Proving open() rejects an
 *       invalid instance is the real backend's conformance job, not
 *       this one's.
 *
 * @note @ref alp_storage_configure_inline_aes is NOT modelled by this
 *       double (`testing_drv.c`'s op always returns
 *       @ref ALP_ERR_NOSUPPORT, matching every non-vendor-extension
 *       storage backend, including `sw_fallback.c`) -- there is no
 *       injection API for it here.
 */

#ifndef ALP_TESTING_STORAGE_H
#define ALP_TESTING_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Selects which storage op @ref alp_testing_storage_fail_next
 *        targets.
 *
 * `<alp/storage.h>` documents `open`/`get_info`/`configure_inline_aes`
 * with their own independent failure contracts (`get_info` and
 * `configure_inline_aes` are not gated behind a caller payload the way
 * read/write/erase/sync are), so this selector -- and the one-shot
 * fault it arms -- covers exactly the four data-path ops.
 */
typedef enum {
	ALP_TESTING_STORAGE_OP_READ = 0, /**< @ref alp_storage_read. */
	ALP_TESTING_STORAGE_OP_WRITE,    /**< @ref alp_storage_write. */
	ALP_TESTING_STORAGE_OP_ERASE,    /**< @ref alp_storage_erase. */
	ALP_TESTING_STORAGE_OP_SYNC,     /**< @ref alp_storage_sync. */
} alp_testing_storage_op_t;

/**
 * @brief Set the simulated device size @p storage_id reports via
 *        @ref alp_storage_get_info and enforces on every
 *        read/write/erase range.
 *
 * @param[in] storage_id  The same id the app passes as
 *                          `alp_storage_config_t.instance_id` to
 *                          @ref alp_storage_open.
 * @param[in] bytes       New logical capacity, in bytes. Shrinking
 *                          capacity below previously-written data does
 *                          not erase or lose that data -- it simply
 *                          becomes unreachable through the portable API
 *                          until capacity is raised again (still visible
 *                          via @ref alp_testing_storage_read_back, which
 *                          is bounded only by the double's fixed
 *                          physical backing size, not this logical
 *                          capacity).
 *
 * @return ALP_OK on success; ALP_ERR_NOMEM if @p bytes exceeds the
 *         double's fixed physical backing size (see
 *         `ALP_TESTING_STORAGE_BACKING_BYTES` in `testing_drv.c`).
 */
alp_status_t alp_testing_storage_set_capacity(uint32_t storage_id, uint64_t bytes);

/**
 * @brief Read back what is ACTUALLY persisted for @p storage_id at
 *        `[offset, offset + len)`, bypassing injected read faults and
 *        corruption marks.
 *
 * This is the test-side "flash dump" tool: unlike @ref alp_storage_read
 * (which the double's `read()` op honours corruption marks and an
 * armed @ref ALP_TESTING_STORAGE_OP_READ fault on), this call always
 * copies the raw backing bytes -- exactly what a bench programmer
 * reading the physical device would see -- so a test can assert the
 * TORN shape a power-loss-interrupted write leaves behind (see the
 * power-loss section on this file's header comment).
 *
 * @param[in]  storage_id  The same id the app passes as
 *                           `alp_storage_config_t.instance_id` to
 *                           @ref alp_storage_open.
 * @param[in]  offset      Byte offset from device start.
 * @param[out] out         Destination buffer. Must be non-NULL if
 *                           @p len > 0.
 * @param[in]  len         Number of bytes to read.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p out is NULL with
 *         @p len > 0, or @p storage_id has never been touched (no
 *         open, no injection); ALP_ERR_OUT_OF_RANGE if
 *         `[offset, offset + len)` exceeds the double's fixed physical
 *         backing size.
 */
alp_status_t
alp_testing_storage_read_back(uint32_t storage_id, uint64_t offset, uint8_t *out, size_t len);

/**
 * @brief Mark `[offset, offset + len)` on @p storage_id corrupt: the
 *        next @ref alp_storage_read overlapping ANY byte in the range
 *        fails @ref ALP_ERR_IO, until that byte is next written or
 *        erased.
 *
 * The mark is per-byte and does not touch the backing bytes underneath
 * it (see the corruption model on this file's header comment) --
 * @ref alp_testing_storage_read_back always returns the real, unmarked
 * data.
 *
 * @param[in] storage_id  The same id the app passes as
 *                          `alp_storage_config_t.instance_id` to
 *                          @ref alp_storage_open.
 * @param[in] offset      Byte offset from device start.
 * @param[in] len         Region length, in bytes.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if `[offset, offset + len)`
 *         exceeds the double's fixed physical backing size.
 */
alp_status_t
alp_testing_storage_inject_corruption(uint32_t storage_id, uint64_t offset, uint64_t len);

/**
 * @brief Arm a one-shot fault for the NEXT @p op invoked on
 *        @p storage_id.
 *
 * Fires exactly once -- on the next call to the op named by @p op --
 * then disarms itself automatically; a call to a DIFFERENT op does not
 * consume it. Checked before the double's own capacity / corruption /
 * power-loss logic for that op, so an armed fault always wins.
 *
 * @param[in] storage_id  The same id the app passes as
 *                          `alp_storage_config_t.instance_id` to
 *                          @ref alp_storage_open.
 * @param[in] op          Which op the fault targets.
 * @param[in] err         Status the targeted op returns. Not validated
 *                          against `<alp/storage.h>`'s documented set
 *                          for that op -- a test may inject any
 *                          @ref alp_status_t.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p op is not a valid
 *         @ref alp_testing_storage_op_t.
 */
alp_status_t
alp_testing_storage_fail_next(uint32_t storage_id, alp_testing_storage_op_t op, alp_status_t err);

/**
 * @brief Arm the NEXT @ref alp_storage_write on @p storage_id to persist
 *        only its first @p bytes_written bytes, then fail with the
 *        documented power-loss / I/O status -- a torn write.
 *
 * Fires exactly once, then disarms itself automatically. Takes priority
 * over a region's corruption marks (the persisted prefix's marks are
 * cleared, exactly as a normal write would) but yields to an armed
 * @ref alp_testing_storage_fail_next(@p storage_id, ALP_TESTING_STORAGE_OP_WRITE, ...)
 * -- an explicitly-armed fault always fires first and leaves this cut
 * still armed for the write after it.
 *
 * @param[in] storage_id     The same id the app passes as
 *                             `alp_storage_config_t.instance_id` to
 *                             @ref alp_storage_open.
 * @param[in] bytes_written  Bytes of the NEXT write's payload to
 *                             actually persist before the simulated
 *                             power loss. Clamped to the write's own
 *                             `len` at fire time -- arming a value
 *                             `>=` a future write's `len` persists the
 *                             whole payload (a non-torn "crashed right
 *                             after the last byte landed" case) while
 *                             still returning the power-loss status.
 *
 * @return ALP_OK on success.
 */
alp_status_t alp_testing_storage_inject_power_loss_after(uint32_t storage_id,
                                                         uint64_t bytes_written);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_STORAGE_H */

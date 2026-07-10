/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storage test double.  Ordinary alp_storage_ops_t backend registered
 * at the reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_STORAGE=y it outranks every real/proxy/
 * fallback storage backend (zephyr_flash=100, zephyr_littlefs=90,
 * sw_fallback=0) and alp_storage_open() rides on it transparently --
 * see the priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * open()      -- ALWAYS succeeds.  Same deliberate ergonomic choice as
 *                 the GPIO/UART/I2C/SPI/ADC/CAN doubles: a "*"
 *                 silicon_ref double has no valid-instance-range
 *                 knowledge to reject a storage_id with, and the
 *                 injection API (<alp/testing/storage.h>) needs
 *                 inject-before-open to work.  kind/read_only/
 *                 instance_id are already stamped onto `state` by the
 *                 dispatcher (src/storage_dispatch.c) before this op
 *                 runs, mirroring zephyr_flash.c's z_open().
 * get_info()  -- reports the injected logical capacity (default: the
 *                 full physical backing size) plus this double's fixed
 *                 block_size (1 -- byte-addressable) / erase_size
 *                 (NOR-like granule) geometry.
 * read()      -- STORAGE-SPECIFIC MUST #1 (issue #610 §2 design note):
 *                 range-checks against the logical capacity first
 *                 (ALP_ERR_OUT_OF_RANGE), then an armed one-shot READ
 *                 fault, then the per-byte corruption bitmap
 *                 (ALP_ERR_IO on any overlap, buffer zeroed) -- only
 *                 then copies from the backing buffer.  Never touches
 *                 alp_testing_storage_read_back()'s view of the data.
 * write()     -- STORAGE-SPECIFIC MUST #2, the power-loss model: range
 *                 check, then an armed one-shot WRITE fault, then an
 *                 armed power-loss cut (persists only the first
 *                 bytes_written bytes of the payload, clears THEIR
 *                 corruption marks, returns ALP_ERR_IO -- a torn
 *                 write), else the full write (clearing the whole
 *                 range's corruption marks).
 * erase()     -- range check, erase_size alignment check
 *                 (ALP_ERR_INVAL, per <alp/storage.h>'s documented
 *                 erase() contract), an armed one-shot ERASE fault,
 *                 else fills the range with 0xFF (NOR-erased-state
 *                 convention) and clears its corruption marks.
 * sync()      -- an armed one-shot SYNC fault, else a no-op ALP_OK
 *                 (the backing buffer has no write-behind cache to
 *                 flush).
 * configure_inline_aes() -- always ALP_ERR_NOSUPPORT; this double does
 *                 not model inline crypto (matches sw_fallback.c and
 *                 every non-vendor-extension storage backend).
 * close()     -- detaches the handle from its slot (be_data = NULL);
 *                 the backing buffer / capacity / corruption marks /
 *                 armed faults are NOT cleared by close() -- this
 *                 double models NON-VOLATILE media, so a close/re-open
 *                 on the same storage_id must observe what was there
 *                 before (mirrors every other alp/testing double
 *                 leaving its side-state intact across close, taken
 *                 one step further here: persistence IS the point of
 *                 this class).  Only alp_testing_reset_all() clears it.
 *
 * The injection API (<alp/testing/storage.h>) and the ops table share
 * the same per-device slot via the generic instance table
 * (src/testing/instance_table.h), keyed by storage_id -- exactly
 * alp_storage_config_t.instance_id, the id the app passes to
 * alp_storage_open() -- so a test can set a capacity, pre-corrupt a
 * region, or arm a fault/power-loss cut before the app ever opens the
 * device.
 *
 * @par Cost: ROM ~2 KB; RAM = capacity * sizeof(slot), where each slot
 *      is ALP_TESTING_STORAGE_BACKING_BYTES (backing buffer) +
 *      ALP_TESTING_STORAGE_BACKING_BYTES/8 (corruption bitmap) + a
 *      handful of scalar fields -- test-only, never linked into a
 *      production image (gated by CONFIG_ALP_SDK_TESTING, itself gated
 *      on CONFIG_ZTEST).  Defaults (4 KiB backing, 4 devices) keep the
 *      total well under 20 KiB, trivial for a native_sim host process.
 * @par Performance: O(capacity) per call (linear slot scan) plus
 *      O(len) per read/write/erase/corruption-mark (bitmap walk /
 *      memcpy/memset over the touched range); fine for the small
 *      payloads and single fixed-size device a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/storage.h>
#include <alp/testing/storage.h>

#include "instance_table.h"
#include "reset_registry.h"
#include "storage_ops.h"

#ifndef ALP_TESTING_STORAGE_MAX_DEVICES
#define ALP_TESTING_STORAGE_MAX_DEVICES 4
#endif

/* 4 KiB: enough for a test to exercise multi-block reads/writes/erases
 * and a torn power-loss write, small enough that MAX_DEVICES copies
 * cost nothing on a native_sim host process (see the ROM/RAM note
 * above). */
#ifndef ALP_TESTING_STORAGE_BACKING_BYTES
#define ALP_TESTING_STORAGE_BACKING_BYTES 4096u
#endif

/* Byte-addressable: this double does not model a minimum program
 * granule (real Zephyr flash_area backends may; zephyr_flash.c
 * defaults to 1 too when the underlying device API is opaque). */
#ifndef ALP_TESTING_STORAGE_BLOCK_SIZE
#define ALP_TESTING_STORAGE_BLOCK_SIZE 1u
#endif

/* NOR-like erase granule.  Divides ALP_TESTING_STORAGE_BACKING_BYTES
 * evenly (16 blocks at the defaults) so a test exercising the full
 * device never has to reason about a ragged last block. */
#ifndef ALP_TESTING_STORAGE_ERASE_SIZE
#define ALP_TESTING_STORAGE_ERASE_SIZE 256u
#endif

typedef struct {
	uint8_t  backing[ALP_TESTING_STORAGE_BACKING_BYTES];
	uint8_t  corrupt_bitmap[ALP_TESTING_STORAGE_BACKING_BYTES / 8u];
	uint64_t capacity; /* logical size reported by get_info(); <= BACKING_BYTES */

	bool                     fail_armed;
	alp_testing_storage_op_t fail_op;
	alp_status_t             fail_err;

	bool     power_loss_armed;
	uint64_t power_loss_bytes_written;
} alp_testing_storage_slot_t;

static alp_testing_storage_slot_t   g_slots[ALP_TESTING_STORAGE_MAX_DEVICES];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_STORAGE_MAX_DEVICES];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state
 * EXCEPT capacity -- a zeroed capacity would reject every
 * read/write/erase with ALP_ERR_OUT_OF_RANGE, so the instance table's
 * memset (done before this runs, same contract as the GPIO double's
 * slot_reset) is followed by explicitly defaulting capacity to the
 * full physical backing size. */
static void slot_reset(void *slot_v)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)slot_v;
	slot->capacity                   = ALP_TESTING_STORAGE_BACKING_BYTES;
}

/* Reset hook (mirrors src/backends/adc/testing_drv.c's adc_reset_hook):
 * every device's backing buffer / corruption bitmap / armed faults
 * live entirely inside this table, so
 * alp_testing_instance_table_reset_all() (run unconditionally by
 * alp_testing_reset_all()) already clears them on its own -- this hook
 * is a defensive, explicit re-assertion of that contract, so a future
 * change to this double's state shape that steps outside the table has
 * somewhere to go without a silent reset gap (see the
 * reset-frees-side-state regression in
 * tests/zephyr/conformance/src/behavior_storage.c). */
static void storage_reset_hook(void)
{
	alp_testing_instance_table_reset(&g_table);
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(&g_table,
		                                g_slots,
		                                g_hdrs,
		                                sizeof(g_slots[0]),
		                                ALP_TESTING_STORAGE_MAX_DEVICES,
		                                slot_reset);
		alp_testing_register_reset_hook(storage_reset_hook);
		g_table_ready = true;
	}
	return &g_table;
}

/* ---- corruption bitmap helpers (one bit per backing byte) ---- */

static void bitmap_mark(uint8_t *bm, uint64_t offset, uint64_t len, bool corrupt)
{
	for (uint64_t i = 0; i < len; ++i) {
		uint64_t byte_idx = (offset + i) / 8u;
		uint8_t  bit      = (uint8_t)(1u << ((offset + i) % 8u));
		if (corrupt) {
			bm[byte_idx] |= bit;
		} else {
			bm[byte_idx] &= (uint8_t)~bit;
		}
	}
}

static bool bitmap_any_set(const uint8_t *bm, uint64_t offset, uint64_t len)
{
	for (uint64_t i = 0; i < len; ++i) {
		uint64_t byte_idx = (offset + i) / 8u;
		uint8_t  bit      = (uint8_t)(1u << ((offset + i) % 8u));
		if ((bm[byte_idx] & bit) != 0u) return true;
	}
	return false;
}

/* ---- injection API (<alp/testing/storage.h>) ---- */

alp_status_t alp_testing_storage_set_capacity(uint32_t storage_id, uint64_t bytes)
{
	if (bytes > ALP_TESTING_STORAGE_BACKING_BYTES) return ALP_ERR_NOMEM;

	alp_testing_storage_slot_t *slot = alp_testing_instance_table_touch(table(), storage_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	slot->capacity = bytes;
	return ALP_OK;
}

alp_status_t
alp_testing_storage_read_back(uint32_t storage_id, uint64_t offset, uint8_t *out, size_t len)
{
	if (out == NULL && len > 0u) return ALP_ERR_INVAL;

	alp_testing_storage_slot_t *slot = alp_testing_instance_table_find(table(), storage_id);
	if (slot == NULL) return ALP_ERR_INVAL;
	if (!alp_storage_range_in_capacity(offset, (uint64_t)len, ALP_TESTING_STORAGE_BACKING_BYTES)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	if (len > 0u) memcpy(out, &slot->backing[offset], len);
	return ALP_OK;
}

alp_status_t
alp_testing_storage_inject_corruption(uint32_t storage_id, uint64_t offset, uint64_t len)
{
	alp_testing_storage_slot_t *slot = alp_testing_instance_table_touch(table(), storage_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	if (!alp_storage_range_in_capacity(offset, len, ALP_TESTING_STORAGE_BACKING_BYTES)) {
		return ALP_ERR_INVAL;
	}

	bitmap_mark(slot->corrupt_bitmap, offset, len, true);
	return ALP_OK;
}

alp_status_t
alp_testing_storage_fail_next(uint32_t storage_id, alp_testing_storage_op_t op, alp_status_t err)
{
	if (op != ALP_TESTING_STORAGE_OP_READ && op != ALP_TESTING_STORAGE_OP_WRITE &&
	    op != ALP_TESTING_STORAGE_OP_ERASE && op != ALP_TESTING_STORAGE_OP_SYNC) {
		return ALP_ERR_INVAL;
	}

	alp_testing_storage_slot_t *slot = alp_testing_instance_table_touch(table(), storage_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	slot->fail_armed = true;
	slot->fail_op    = op;
	slot->fail_err   = err;
	return ALP_OK;
}

alp_status_t alp_testing_storage_inject_power_loss_after(uint32_t storage_id,
                                                         uint64_t bytes_written)
{
	alp_testing_storage_slot_t *slot = alp_testing_instance_table_touch(table(), storage_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	slot->power_loss_armed         = true;
	slot->power_loss_bytes_written = bytes_written;
	return ALP_OK;
}

/* ---- alp_storage_ops_t ---- */

static alp_status_t t_open(const alp_storage_config_t  *cfg,
                           alp_storage_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	alp_testing_storage_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->instance_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	/* kind / read_only / instance_id are already stamped onto `st` by
	 * the dispatcher (src/storage_dispatch.c) before this op runs --
	 * mirrors zephyr_flash.c's z_open() / sw_fallback.c's sw_open(). */
	st->dev         = NULL;
	st->be_data     = slot;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t t_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	info->total_bytes = slot->capacity;
	info->block_size  = ALP_TESTING_STORAGE_BLOCK_SIZE;
	info->erase_size  = ALP_TESTING_STORAGE_ERASE_SIZE;
	return ALP_OK;
}

static alp_status_t t_read(alp_storage_backend_state_t *st, uint64_t offset, void *data, size_t len)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, (uint64_t)len, slot->capacity)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	if (slot->fail_armed && slot->fail_op == ALP_TESTING_STORAGE_OP_READ) {
		alp_status_t err = slot->fail_err;
		slot->fail_armed = false;
		if (len > 0u) memset(data, 0, len);
		return err;
	}

	if (bitmap_any_set(slot->corrupt_bitmap, offset, (uint64_t)len)) {
		if (len > 0u) memset(data, 0, len);
		return ALP_ERR_IO;
	}

	if (len > 0u) memcpy(data, &slot->backing[offset], len);
	return ALP_OK;
}

static alp_status_t
t_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data, size_t len)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, (uint64_t)len, slot->capacity)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	if (slot->fail_armed && slot->fail_op == ALP_TESTING_STORAGE_OP_WRITE) {
		alp_status_t err = slot->fail_err;
		slot->fail_armed = false;
		return err;
	}

	if (slot->power_loss_armed) {
		slot->power_loss_armed = false;
		uint64_t n             = slot->power_loss_bytes_written;
		if (n > (uint64_t)len) n = (uint64_t)len;

		if (n > 0u) {
			memcpy(&slot->backing[offset], data, (size_t)n);
			bitmap_mark(slot->corrupt_bitmap, offset, n, false);
		}
		return ALP_ERR_IO;
	}

	if (len > 0u) {
		memcpy(&slot->backing[offset], data, len);
		bitmap_mark(slot->corrupt_bitmap, offset, (uint64_t)len, false);
	}
	return ALP_OK;
}

static alp_status_t t_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, len, slot->capacity)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	/* <alp/storage.h>'s alp_storage_erase() documents both bounds as
	 * MUST align to erase_size, rejecting a misaligned request with
	 * ALP_ERR_INVAL rather than partially erasing. */
	if (offset % ALP_TESTING_STORAGE_ERASE_SIZE != 0u ||
	    len % ALP_TESTING_STORAGE_ERASE_SIZE != 0u) {
		return ALP_ERR_INVAL;
	}

	if (slot->fail_armed && slot->fail_op == ALP_TESTING_STORAGE_OP_ERASE) {
		alp_status_t err = slot->fail_err;
		slot->fail_armed = false;
		return err;
	}

	if (len > 0u) {
		memset(&slot->backing[offset], 0xFF, (size_t)len); /* NOR-erased-state convention */
		bitmap_mark(slot->corrupt_bitmap, offset, len, false);
	}
	return ALP_OK;
}

static alp_status_t t_sync(alp_storage_backend_state_t *st)
{
	alp_testing_storage_slot_t *slot = (alp_testing_storage_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	if (slot->fail_armed && slot->fail_op == ALP_TESTING_STORAGE_OP_SYNC) {
		alp_status_t err = slot->fail_err;
		slot->fail_armed = false;
		return err;
	}
	return ALP_OK; /* no write-behind cache to flush */
}

static alp_status_t t_configure_inline_aes(alp_storage_backend_state_t    *st,
                                           const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	/* This double does not model inline crypto -- matches
	 * sw_fallback.c and every non-vendor-extension storage backend;
	 * see the @note on <alp/testing/storage.h>. */
	return ALP_ERR_NOSUPPORT;
}

static void t_close(alp_storage_backend_state_t *st)
{
	st->be_data = NULL;
}

static const alp_storage_ops_t _ops = {
	.open                 = t_open,
	.get_info             = t_get_info,
	.read                 = t_read,
	.write                = t_write,
	.erase                = t_erase,
	.sync                 = t_sync,
	.configure_inline_aes = t_configure_inline_aes,
	.close                = t_close,
};

ALP_BACKEND_REGISTER(storage,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

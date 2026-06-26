/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr littlefs backend.  Wraps a mounted FS_LITTLEFS instance
 * exposing it as an offset-addressed alp_storage device.
 *
 * Mount points are resolved by instance_id -- the SoC pack
 * registers fs_mount_t entries via FS_MNT macros and littlefs
 * mounts them at boot.  The backend keeps a per-handle file
 * pointer (`/lfs<N>/alp_storage.bin`) so reads / writes map
 * onto the open(2) / read(2) / write(2) shape rather than raw
 * partition I/O.
 *
 * The shape stays "block-oriented read / write / erase" per the
 * <alp/storage.h> contract: erase = fs_truncate to zero then
 * grow back to the offset+len region.
 *
 * Inline AES on littlefs is not portable; configure_inline_aes
 * returns NOSUPPORT.  Vendor packs that wire a HW-encrypted
 * flash device under littlefs implement their own backend.
 *
 * Registered as silicon_ref="*" at priority 90 -- one tick below
 * zephyr_flash so raw flash wins by default unless the caller
 * explicitly selects ALP_STORAGE_KIND_SD_MMC or the active
 * silicon disables CONFIG_FLASH_MAP.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/soc_caps.h>
#include <alp/storage.h>

#include "storage_ops.h"

typedef struct lfs_state {
	struct fs_file_t file;
	char             path[32];
	uint64_t         size_cache;
	bool             open;
} lfs_state_t;

#ifndef CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL
#define CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL 2
#endif

static lfs_state_t _lfs_pool[CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL];
static bool        _lfs_in_use[CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL];

static lfs_state_t *_lfs_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL; ++i) {
		if (!_lfs_in_use[i]) {
			memset(&_lfs_pool[i], 0, sizeof(_lfs_pool[i]));
			_lfs_in_use[i] = true;
			return &_lfs_pool[i];
		}
	}
	return NULL;
}

static void _lfs_free(lfs_state_t *s)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL; ++i) {
		if (&_lfs_pool[i] == s) {
			_lfs_in_use[i] = false;
			return;
		}
	}
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EIO:
		return ALP_ERR_IO;
	case -ENODEV:
	case -ENOENT:
		return ALP_ERR_NOT_READY;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case -ERANGE:
		return ALP_ERR_OUT_OF_RANGE;
	case -ENOMEM:
	case -ENOSPC:
		return ALP_ERR_NOMEM;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t lfs_open(const alp_storage_config_t  *cfg,
                             alp_storage_backend_state_t *st,
                             alp_capabilities_t          *caps_out)
{
	/* littlefs is layered on top of a flash partition -- only the
     * QSPI / OSPI / INTERNAL_FLASH kinds make sense here.  SD/MMC
     * routes to a different backend. */
	if (cfg->kind == ALP_STORAGE_KIND_SD_MMC) return ALP_ERR_NOSUPPORT;

	lfs_state_t *s = _lfs_alloc();
	if (s == NULL) return ALP_ERR_NOMEM;

	fs_file_t_init(&s->file);
	/* Convention: /lfs<N>/alp_storage.bin under the FS_LITTLEFS
     * mount that matches the instance_id.  Apps that want a
     * different filename layer their own FS calls on top. */
	(void)snprintf(s->path, sizeof(s->path), "/lfs%u/alp_storage.bin", (unsigned)cfg->instance_id);

	fs_mode_t mode = FS_O_RDWR | FS_O_CREATE;
	if (cfg->read_only) {
		mode = FS_O_READ;
	}
	int err = fs_open(&s->file, s->path, mode);
	if (err != 0) {
		_lfs_free(s);
		return _errno_to_alp(err);
	}
	s->open         = true;
	st->dev         = NULL;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t lfs_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s == NULL || !s->open) return ALP_ERR_NOT_READY;
	struct fs_dirent ent = { 0 };
	int              err = fs_stat(s->path, &ent);
	if (err != 0) return _errno_to_alp(err);
	info->total_bytes = ent.size;
	/* littlefs has no fixed block boundary at the SDK layer --
     * report 1-byte granularity so callers don't pad. */
	info->block_size = 1u;
	info->erase_size = 1u;
	return ALP_OK;
}

static alp_status_t
lfs_read(alp_storage_backend_state_t *st, uint64_t offset, void *data, size_t len)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s == NULL || !s->open) return ALP_ERR_NOT_READY;
	int err = fs_seek(&s->file, (off_t)offset, FS_SEEK_SET);
	if (err != 0) return _errno_to_alp(err);
	ssize_t got = fs_read(&s->file, data, len);
	if (got < 0) return _errno_to_alp((int)got);
	if ((size_t)got != len) return ALP_ERR_OUT_OF_RANGE;
	return ALP_OK;
}

static alp_status_t
lfs_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data, size_t len)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s == NULL || !s->open) return ALP_ERR_NOT_READY;
	int err = fs_seek(&s->file, (off_t)offset, FS_SEEK_SET);
	if (err != 0) return _errno_to_alp(err);
	ssize_t put = fs_write(&s->file, data, len);
	if (put < 0) return _errno_to_alp((int)put);
	if ((size_t)put != len) return ALP_ERR_IO;
	return ALP_OK;
}

static alp_status_t lfs_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s == NULL || !s->open) return ALP_ERR_NOT_READY;
	/* littlefs has no per-region erase -- emulate by overwriting
     * the region with 0xFF, matching NOR-flash erased state. */
	static const uint8_t _erased = 0xFFu;
	int                  err     = fs_seek(&s->file, (off_t)offset, FS_SEEK_SET);
	if (err != 0) return _errno_to_alp(err);
	for (uint64_t i = 0; i < len; ++i) {
		ssize_t put = fs_write(&s->file, &_erased, 1u);
		if (put < 0) return _errno_to_alp((int)put);
		if (put != 1) return ALP_ERR_IO;
	}
	return ALP_OK;
}

static alp_status_t lfs_sync(alp_storage_backend_state_t *st)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s == NULL || !s->open) return ALP_ERR_NOT_READY;
	int err = fs_sync(&s->file);
	return _errno_to_alp(err);
}

static alp_status_t lfs_configure_inline_aes(alp_storage_backend_state_t    *st,
                                             const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	/* Inline AES through the filesystem layer is not portable;
     * vendor packs that need encrypted-at-rest littlefs storage
     * register dedicated backends. */
	return ALP_ERR_NOSUPPORT;
}

static void lfs_close(alp_storage_backend_state_t *st)
{
	lfs_state_t *s = (lfs_state_t *)st->be_data;
	if (s != NULL && s->open) {
		(void)fs_close(&s->file);
		s->open = false;
		_lfs_free(s);
		st->be_data = NULL;
	}
}

static const alp_storage_ops_t _ops = {
	.open                 = lfs_open,
	.get_info             = lfs_get_info,
	.read                 = lfs_read,
	.write                = lfs_write,
	.erase                = lfs_erase,
	.sync                 = lfs_sync,
	.configure_inline_aes = lfs_configure_inline_aes,
	.close                = lfs_close,
};

ALP_BACKEND_REGISTER(storage,
                     zephyr_littlefs,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 90,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

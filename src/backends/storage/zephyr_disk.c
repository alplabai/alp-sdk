/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr disk_access backend for ALP_STORAGE_KIND_SD_MMC.  Routes the
 * alp_storage_* surface through <zephyr/storage/disk_access.h>, so any
 * disk driver Zephyr exposes by name (sdmmc "SD", RAM disk, ...) is
 * reachable.  The disk name comes from CONFIG_ALP_SDK_STORAGE_ZEPHYR_DISK_NAME;
 * alp_storage_config_t has no name field and instance_id is ignored.
 *
 * Byte-addressed API over a sector device: sector-aligned runs go straight
 * to disk_access_*; ragged heads/tails (and reads/writes into a buffer that
 * is not 4-byte aligned) bounce through one shared sector scratch, with
 * read-modify-write for writes.  erase and inline AES are NOSUPPORT.
 *
 * Registered at priority 80, below zephyr_flash (100) and zephyr_littlefs
 * (90), which decline SD_MMC with ALP_ERR_NOSUPPORT; alp_storage_open walks
 * to this backend via alp_backend_select_next().  This backend declines
 * every other kind the same way.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/storage.h>

#include "alp_errno.h"
#include "storage_ops.h"

#define SCRATCH_BYTES CONFIG_ALP_SDK_STORAGE_ZEPHYR_DISK_MAX_SECTOR_SIZE

typedef struct {
	uint32_t sector_size;
	uint32_t sector_count;
} disk_state_t;

static disk_state_t _states[CONFIG_ALP_SDK_MAX_STORAGE_HANDLES];
static bool         _used[CONFIG_ALP_SDK_MAX_STORAGE_HANDLES];
/* One scratch sector shared by every handle, serialised by _disk_lock. */
static uint8_t _scratch[SCRATCH_BYTES] __aligned(4);
static K_MUTEX_DEFINE(_disk_lock);

static const char *const _name = CONFIG_ALP_SDK_STORAGE_ZEPHYR_DISK_NAME;

static alp_status_t d_open(const alp_storage_config_t  *cfg,
                           alp_storage_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	(void)caps_out;
	if (cfg->kind != ALP_STORAGE_KIND_SD_MMC) return ALP_ERR_NOSUPPORT;

	int err = disk_access_init(_name);
	if (err != 0) return alp_status_from_zephyr_errno(err);

	uint32_t count = 0;
	uint32_t size  = 0;
	if (disk_access_ioctl(_name, DISK_IOCTL_GET_SECTOR_COUNT, &count) != 0 ||
	    disk_access_ioctl(_name, DISK_IOCTL_GET_SECTOR_SIZE, &size) != 0) {
		return ALP_ERR_IO;
	}
	if (size == 0u || size > SCRATCH_BYTES) return ALP_ERR_NOSUPPORT;

	k_mutex_lock(&_disk_lock, K_FOREVER);
	disk_state_t *s = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(_states); ++i) {
		if (!_used[i]) {
			_used[i] = true;
			s        = &_states[i];
			break;
		}
	}
	k_mutex_unlock(&_disk_lock);
	if (s == NULL) return ALP_ERR_NOMEM;

	s->sector_size  = size;
	s->sector_count = count;
	st->be_data     = s;
	return ALP_OK;
}

static alp_status_t d_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	const disk_state_t *s = st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	info->total_bytes = (uint64_t)s->sector_count * s->sector_size;
	info->block_size  = s->sector_size;
	info->erase_size  = s->sector_size;
	return ALP_OK;
}

static alp_status_t
_rw(alp_storage_backend_state_t *st, bool wr, uint64_t off, uint8_t *buf, size_t len)
{
	const disk_state_t *s = st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	uint64_t total = (uint64_t)s->sector_count * s->sector_size;
	if (!alp_storage_range_in_capacity(off, len, total)) return ALP_ERR_OUT_OF_RANGE;

	k_mutex_lock(&_disk_lock, K_FOREVER);
	int err = 0;
	while (len > 0u && err == 0) {
		uint64_t sec  = off / s->sector_size;
		uint32_t head = (uint32_t)(off % s->sector_size);
		if (head == 0u && len >= s->sector_size && ((uintptr_t)buf & 3u) == 0u) {
			/* Aligned bulk run: straight through, whole sectors. */
			uint32_t n = (uint32_t)(len / s->sector_size);
			err        = wr ? disk_access_write(_name, buf, (uint32_t)sec, n)
			                : disk_access_read(_name, buf, (uint32_t)sec, n);
			off += (uint64_t)n * s->sector_size;
			buf += (size_t)n * s->sector_size;
			len -= (size_t)n * s->sector_size;
			continue;
		}
		/* Ragged piece: one sector via the scratch. */
		size_t chunk = MIN(len, (size_t)(s->sector_size - head));
		if (!wr || chunk != s->sector_size) {
			err = disk_access_read(_name, _scratch, (uint32_t)sec, 1);
			if (err != 0) break;
		}
		if (wr) {
			memcpy(&_scratch[head], buf, chunk);
			err = disk_access_write(_name, _scratch, (uint32_t)sec, 1);
		} else {
			memcpy(buf, &_scratch[head], chunk);
		}
		off += chunk;
		buf += chunk;
		len -= chunk;
	}
	k_mutex_unlock(&_disk_lock);
	return alp_status_from_zephyr_errno(err);
}

static alp_status_t d_read(alp_storage_backend_state_t *st, uint64_t off, void *data, size_t len)
{
	return _rw(st, false, off, data, len);
}

static alp_status_t
d_write(alp_storage_backend_state_t *st, uint64_t off, const void *data, size_t len)
{
	return _rw(st, true, off, (uint8_t *)data, len);
}

static alp_status_t d_erase(alp_storage_backend_state_t *st, uint64_t off, uint64_t len)
{
	(void)st;
	(void)off;
	(void)len;
	/* disk_access has no erase/TRIM primitive. */
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t d_sync(alp_storage_backend_state_t *st)
{
	if (st->be_data == NULL) return ALP_ERR_NOT_READY;
	return alp_status_from_zephyr_errno(disk_access_ioctl(_name, DISK_IOCTL_CTRL_SYNC, NULL));
}

static alp_status_t d_configure_inline_aes(alp_storage_backend_state_t    *st,
                                           const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	return ALP_ERR_NOSUPPORT;
}

static void d_close(alp_storage_backend_state_t *st)
{
	disk_state_t *s = st->be_data;
	if (s == NULL) return;
	(void)disk_access_ioctl(_name, DISK_IOCTL_CTRL_SYNC, NULL);
	k_mutex_lock(&_disk_lock, K_FOREVER);
	_used[s - _states] = false;
	k_mutex_unlock(&_disk_lock);
	st->be_data = NULL;
}

static const alp_storage_ops_t _ops = {
	.open                 = d_open,
	.get_info             = d_get_info,
	.read                 = d_read,
	.write                = d_write,
	.erase                = d_erase,
	.sync                 = d_sync,
	.configure_inline_aes = d_configure_inline_aes,
	.close                = d_close,
};

ALP_BACKEND_REGISTER(storage,
                     zephyr_disk,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 80,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

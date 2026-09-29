/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr flash_area backend.  Routes through the
 * <zephyr/storage/flash_map.h> API; the SoC pack supplies the
 * fixed-partition DT entries that flash_area_open consumes.
 *
 * instance_id maps directly to a flash-area ID.  The kind field of
 * alp_storage_config_t selects this backend for INTERNAL_FLASH /
 * QSPI_FLASH / OSPI_FLASH (the same flash_area abstraction covers
 * all three).  SD/MMC opens are deferred to other backends.
 *
 * Inline AES on plain Zephyr flash is not portable -- vendor packs
 * (Alif SecAES, NXP OTFAD) register their own backends to implement
 * the configure_inline_aes op when they ship.
 *
 * Registered as silicon_ref="*" at priority 100; the dispatcher's
 * selector picks this when CONFIG_FLASH_MAP is on and no vendor
 * extension claims a higher priority for the active silicon.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/soc_caps.h>
#include <alp/storage.h>

#include "alp_errno.h"
#include "storage_ops.h"

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * BEHAVIOUR CHANGE: this switch had no -EAGAIN and/or no -ETIMEDOUT
	 * arm, so a driver-reported deadline surfaced as ALP_ERR_IO.  Callers
	 * can now receive ALP_ERR_TIMEOUT here, and ALP_ERR_NOT_READY /
	 * ALP_ERR_NOMEM / ALP_ERR_NOSUPPORT for the other arms the switch
	 * lacked.  Every arm it DID carry agreed with the baseline. */
	return alp_status_from_zephyr_errno(err);
}

static alp_status_t z_open(const alp_storage_config_t  *cfg,
                           alp_storage_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	/* SD/MMC isn't a flash_area abstraction.  alp_storage_open()
     * calls alp_backend_select() once (no retry loop -- that's
     * alp_backend_select_next(), used by security/update_log, not
     * storage), so this NOSUPPORT surfaces straight to the caller. */
	if (cfg->kind == ALP_STORAGE_KIND_SD_MMC) return ALP_ERR_NOSUPPORT;

	const struct flash_area *fa  = NULL;
	int                      err = flash_area_open(cfg->instance_id, &fa);
	if (err != 0 || fa == NULL) {
		return _errno_to_alp(err);
	}
	st->dev         = (void *)fa;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	/* include/alp/storage.h: "Both bounds MUST align to the device's
     * erase_size".  flash_area_get_device(fa) below is a real device
     * handle, not opaque -- flash_get_page_info_by_offs() reads the
     * actual page/sector size straight from the flash driver.  1u is
     * only a last-resort default if CONFIG_FLASH_PAGE_LAYOUT is off
     * or the lookup fails. */
	info->total_bytes        = fa->fa_size;
	info->block_size         = 1u;
	info->erase_size         = 1u;
	const struct device *dev = flash_area_get_device(fa);
	if (dev != NULL) {
		info->block_size = flash_get_write_block_size(dev);
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
		struct flash_pages_info page;
		if (flash_get_page_info_by_offs(dev, fa->fa_off, &page) == 0) {
			info->erase_size = page.size;
		}
#endif
	}
	return ALP_OK;
}

static alp_status_t z_read(alp_storage_backend_state_t *st, uint64_t offset, void *data, size_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, len, fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
	int err = flash_area_read(fa, (off_t)offset, data, len);
	return _errno_to_alp(err);
}

static alp_status_t
z_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data, size_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, len, fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
	int err = flash_area_write(fa, (off_t)offset, data, len);
	return _errno_to_alp(err);
}

static alp_status_t z_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, len, fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
	int err = flash_area_erase(fa, (off_t)offset, (size_t)len);
	return _errno_to_alp(err);
}

static alp_status_t z_sync(alp_storage_backend_state_t *st)
{
	(void)st;
	/* flash_area writes are synchronous on every Zephyr-supported
     * controller; explicit flush is unnecessary. */
	return ALP_OK;
}

static alp_status_t z_configure_inline_aes(alp_storage_backend_state_t    *st,
                                           const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	/* Plain Zephyr flash has no inline-AES path -- vendor packs
     * (Alif SecAES, NXP OTFAD) register dedicated backends that
     * win on priority and implement this op. */
	return ALP_ERR_NOSUPPORT;
}

static void z_close(alp_storage_backend_state_t *st)
{
	if (st->dev != NULL) {
		flash_area_close((const struct flash_area *)st->dev);
		st->dev = NULL;
	}
}

static const alp_storage_ops_t _ops = {
	.open                 = z_open,
	.get_info             = z_get_info,
	.read                 = z_read,
	.write                = z_write,
	.erase                = z_erase,
	.sync                 = z_sync,
	.configure_inline_aes = z_configure_inline_aes,
	.close                = z_close,
};

ALP_BACKEND_REGISTER(storage,
                     zephyr_flash,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

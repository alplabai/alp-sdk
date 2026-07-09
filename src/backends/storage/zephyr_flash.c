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

#include "alp_range.h"
#include "storage_ops.h"

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
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
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t z_open(const alp_storage_config_t  *cfg,
                           alp_storage_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	/* SD/MMC isn't a flash_area abstraction -- defer to a different
     * backend.  Returning NOSUPPORT lets the dispatcher's selector
     * fall through to the next match. */
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
	/* Erase size from the underlying flash device.  Zephyr exposes
     * the unit via flash_get_write_block_size / flash_params; for
     * v0.6 we report fa_size as both total and erase granule when
     * the device API is opaque.  Real-silicon backends override. */
	info->total_bytes        = fa->fa_size;
	info->block_size         = 1u;
	info->erase_size         = 1u;
	const struct device *dev = flash_area_get_device(fa);
	if (dev != NULL) {
		info->block_size = flash_get_write_block_size(dev);
	}
	return ALP_OK;
}

static alp_status_t z_read(alp_storage_backend_state_t *st, uint64_t offset, void *data, size_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_range_ok(offset, (uint64_t)len, (uint64_t)fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
	int err = flash_area_read(fa, (off_t)offset, data, len);
	return _errno_to_alp(err);
}

static alp_status_t
z_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data, size_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_range_ok(offset, (uint64_t)len, (uint64_t)fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
	int err = flash_area_write(fa, (off_t)offset, data, len);
	return _errno_to_alp(err);
}

static alp_status_t z_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	const struct flash_area *fa = (const struct flash_area *)st->dev;
	if (fa == NULL) return ALP_ERR_NOT_READY;
	if (!alp_range_ok(offset, len, (uint64_t)fa->fa_size)) return ALP_ERR_OUT_OF_RANGE;
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

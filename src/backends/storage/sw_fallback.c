/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software storage fallback.  Stateless stub for native_sim builds;
 * not a real device.
 *
 * Every op besides open / close returns ALP_ERR_NOSUPPORT (matching
 * the gpio / i2s / can sw_fallback shapes).  Apps that link this
 * backend should never reach read / write / erase paths on a real
 * silicon build -- zephyr_flash + zephyr_littlefs win on priority.
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_flash
 * (priority 100) and zephyr_littlefs (priority 90) on real silicon;
 * picked only when the test build forces it via
 * CONFIG_ALP_SDK_STORAGE_SW_FALLBACK=y with no Zephyr backends
 * present.
 *
 * @par Cost: ROM ~200 B, RAM 0 bytes (no per-handle state beyond
 *      the dispatcher's pool slot).
 * @par Performance: O(1) on every call; deterministic NOSUPPORT
 *      returns mean test assertions don't need timing fences.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/storage.h>

#include "storage_ops.h"

static alp_status_t sw_open(const alp_storage_config_t *cfg, alp_storage_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
	(void)cfg;
	st->dev         = NULL;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	(void)st;
	if (info != NULL) {
		*info = (alp_storage_info_t){ 0 };
	}
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_read(alp_storage_backend_state_t *st, uint64_t offset, void *data,
                            size_t len)
{
	(void)st;
	(void)offset;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data,
                             size_t len)
{
	(void)st;
	(void)offset;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	(void)st;
	(void)offset;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_sync(alp_storage_backend_state_t *st)
{
	(void)st;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_configure_inline_aes(alp_storage_backend_state_t    *st,
                                            const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	return ALP_ERR_NOSUPPORT;
}

static const alp_storage_ops_t _ops = {
	.open                 = sw_open,
	.get_info             = sw_get_info,
	.read                 = sw_read,
	.write                = sw_write,
	.erase                = sw_erase,
	.sync                 = sw_sync,
	.configure_inline_aes = sw_configure_inline_aes,
	.close                = NULL,
};

ALP_BACKEND_REGISTER(storage, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

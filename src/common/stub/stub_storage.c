/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storage (+ inline-AES extension) NOSUPPORT stubs --
 * <alp/storage.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673).  Unguarded: no
 * vendor backend has ever overridden this class.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/storage.h"

alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_storage_get_info(alp_storage_t *storage, alp_storage_info_t *info)
{
	(void)storage;
	if (info) *info = (alp_storage_info_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_read(alp_storage_t *storage, uint64_t o, void *d, size_t l)
{
	(void)storage;
	(void)o;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_write(alp_storage_t *storage, uint64_t o, const void *d, size_t l)
{
	(void)storage;
	(void)o;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_erase(alp_storage_t *storage, uint64_t o, uint64_t l)
{
	(void)storage;
	(void)o;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_sync(alp_storage_t *storage)
{
	(void)storage;
	return ALP_ERR_NOSUPPORT;
}
void alp_storage_close(alp_storage_t *storage)
{
	(void)storage;
}
const alp_capabilities_t *alp_storage_capabilities(const alp_storage_t *storage)
{
	(void)storage;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Storage inline-AES (alp/storage.h v0.5 extension)                   */
/* ------------------------------------------------------------------ */

alp_status_t alp_storage_configure_inline_aes(alp_storage_t                  *storage,
                                              const alp_storage_aes_config_t *cfg)
{
	if (cfg == NULL) return ALP_ERR_INVAL;
	(void)storage;
	return ALP_ERR_NOSUPPORT;
}

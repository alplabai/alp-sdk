/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-core (shmem + mailbox + hwsem + core boot) NOSUPPORT stubs --
 * <alp/mproc.h>.  Split out of the former src/common/stub_backend.c
 * monolith (issue #673).  Unguarded: no vendor backend has ever
 * overridden this class.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/mproc.h"
#include "alp/peripheral.h"

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_shmem_view(alp_shmem_t *s, void **b, size_t *o)
{
	(void)s;
	if (b) *b = NULL;
	if (o) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_shmem_close(alp_shmem_t *s)
{
	(void)s;
}
const alp_capabilities_t *alp_shmem_capabilities(const alp_shmem_t *s)
{
	(void)s;
	return NULL;
}
alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_mbox_send(alp_mbox_t *m, const void *d, size_t l, uint32_t t)
{
	(void)m;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_mbox_set_callback(alp_mbox_t *m, alp_mbox_msg_cb_t cb, void *u)
{
	(void)m;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
void alp_mbox_close(alp_mbox_t *m)
{
	(void)m;
}
const alp_capabilities_t *alp_mbox_capabilities(const alp_mbox_t *m)
{
	(void)m;
	return NULL;
}
alp_hwsem_t *alp_hwsem_open(uint32_t id)
{
	(void)id;
	return NULL;
}
alp_status_t alp_hwsem_try_lock(alp_hwsem_t *s)
{
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hwsem_lock(alp_hwsem_t *s, uint32_t t)
{
	(void)s;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hwsem_unlock(alp_hwsem_t *s)
{
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
void alp_hwsem_close(alp_hwsem_t *s)
{
	(void)s;
}
const alp_capabilities_t *alp_hwsem_capabilities(const alp_hwsem_t *s)
{
	(void)s;
	return NULL;
}
alp_status_t alp_mproc_boot_core(alp_core_id_t core, uintptr_t entry_addr)
{
	(void)core;
	(void)entry_addr;
	return ALP_ERR_NOSUPPORT;
}

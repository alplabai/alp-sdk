/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * BKRAM wake record store for the SoM power-domain layer (#2784, U5).
 *
 * The record says which domains were quiesced so the cold-boot wake knows what
 * to put back.  It must live in memory that survives STOP, which on the Alif
 * E8 is the 4 KB Utility SRAM ("BKRAM", always retained and reserved for the
 * SDK).  The BKRAM base address is not citable from the tree yet, so the
 * record sits behind the four store_* functions below and is backed by a
 * `__noinit` RAM placeholder.
 *
 * TODO(U7): back this with BKRAM (a zephyr,retained-ram node once its base
 * address is cited from the DFP).  A __noinit cell survives a warm reset but
 * NOT a STOP, so until U7 lands a STOP cycle never finds a valid record and
 * the wake path touches nothing.  That is the safe direction.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/linker/sections.h>
#include <zephyr/toolchain.h>

#include "som_power.h"

static alp_som_pd_record_t _store __noinit;

uint32_t alp_som_pd_record_crc(const alp_som_pd_record_t *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(alp_som_pd_record_t, crc));
}

bool alp_som_pd_record_valid(const alp_som_pd_record_t *rec)
{
	return rec->magic == ALP_SOM_PD_RECORD_MAGIC && rec->crc == alp_som_pd_record_crc(rec);
}

bool alp_som_pd_store_load(alp_som_pd_record_t *out)
{
	memcpy(out, &_store, sizeof(*out));
	return alp_som_pd_record_valid(out);
}

void alp_som_pd_store_save(alp_som_pd_record_t *rec)
{
	rec->magic = ALP_SOM_PD_RECORD_MAGIC;
	rec->crc   = alp_som_pd_record_crc(rec);
	memcpy(&_store, rec, sizeof(_store));
}

void alp_som_pd_store_clear(void)
{
	memset(&_store, 0, sizeof(_store));
}

void alp_som_pd_store_poke(const alp_som_pd_record_t *rec)
{
	memcpy(&_store, rec, sizeof(_store));
}

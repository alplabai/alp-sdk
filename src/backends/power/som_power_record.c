/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * BKRAM wake record store for the SoM power-domain layer (#2784, U5/U7).
 *
 * The record says which domains were quiesced and how the sleep was armed, so the
 * cold-boot wake knows what to put back and why the SoC woke.  It must live in
 * memory that survives STOP, which on the Alif E8 is the 4 KB Utility SRAM
 * ("BKRAM": always retained across STOP, reserved for the SDK and never
 * application RAM).
 *
 * Placement
 * ---------
 * The generated E8 devicetree carries a `bkram` node (`zephyr,memory-region =
 * "ALP_BKRAM"`, reg 0x4902C000 size 0x1000), which makes the Zephyr linker
 * emit a NOLOAD output section of that name at that address -- nothing zeroes
 * or loads it at boot, so the contents of the previous cycle are still there on
 * the cold-boot wake.  The base address comes from the Alif DFP SVDs
 * (`<memory name="Backup_SRAM" start="0x4902C000" size="0x00001000"/>`; the
 * peripheral map of the E8 leaves exactly that 4 KB slot free between ADC_VREF
 * 0x4902B000 and PDM 0x4902D000, and its CLKCTL_PER_SLV.BKRAM_CKEN and
 * VBAT.RET_CTRL.BKRAM_RET_MASK fields address this block).  Only a build with the
 * STOP backend (CONFIG_ALP_SDK_POWER_ALIF_SE) places the record there, so BKRAM is
 * untouched unless the backend that needs it is enabled.  Every other build, and
 * targets without the node (native_sim, E4/E6), keep the record in a plain
 * `__noinit` cell: that survives a warm reset but not a STOP, so a STOP cycle
 * there never finds a valid record and the wake path touches nothing, which is
 * the safe direction.
 *
 * A bench scratch cell (CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH, off by
 * default) shares the section so a bench image can prove retention with a
 * counter the SDK does not own; it is never present in a product build.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/linker/sections.h>
#include <zephyr/sys/crc.h>
#include <zephyr/toolchain.h>

#include "som_power.h"

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(bkram)) && defined(CONFIG_ALP_SDK_POWER_ALIF_SE)
#define SOMPD_BKRAM_SECTION Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME_TOKEN(DT_NODELABEL(bkram)))
#define SOMPD_IN_BKRAM      1
#else
#define SOMPD_BKRAM_SECTION __noinit
#define SOMPD_IN_BKRAM      0
#endif

/* One layout for the whole 4 KB region: the record first, then the optional bench
 * scratch.  A single struct keeps the offsets stable and lets the build assert
 * that everything fits. */
typedef struct {
	alp_som_pd_record_t record;
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	alp_som_pd_bench_t bench;
#endif
} sompd_bkram_t;

#if SOMPD_IN_BKRAM
BUILD_ASSERT(sizeof(sompd_bkram_t) <= DT_REG_SIZE(DT_NODELABEL(bkram)),
             "som_power: the BKRAM layout must fit the Utility SRAM");
#endif

static sompd_bkram_t _bk SOMPD_BKRAM_SECTION;
#define _store (_bk.record)

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

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* Bench-only retention proof: a counter with its own magic + CRC, so a power cycle
 * (random SRAM) reads as 0 rather than garbage. */
#define SOMPD_BENCH_MAGIC 0x42454e43u /* "BENC" */

uint32_t alp_som_pd_bench_count(void)
{
	alp_som_pd_bench_t b = _bk.bench;

	if (b.magic != SOMPD_BENCH_MAGIC || b.crc != crc32_ieee((const uint8_t *)&b, 8u)) {
		return 0u;
	}
	return b.count;
}

void alp_som_pd_bench_set(uint32_t count)
{
	alp_som_pd_bench_t b = { .magic = SOMPD_BENCH_MAGIC, .count = count };

	b.crc     = crc32_ieee((const uint8_t *)&b, 8u);
	_bk.bench = b;
}
#endif

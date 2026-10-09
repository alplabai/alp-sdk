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
 * the cold-boot wake.  The base address is bench-verified on the E8
 * (E1M-AEN803, 2026-10-09: 4 KiB read/write at 0x4902C000, survives SYSRESETREQ,
 * lost on a cold power cycle) and matches the Alif DFP SVDs of the E1C / E3 / E5 /
 * E7 (`<memory name="Backup_SRAM" start="0x4902C000" size="0x00001000"/>`; the E8
 * SVD omits its <memory> list).  The clock gate is BKRAM_CKEN (CLKCTL_PER_SLV
 * 0x4902F000 bit 4, set at cold boot), which this file asserts before every access;
 * retention is VBAT.RET_CTRL bit 0 (BKRAM_RET_MASK), checked by the STOP backend.  Only a build with the
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
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/sys_io.h>
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
	alp_som_pd_diag_t  diag[2]; /* PRE (before the WFI), BOOT (earliest init of the next boot) */
#endif
} sompd_bkram_t;

#if SOMPD_IN_BKRAM
BUILD_ASSERT(sizeof(sompd_bkram_t) <= DT_REG_SIZE(DT_NODELABEL(bkram)),
             "som_power: the BKRAM layout must fit the Utility SRAM");
#endif

/* The record layout is the contract between the sleep that writes it and the next
 * boot that reads it (and the CRC covers everything before `crc`). */
BUILD_ASSERT(sizeof(alp_som_pd_record_t) == 52, "wake record must be 52 bytes");
BUILD_ASSERT(offsetof(alp_som_pd_record_t, crc) == 48, "wake record CRC must sit at offset 48");

static sompd_bkram_t _bk SOMPD_BKRAM_SECTION;
#define _store (_bk.record)

#if SOMPD_IN_BKRAM

/* BKRAM_CKEN: CLKCTL_PER_SLV (Alif DFP soc.h CLKCTL_PER_SLV_BASE 0x4902F000) bit 4,
 * reset value 0x10 and read back as 1 at cold boot on the E8 (bench, 2026-10-09).
 * Asserted before every access rather than assumed: an access to a gated block would
 * fault or read garbage. */
#define SOMPD_BKRAM_CKEN_REG 0x4902F000u
#define SOMPD_BKRAM_CKEN_BIT BIT(4)

static void bkram_clock_assert(void)
{
	uint32_t v = sys_read32(SOMPD_BKRAM_CKEN_REG);

	if ((v & SOMPD_BKRAM_CKEN_BIT) == 0u) {
		sys_write32(v | SOMPD_BKRAM_CKEN_BIT, SOMPD_BKRAM_CKEN_REG);
	}
}
#else
static inline void bkram_clock_assert(void)
{
}
#endif

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
	bkram_clock_assert();
	memcpy(out, &_store, sizeof(*out));
	return alp_som_pd_record_valid(out);
}

void alp_som_pd_store_save(alp_som_pd_record_t *rec)
{
	bkram_clock_assert();
	rec->magic = ALP_SOM_PD_RECORD_MAGIC;
	rec->crc   = alp_som_pd_record_crc(rec);
	memcpy(&_store, rec, sizeof(_store));
}

void alp_som_pd_store_clear(void)
{
	bkram_clock_assert();
	memset(&_store, 0, sizeof(_store));
}

void alp_som_pd_store_poke(const alp_som_pd_record_t *rec)
{
	bkram_clock_assert();
	memcpy(&_store, rec, sizeof(_store));
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* Bench-only retention proof: a counter with its own magic + CRC, so a power cycle
 * (random SRAM) reads as 0 rather than garbage. */
#define SOMPD_BENCH_MAGIC 0x42454e43u /* "BENC" */

uint32_t alp_som_pd_bench_count(void)
{
	bkram_clock_assert();
	alp_som_pd_bench_t b = _bk.bench;

	if (b.magic != SOMPD_BENCH_MAGIC || b.crc != crc32_ieee((const uint8_t *)&b, 8u)) {
		return 0u;
	}
	return b.count;
}

void alp_som_pd_bench_set(uint32_t count)
{
	bkram_clock_assert();
	alp_som_pd_bench_t b = { .magic = SOMPD_BENCH_MAGIC, .count = count };

	b.crc     = crc32_ieee((const uint8_t *)&b, 8u);
	_bk.bench = b;
}

#define SOMPD_DIAG_MAGIC 0x44494147u /* "DIAG" */

void alp_som_pd_diag_save(unsigned slot, const uint32_t *words, unsigned n)
{
	alp_som_pd_diag_t d = { .magic = SOMPD_DIAG_MAGIC };

	if (slot >= 2u) {
		return;
	}
	bkram_clock_assert();
	d.seq = (_bk.diag[slot].magic == SOMPD_DIAG_MAGIC) ? _bk.diag[slot].seq + 1u : 1u;
	for (unsigned i = 0; i < n && i < ALP_SOM_PD_DIAG_WORDS; ++i) {
		d.w[i] = words[i];
	}
	_bk.diag[slot] = d;
}

bool alp_som_pd_diag_load(unsigned slot, alp_som_pd_diag_t *out)
{
	if (slot >= 2u) {
		return false;
	}
	bkram_clock_assert();
	*out = _bk.diag[slot];
	return out->magic == SOMPD_DIAG_MAGIC;
}
#endif

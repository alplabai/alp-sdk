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
BUILD_ASSERT(sizeof(alp_som_pd_record_t) == 60, "wake record must be 60 bytes");
BUILD_ASSERT(offsetof(alp_som_pd_record_t, crc) == 56, "wake record CRC must sit at offset 56");

static sompd_bkram_t _bk SOMPD_BKRAM_SECTION;

/* The SDK's view of the BKRAM contents.  Normally the region itself.  While the boot-time
 * clock restore runs it is a plain RAM shadow (alp_som_pd_shadow_begin / _end): the SE's
 * set_run_cfg can switch the Utility SRAM's retention LDO or its clock off and leave the
 * block returning a constant and ignoring writes (bench U8e), taking the record, the bench
 * cell and the diag with it.  The data is copied out first, the block is proved writable
 * after the restore, and only then is it copied back. */
#if SOMPD_IN_BKRAM
static sompd_bkram_t  _shadow;
static sompd_bkram_t *_pk = &_bk;
#else
#define _pk (&_bk)
#endif
#define _store (_pk->record)

#if SOMPD_IN_BKRAM

/* BKRAM_CKEN: CLKCTL_PER_SLV (Alif DFP soc.h CLKCTL_PER_SLV_BASE 0x4902F000) bit 4,
 * reset value 0x10 and read back as 1 at cold boot on the E8 (bench, 2026-10-09).
 * Asserted before every access rather than assumed: an access to a gated block would
 * fault or read garbage. */
#define SOMPD_BKRAM_CKEN_REG 0x4902F000u
#define SOMPD_BKRAM_CKEN_BIT BIT(4)

static void bkram_clock_assert(void)
{
	if (_pk != &_bk) {
		return; /* shadowed: the data is in RAM, the block is not touched */
	}
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

/* Non-destructive write/readback of the words past the SDK layout (offset 0xF00, 64 words):
 * every word is saved, all are written with an index-mixed pattern and then all are read back
 * (so a block that returns a constant, or a bus that echoes the last write, fails), then the
 * saved values go back.  Two patterns, complementary bits. */
#define SOMPD_SELFTEST_OFF   0xF00u
#define SOMPD_SELFTEST_WORDS 64u

bool alp_som_pd_bkram_selftest(void)
{
#if SOMPD_IN_BKRAM
	BUILD_ASSERT(sizeof(sompd_bkram_t) <= SOMPD_SELFTEST_OFF,
	             "the BKRAM self-test words must lie past the SDK layout");
	BUILD_ASSERT(SOMPD_SELFTEST_OFF + SOMPD_SELFTEST_WORDS * 4u <= DT_REG_SIZE(DT_NODELABEL(bkram)),
	             "the BKRAM self-test words must fit the region");
	static const uint32_t pat[2] = { 0xA5A5A5A5u, 0x5A5A5A5Au };
	volatile uint32_t    *t =
	    (volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(bkram)) + SOMPD_SELFTEST_OFF);
	uint32_t     keep[SOMPD_SELFTEST_WORDS];
	bool         ok  = true;
	unsigned int key = irq_lock();

	/* Unconditionally: the block itself is under test, never the shadow. */
	uint32_t en = sys_read32(SOMPD_BKRAM_CKEN_REG);

	if ((en & SOMPD_BKRAM_CKEN_BIT) == 0u) {
		sys_write32(en | SOMPD_BKRAM_CKEN_BIT, SOMPD_BKRAM_CKEN_REG);
	}
	for (unsigned i = 0; i < SOMPD_SELFTEST_WORDS; ++i) {
		keep[i] = t[i];
	}
	for (unsigned p = 0; p < 2u && ok; ++p) {
		for (unsigned i = 0; i < SOMPD_SELFTEST_WORDS; ++i) {
			t[i] = pat[p] ^ (i * 0x9E3779B9u);
		}
		for (unsigned i = 0; i < SOMPD_SELFTEST_WORDS; ++i) {
			if (t[i] != (pat[p] ^ (i * 0x9E3779B9u))) {
				ok = false;
				break;
			}
		}
	}
	for (unsigned i = 0; i < SOMPD_SELFTEST_WORDS; ++i) {
		t[i] = keep[i];
	}
	irq_unlock(key);
	return ok;
#else
	return true;
#endif
}

void alp_som_pd_shadow_begin(void)
{
#if SOMPD_IN_BKRAM
	if (_pk == &_bk) {
		bkram_clock_assert();
		memcpy(&_shadow, &_bk, sizeof(_shadow));
		_pk = &_shadow;
	}
#endif
}

bool alp_som_pd_shadow_end(void)
{
#if SOMPD_IN_BKRAM
	if (_pk == &_bk) {
		return true;
	}
	if (!alp_som_pd_bkram_selftest()) {
		return false; /* BKRAM is dead: stay on the shadow, the sleep path refuses */
	}
	/* _pk is still the shadow, so assert the block's clock by hand. */
	uint32_t v = sys_read32(SOMPD_BKRAM_CKEN_REG);

	if ((v & SOMPD_BKRAM_CKEN_BIT) == 0u) {
		sys_write32(v | SOMPD_BKRAM_CKEN_BIT, SOMPD_BKRAM_CKEN_REG);
	}
	memcpy(&_bk, &_shadow, sizeof(_bk));
	if (memcmp(&_bk, &_shadow, sizeof(_bk)) != 0) {
		return false;
	}
	_pk = &_bk;
#endif
	return true;
}

bool alp_som_pd_bkram_live(void)
{
#if SOMPD_IN_BKRAM
	return _pk == &_bk;
#else
	return true;
#endif
}

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

/* CRC-32 of the ROM region (vector table, code, rodata): changes with any rebuild or
 * variant.  Computed once per boot.  Bench builds only: it costs 15-30 ms, which a product
 * boot must not pay; there the identity is the constant 0 (every image's data is "ours"). */
#if SOMPD_IN_BKRAM && defined(CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH)
#include <zephyr/linker/linker-defs.h>

__weak uint32_t alp_som_pd_image_id(void)
{
	static uint32_t id;
	static bool     have;

	if (!have) {
		id   = crc32_ieee((const uint8_t *)__rom_region_start,
		                  (size_t)(__rom_region_end - __rom_region_start));
		have = true;
	}
	return id;
}
#else
__weak uint32_t alp_som_pd_image_id(void)
{
	return 0u;
}
#endif

void alp_som_pd_store_save(alp_som_pd_record_t *rec)
{
	bkram_clock_assert();
	rec->image_id = alp_som_pd_image_id();
	rec->magic    = ALP_SOM_PD_RECORD_MAGIC;
	rec->crc      = alp_som_pd_record_crc(rec);
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
#define SOMPD_BENCH_MAGIC 0x42454e32u /* "BEN2" */

uint32_t alp_som_pd_bench_count(void)
{
	bkram_clock_assert();
	alp_som_pd_bench_t b = _pk->bench;

	/* Another image's counter reads 0: a clean flash starts fresh. */
	if (b.magic != SOMPD_BENCH_MAGIC || b.crc != crc32_ieee((const uint8_t *)&b, 12u) ||
	    b.image != alp_som_pd_image_id()) {
		return 0u;
	}
	return b.count;
}

bool alp_som_pd_bkram_foreign(void)
{
	bkram_clock_assert();
	alp_som_pd_bench_t b = _pk->bench;

	if (b.magic != SOMPD_BENCH_MAGIC) {
		return false; /* no counter at all: nothing says another image ran */
	}
	return b.crc != crc32_ieee((const uint8_t *)&b, 12u) || b.image != alp_som_pd_image_id();
}

void alp_som_pd_bkram_adopt(void)
{
	alp_som_pd_bench_set(0u);
	alp_som_pd_diag_invalidate(ALP_SOM_PD_DIAG_PRE);
}

void alp_som_pd_bench_set(uint32_t count)
{
	bkram_clock_assert();
	alp_som_pd_bench_t b = { .magic = SOMPD_BENCH_MAGIC,
		                     .count = count,
		                     .image = alp_som_pd_image_id() };

	b.crc      = crc32_ieee((const uint8_t *)&b, 12u);
	_pk->bench = b;
}

#define SOMPD_DIAG_MAGIC 0x44494147u /* "DIAG" */

static uint32_t _diag_boot_seq;

void alp_som_pd_diag_patch(unsigned slot, unsigned idx, uint32_t value)
{
	if (slot >= 2u || idx >= ALP_SOM_PD_DIAG_WORDS) {
		return;
	}
	bkram_clock_assert();
	_pk->diag[slot].w[idx] = value;
}

void alp_som_pd_diag_invalidate(unsigned slot)
{
	if (slot >= 2u) {
		return;
	}
	bkram_clock_assert();
	_pk->diag[slot].magic = 0u;
}

void alp_som_pd_diag_save(unsigned slot, const uint32_t *words, unsigned n)
{
	alp_som_pd_diag_t d = { .magic = SOMPD_DIAG_MAGIC };

	if (slot >= 2u) {
		return;
	}
	bkram_clock_assert();
	/* BOOT is written first thing each boot and numbers it (previous BOOT + 1); PRE, written
	 * later in the same boot, carries that boot's number, so after the next boot the two
	 * slots show which boot wrote what.  `cycle` is the bench counter at the time. */
	if (slot == ALP_SOM_PD_DIAG_BOOT) {
		_diag_boot_seq =
		    (_pk->diag[slot].magic == SOMPD_DIAG_MAGIC) ? _pk->diag[slot].seq + 1u : 1u;
	}
	d.seq   = _diag_boot_seq;
	d.cycle = alp_som_pd_bench_count();
	for (unsigned i = 0; i < n && i < ALP_SOM_PD_DIAG_WORDS; ++i) {
		d.w[i] = words[i];
	}
	_pk->diag[slot] = d;
}

bool alp_som_pd_diag_load(unsigned slot, alp_som_pd_diag_t *out)
{
	if (slot >= 2u) {
		return false;
	}
	bkram_clock_assert();
	*out = _pk->diag[slot];
	return out->magic == SOMPD_DIAG_MAGIC;
}
#endif

#ifndef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
bool alp_som_pd_bkram_foreign(void)
{
	return false;
}

void alp_som_pd_bkram_adopt(void)
{
}
#endif

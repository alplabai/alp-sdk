/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit checks for alp_secure_page_mirror_t (include/alp/hw_info.h): struct
 * size/layout, the CRC-32 round-trip, and blank-page detection.
 *
 * Pure host logic -- no I2C, no eeprom_24c128_t -- so this file is portable
 * across the baremetal/yocto/zephyr test layers the same way
 * tests/common/eeprom_24c128_range.c is.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_alp_secure_page_mirror
 *   ctest --test-dir build -R alp_test_alp_secure_page_mirror
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_alp_secure_page_mirror.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/hw_info.h"

#include "test_assert.h"

/* CRC-32 ISO-3309 -- deliberately its own copy, not shared with
 * examples/aen/aen-eeprom-provision/src/main.c's crc32_iso3309(): this test
 * and that app live in different portability tiers (this file builds under
 * the host test layer; that app is Zephyr-only), and the repo's established
 * convention for this exact algorithm is a small independent copy per
 * consumer rather than a shared header -- see that file's own comment for
 * why duplication is deliberate here. */
static uint32_t crc32_iso3309(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) {
		crc ^= (uint32_t)buf[i];
		for (unsigned b = 0; b < 8; ++b) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
			crc           = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

/* Mirrors examples/aen/aen-eeprom-provision's secure_page_is_blank(): test
 * the magic field, not "every byte reads 0xFF" -- see that function's doc
 * comment and EEPROM-MANIFEST-SPEC.md's blank-page rule for why. */
static bool mirror_is_blank(const uint8_t page[sizeof(alp_secure_page_mirror_t)])
{
	uint32_t magic;
	memcpy(&magic, page, sizeof(magic));
	return magic != ALP_SECURE_PAGE_MAGIC;
}

static void test_struct_size(void)
{
	ALP_ASSERT_EQ_INT(sizeof(alp_secure_page_mirror_t), 64);
}

/* Pin every field's offset -- a compiler that ever inserted padding (it
 * shouldn't; see the struct's doc comment for why every width was chosen to
 * avoid that) would move every field after the insertion point, and the
 * 64-byte total size check alone would not catch a same-total reshuffle. */
static void test_struct_layout(void)
{
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, magic), 0x00);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, schema_version), 0x04);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, sku), 0x05);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, hw_rev), 0x15);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, serial), 0x21);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, mfg_year), 0x38);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, mfg_month), 0x3A);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, mfg_day), 0x3B);
	ALP_ASSERT_EQ_INT(offsetof(alp_secure_page_mirror_t, crc32), 0x3C);

	ALP_ASSERT_EQ_INT(sizeof(((alp_secure_page_mirror_t *)0)->sku), ALP_SECURE_PAGE_SKU_LEN);
	ALP_ASSERT_EQ_INT(sizeof(((alp_secure_page_mirror_t *)0)->hw_rev), ALP_SECURE_PAGE_HW_REV_LEN);
	ALP_ASSERT_EQ_INT(sizeof(((alp_secure_page_mirror_t *)0)->serial), ALP_SECURE_PAGE_SERIAL_LEN);
}

/* EEPROM-MANIFEST-SPEC.md's worked test vector for a fictional
 * E1M-AEN801 2626-r2, serial TEST-0001 (the same placeholder the
 * 128-byte manifest's own test vector uses -- not a real unit), mfg
 * 2026-05-11 -- computed independently with Python's zlib.crc32 when
 * the spec was written; reproducing it here with this file's own C
 * implementation cross-checks both against a third, independent
 * source (this file's crc32_iso3309 vs Python's zlib.crc32). */
static void test_crc_matches_spec_vector(void)
{
	alp_secure_page_mirror_t m;
	memset(&m, 0, sizeof(m));
	m.magic          = ALP_SECURE_PAGE_MAGIC;
	m.schema_version = ALP_SECURE_PAGE_SCHEMA_VERSION;
	memcpy(m.sku, "E1M-AEN801", sizeof("E1M-AEN801"));
	memcpy(m.hw_rev, "2626-r2", sizeof("2626-r2"));
	/* TEST-0001 -- the same placeholder-labelled fictional serial the
     * 128-byte manifest's own spec test vector uses, deliberately not a
     * real allocated unit or a real SKU/serial pairing. */
	memcpy(m.serial, "TEST-0001", sizeof("TEST-0001"));
	m.mfg_year  = 2026;
	m.mfg_month = 5;
	m.mfg_day   = 11;

	const size_t covered = sizeof(m) - sizeof(m.crc32);
	m.crc32              = crc32_iso3309((const uint8_t *)&m, covered);

	ALP_ASSERT_EQ_INT(m.crc32, 0xBABB16C3u);
}

/* CRC round-trip: build, seal, verify; then corrupt one byte anywhere in
 * the covered range and confirm the same recompute now disagrees.  This is
 * the pair explicitly named for mutation-checking: break the CRC's covered
 * range or its comparison and this test must die. */
static void test_crc_round_trip(void)
{
	alp_secure_page_mirror_t m;
	memset(&m, 0, sizeof(m));
	m.magic          = ALP_SECURE_PAGE_MAGIC;
	m.schema_version = ALP_SECURE_PAGE_SCHEMA_VERSION;
	memcpy(m.sku, "E1M-AEN401", sizeof("E1M-AEN401"));
	memcpy(m.hw_rev, "r1", sizeof("r1"));
	memcpy(m.serial, "2026W01-0001", sizeof("2026W01-0001"));
	m.mfg_year  = 2026;
	m.mfg_month = 1;
	m.mfg_day   = 15;

	const size_t covered = sizeof(m) - sizeof(m.crc32);
	m.crc32              = crc32_iso3309((const uint8_t *)&m, covered);

	uint32_t recomputed = crc32_iso3309((const uint8_t *)&m, covered);
	ALP_ASSERT_EQ_INT(m.crc32, recomputed);

	/* Corrupt one byte inside the covered range (the serial field) and
     * confirm the stored CRC no longer matches -- proves the covered range
     * actually includes this byte, not just that the function runs. */
	uint8_t *raw = (uint8_t *)&m;
	raw[offsetof(alp_secure_page_mirror_t, serial)] ^= 0xFFu;
	uint32_t after_corruption = crc32_iso3309((const uint8_t *)&m, covered);
	ALP_ASSERT_TRUE(after_corruption != m.crc32);
}

static void test_blank_page_all_ff(void)
{
	uint8_t page[sizeof(alp_secure_page_mirror_t)];
	memset(page, 0xFF, sizeof(page));
	ALP_ASSERT_TRUE(mirror_is_blank(page));
}

static void test_blank_page_all_zero_is_also_blank(void)
{
	/* Not the datasheet's erased state, but still correctly "not our
     * magic" -- the blank check must not special-case 0xFF. */
	uint8_t page[sizeof(alp_secure_page_mirror_t)];
	memset(page, 0x00, sizeof(page));
	ALP_ASSERT_TRUE(mirror_is_blank(page));
}

static void test_provisioned_page_is_not_blank(void)
{
	alp_secure_page_mirror_t m;
	memset(&m, 0xFF, sizeof(m));     /* start from "erased" ... */
	m.magic = ALP_SECURE_PAGE_MAGIC; /* ... then only the magic is real. */

	uint8_t page[sizeof(alp_secure_page_mirror_t)];
	memcpy(page, &m, sizeof(page));
	ALP_ASSERT_TRUE(!mirror_is_blank(page));
}

/* ---- alp_secure_page_mirror_classify() ----
 *
 * Previously had NO runtime coverage anywhere: the provisioning example's
 * three build modes are all `build_only: true` in testcase.yaml, and this
 * file only exercised the struct/CRC/blank-detect layer.  That gap is why
 * the misaligned-cast bug (device-read bytes and the compiled-in blob both
 * cast straight to `const alp_secure_page_mirror_t *`) survived review of
 * the wire-format fix uncaught -- these tests close it. */

static void build_valid_mirror(uint8_t page[sizeof(alp_secure_page_mirror_t)])
{
	alp_secure_page_mirror_t m;
	memset(&m, 0, sizeof(m));
	m.magic          = ALP_SECURE_PAGE_MAGIC;
	m.schema_version = ALP_SECURE_PAGE_SCHEMA_VERSION;
	memcpy(m.sku, "E1M-AEN401", sizeof("E1M-AEN401"));
	memcpy(m.hw_rev, "r1", sizeof("r1"));
	memcpy(m.serial, "TEST-0001", sizeof("TEST-0001"));
	m.mfg_year  = 2026;
	m.mfg_month = 3;
	m.mfg_day   = 1;
	memcpy(page, &m, sizeof(m));
}

static void test_classify_accepts_valid_mirror(void)
{
	uint8_t page[sizeof(alp_secure_page_mirror_t)];
	build_valid_mirror(page);

	alp_secure_page_mirror_t out;
	memset(&out, 0xAA, sizeof(out)); /* poison -- classify must overwrite all of it */
	ALP_ASSERT_TRUE(alp_secure_page_mirror_classify(page, &out));
	ALP_ASSERT_EQ_INT(out.magic, ALP_SECURE_PAGE_MAGIC);
	ALP_ASSERT_EQ_INT(out.schema_version, ALP_SECURE_PAGE_SCHEMA_VERSION);
	ALP_ASSERT_EQ_INT(memcmp(out.sku, "E1M-AEN401", sizeof("E1M-AEN401")), 0);
}

static void test_classify_refuses_wrong_magic(void)
{
	uint8_t page[sizeof(alp_secure_page_mirror_t)];
	build_valid_mirror(page);
	memset(page, 0xFF, 4); /* corrupt magic -- looks like an erased page */

	alp_secure_page_mirror_t out;
	ALP_ASSERT_TRUE(!alp_secure_page_mirror_classify(page, &out));
}

/* Forward AND backward direction: a schema_version this build does not
 * have explicit code for must be refused whether it is higher (a future
 * layout this build predates) or lower (a version 0 never existed) --
 * the forward-compatibility rule stated in this struct's own doc comment
 * and EEPROM-MANIFEST-SPEC.md, and the one place a locked page's reader
 * bug is unfixable. */
static void test_classify_refuses_unrecognised_schema_version_both_directions(void)
{
	uint8_t                  page[sizeof(alp_secure_page_mirror_t)];
	alp_secure_page_mirror_t out;

	build_valid_mirror(page);
	page[4] = (uint8_t)(ALP_SECURE_PAGE_SCHEMA_VERSION + 1); /* higher */
	ALP_ASSERT_TRUE(!alp_secure_page_mirror_classify(page, &out));

	build_valid_mirror(page);
	page[4] = 0; /* lower */
	ALP_ASSERT_TRUE(!alp_secure_page_mirror_classify(page, &out));
}

/* THE regression test for the misaligned-cast bug (review MAJOR 4):
 * embed a valid mirror at a 1-byte-aligned offset inside a larger buffer,
 * mirroring eeprom_24c128_identity_t's actual layout (`secure_page_valid`
 * is a `bool` at offset 0, `secure_page[64]` at offset 1 -- alignment 1
 * throughout). A pointer straight into the middle of `holder` below is
 * guaranteed misaligned for a 4-byte-aligned type; classify() must still
 * produce the right answer because it goes through `memcpy`, never a
 * cast. */
static void test_classify_works_on_a_misaligned_source_buffer(void)
{
	uint8_t holder[1 + sizeof(alp_secure_page_mirror_t)];
	holder[0] = 0xAA; /* stands in for eeprom_24c128_identity_t::secure_page_valid */
	build_valid_mirror(holder + 1);

	const uint8_t           *misaligned = holder + 1; /* offset 1 -- not 4-byte aligned */
	alp_secure_page_mirror_t out;
	ALP_ASSERT_TRUE(alp_secure_page_mirror_classify(misaligned, &out));
	ALP_ASSERT_EQ_INT(out.mfg_year, 2026); /* the uint16_t field UBSan flags on a bad cast */
	ALP_ASSERT_EQ_INT(memcmp(out.sku, "E1M-AEN401", sizeof("E1M-AEN401")), 0);
}

int main(void)
{
	test_struct_size();
	test_struct_layout();
	test_crc_matches_spec_vector();
	test_crc_round_trip();
	test_blank_page_all_ff();
	test_blank_page_all_zero_is_also_blank();
	test_provisioned_page_is_not_blank();

	test_classify_accepts_valid_mirror();
	test_classify_refuses_wrong_magic();
	test_classify_refuses_unrecognised_schema_version_both_directions();
	test_classify_works_on_a_misaligned_source_buffer();

	ALP_TEST_SUMMARY();
}

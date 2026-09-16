/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit checks for the array-manifest-vs-mirror cross-check that
 * examples/aen/aen-eeprom-provision's mode 3 (ALP_LOCK_SECURE_PAGE) runs
 * before it permanently seals the Secure Data Page.
 *
 * The check compares the 128-byte array manifest at 0x50 against the
 * 64-byte mirror about to be locked, field by field (sku/hw_rev/serial/mfg
 * date), as NUL-terminated strings -- the two objects use DIFFERENT field
 * widths (manifest 24/8/24, mirror 16/12/23), so a raw byte-range compare
 * would treat the width difference itself as a disagreement.  Without it, a
 * self-consistent CRC-valid mirror belonging to another module, or one
 * carrying a typo'd --serial, would lock forever undetected.
 * test_refuses_when_sku_disagrees() reproduces the case review found: a
 * mirror carrying sku E1M-AEN801 about to be sealed onto a module whose
 * array manifest says E1M-AEN401, serial 2026W36-0004.
 *
 * SCOPE, stated exactly -- mode 3 has three lock-time gates and this file
 * covers ONE of them, partially:
 *
 *   - The cross-check's ALGORITHM is covered here, in a duplicate.  Nothing
 *     ties this duplicate to main.c's array_manifest_agrees_with_mirror(),
 *     so a defect introduced in main.c's copy alone still passes.
 *   - The ALP_SECURE_PAGE_COLD_CYCLED attestation gate has NO test.  It is
 *     a compile-time #if; an earlier revision of this file "tested" it with
 *     a `return x` helper asserted to return x, which could not go red for
 *     any defect, and has been deleted rather than banked as coverage.
 *   - The post-lock re-read + byte-compare has NO test, for the same
 *     reason: a memcmp wrapper checked against a buffer the test just
 *     memcpy'd proves nothing about main.c.
 *
 * examples/aen/aen-eeprom-provision/src/main.c is Zephyr-only (it drives the
 * portable alp_i2c and eeprom_24c128 API against a real I2C bus) and cannot
 * build under this host test layer -- see tests/common/alp_secure_page_mirror.c's
 * header comment for the established precedent (that file duplicates
 * secure_page_is_blank() rather than sharing a source file across
 * portability tiers).  That those gates exist, in the right order, ahead of
 * the lock is a code-review property, not something a host unit test can
 * observe without a real board.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_aen_eeprom_provision_lock_gates
 *   ctest --test-dir build -R alp_test_aen_eeprom_provision_lock_gates
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_aen_eeprom_provision_lock_gates.
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
 * the host test layer; that app is Zephyr-only) -- same reasoning as
 * tests/common/alp_secure_page_mirror.c's identical copy. */
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

/* ------------------------------------------------------------------------
 * Gate 1: array-manifest-vs-mirror field cross-check.
 * ------------------------------------------------------------------------ */

/* Duplicate of main.c mode 3's field_matches() -- see that copy's doc
 * comment for why each field is bound-copied to ITS OWN width into a
 * NUL-terminated buffer before strcmp, rather than raw-memcmp'd. */
static bool field_matches(const char *manifest_field,
                          size_t      manifest_width,
                          const char *mirror_field,
                          size_t      mirror_width)
{
	char   mbuf[ALP_HW_INFO_SERIAL_LEN + 1];
	char   sbuf[ALP_SECURE_PAGE_SERIAL_LEN + 1];
	size_t mcopy = manifest_width < sizeof(mbuf) ? manifest_width : sizeof(mbuf) - 1u;
	size_t scopy = mirror_width < sizeof(sbuf) ? mirror_width : sizeof(sbuf) - 1u;

	memcpy(mbuf, manifest_field, mcopy);
	mbuf[mcopy] = '\0';
	memcpy(sbuf, mirror_field, scopy);
	sbuf[scopy] = '\0';

	return strcmp(mbuf, sbuf) == 0;
}

/* Duplicate of main.c mode 3's array_manifest_agrees_with_mirror(), minus
 * the I2C read: takes the manifest bytes directly (as if already read from
 * 0x50) instead of an eeprom_24c128_t* -- the cross-check LOGIC is what's
 * under test here, not the I2C transfer (covered by
 * tests/common/eeprom_24c128_range.c / eeprom_24c128_read_identity.c). */
static bool manifest_agrees_with_mirror(const uint8_t                  *manifest_raw,
                                        const alp_secure_page_mirror_t *mirror)
{
	alp_hw_info_eeprom_t manifest;
	memcpy(&manifest, manifest_raw, sizeof(manifest));

	if (manifest.magic != ALP_HW_INFO_MAGIC) return false;
	if (manifest.schema_version != ALP_HW_INFO_SCHEMA_VERSION) return false;
	const size_t   covered = sizeof(manifest) - sizeof(manifest.crc32);
	const uint32_t calc    = crc32_iso3309(manifest_raw, covered);
	if (calc != manifest.crc32) return false;

	bool ok = true;
	if (!field_matches(manifest.sku, sizeof(manifest.sku), mirror->sku, sizeof(mirror->sku))) {
		ok = false;
	}
	if (!field_matches(
	        manifest.hw_rev, sizeof(manifest.hw_rev), mirror->hw_rev, sizeof(mirror->hw_rev))) {
		ok = false;
	}
	if (!field_matches(
	        manifest.serial, sizeof(manifest.serial), mirror->serial, sizeof(mirror->serial))) {
		ok = false;
	}
	if (manifest.mfg_year != mirror->mfg_year || manifest.mfg_month != mirror->mfg_month ||
	    manifest.mfg_day != mirror->mfg_day) {
		ok = false;
	}
	return ok;
}

/* Build a self-consistent, CRC-valid array manifest -- the same shape a
 * real scripts/program_eeprom.py output has, so a passing test here means a
 * passing check against real bytes, not just against a hand-rolled
 * struct. */
static void make_manifest(uint8_t     out[sizeof(alp_hw_info_eeprom_t)],
                          const char *sku,
                          const char *hw_rev,
                          const char *serial,
                          uint16_t    mfg_year,
                          uint8_t     mfg_month,
                          uint8_t     mfg_day)
{
	alp_hw_info_eeprom_t m;
	memset(&m, 0, sizeof(m));
	m.magic          = ALP_HW_INFO_MAGIC;
	m.schema_version = ALP_HW_INFO_SCHEMA_VERSION;
	strncpy(m.family, "aen", sizeof(m.family) - 1u);
	strncpy(m.sku, sku, sizeof(m.sku) - 1u);
	strncpy(m.hw_rev, hw_rev, sizeof(m.hw_rev) - 1u);
	strncpy(m.serial, serial, sizeof(m.serial) - 1u);
	m.mfg_year  = mfg_year;
	m.mfg_month = mfg_month;
	m.mfg_day   = mfg_day;
	m.crc32     = crc32_iso3309((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
	memcpy(out, &m, sizeof(m));
}

/* Same idea for the 64-byte mirror -- its own CRC/magic/schema are checked
 * elsewhere in main.c's flow (secure_page_blob_is_sane() and the byte-exact
 * device compare), not by manifest_agrees_with_mirror(), so this helper
 * leaves the mirror's crc32 field at 0. */
static void make_mirror(alp_secure_page_mirror_t *m,
                        const char               *sku,
                        const char               *hw_rev,
                        const char               *serial,
                        uint16_t                  mfg_year,
                        uint8_t                   mfg_month,
                        uint8_t                   mfg_day)
{
	memset(m, 0, sizeof(*m));
	m->magic          = ALP_SECURE_PAGE_MAGIC;
	m->schema_version = ALP_SECURE_PAGE_SCHEMA_VERSION;
	strncpy(m->sku, sku, sizeof(m->sku) - 1u);
	strncpy(m->hw_rev, hw_rev, sizeof(m->hw_rev) - 1u);
	strncpy(m->serial, serial, sizeof(m->serial) - 1u);
	m->mfg_year  = mfg_year;
	m->mfg_month = mfg_month;
	m->mfg_day   = mfg_day;
}

static void test_agrees_when_identical(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(manifest_agrees_with_mirror(manifest, &mirror));
}

/* Gate 1's exact reproduction: the committed board.yaml said
 * sku: E1M-AEN801 but the units being provisioned were E1M-AEN401 -- a
 * mirror generated against that board.yaml is self-consistent (sane
 * magic/schema/CRC) yet carries the WRONG sku for the module it is about
 * to be sealed onto. Killed by: manifest_agrees_with_mirror() returning
 * false (via field_matches() on "sku" disagreeing). */
static void test_refuses_when_sku_disagrees(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN801", "r2", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

/* hw_rev is the second field that resolves from board.yaml, and the one the
 * SoM hw-rev key tracks. A mirror built for the wrong board revision agrees
 * on sku and serial and disagrees only here. */
static void test_refuses_when_hw_rev_disagrees(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r1", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

/* The width asymmetry the NUL-terminated compare exists for: a manifest sku
 * filled to its FULL 24 bytes with no terminator cannot be represented in
 * the mirror's 16-byte sku at all, so the two must never be reported as
 * agreeing. Built with memcpy, not strncpy -- strncpy always leaves a NUL
 * here and so never constructs this case. */
static void test_refuses_when_manifest_field_is_full_width_unterminated(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	alp_hw_info_eeprom_t raw;
	memcpy(&raw, manifest, sizeof(raw));
	memset(raw.sku, 'X', sizeof(raw.sku)); /* all 24 bytes, no terminator */
	raw.crc32 = crc32_iso3309((const uint8_t *)&raw, sizeof(raw) - sizeof(raw.crc32));
	memcpy(manifest, &raw, sizeof(raw));

	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

/* The manifest's hw_rev field (8 bytes) is NARROWER than the mirror's (12),
 * and `serial` sits immediately after it in the manifest. Copying the
 * manifest field at the MIRROR's width would run 4 bytes past hw_rev into
 * serial -- invisible while hw_rev is NUL-terminated, but a full-width
 * 8-char hw_rev has no terminator to stop at, and the two would then be
 * reported as disagreeing when they agree. Each field must be bounded by
 * ITS OWN width; this is the case that proves it. */
static void test_agrees_on_full_width_hw_rev_without_overreading_serial(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	alp_hw_info_eeprom_t raw;
	memcpy(&raw, manifest, sizeof(raw));
	/* Exactly ALP_HW_INFO_HW_REV_LEN bytes, no terminator -- strncpy would
	 * always leave one, so this case can only be built with memcpy. */
	memcpy(raw.hw_rev, "2626-r22", sizeof(raw.hw_rev));
	raw.crc32 = crc32_iso3309((const uint8_t *)&raw, sizeof(raw) - sizeof(raw.crc32));
	memcpy(manifest, &raw, sizeof(raw));

	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "2626-r22", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(manifest_agrees_with_mirror(manifest, &mirror));
}

/* The task also names a typo'd --serial as the same exposure. */
static void test_refuses_when_serial_disagrees(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0099", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

static void test_refuses_when_mfg_date_disagrees(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 5u); /* day differs */

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

/* "No manifest" is NOT "nothing to disagree with" -- a blank/erased array
 * (no ALPH magic) must refuse exactly like a real field disagreement. */
static void test_refuses_when_manifest_absent(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	memset(manifest, 0xFFu, sizeof(manifest)); /* erased EEPROM */
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

static void test_refuses_when_manifest_crc_bad(void)
{
	uint8_t manifest[sizeof(alp_hw_info_eeprom_t)];
	make_manifest(manifest, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);
	manifest[8] ^= 0xFFu; /* corrupt a byte inside `family`, covered by the CRC */
	alp_secure_page_mirror_t mirror;
	make_mirror(&mirror, "E1M-AEN401", "r2", "2026W36-0004", 2026u, 9u, 4u);

	ALP_ASSERT_TRUE(!manifest_agrees_with_mirror(manifest, &mirror));
}

int main(void)
{
	test_agrees_when_identical();
	test_refuses_when_sku_disagrees();
	test_refuses_when_hw_rev_disagrees();
	test_refuses_when_manifest_field_is_full_width_unterminated();
	test_agrees_on_full_width_hw_rev_without_overreading_serial();
	test_refuses_when_serial_disagrees();
	test_refuses_when_mfg_date_disagrees();
	test_refuses_when_manifest_absent();
	test_refuses_when_manifest_crc_bad();

	ALP_TEST_SUMMARY();
}

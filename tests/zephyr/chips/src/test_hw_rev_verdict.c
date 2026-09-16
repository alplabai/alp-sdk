/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2138: examples/aen/aen-evk-demo's Phase 11 r1/r2 IO8-safety gate
 * (hw_rev_verdict.h's aen_evkdemo_hw_rev_confirms_io8_safe()), pulled into
 * its own header for the same reason as sound_verdict.h / amp_fault_
 * verdict.h in that directory -- so it is exercised here, end to end from a
 * raw EEPROM manifest, instead of trusted by a main.c inspection or a
 * pytest substring grep (the round-2 review of this issue found the
 * substring-grep version passed both a stub TODO in place of the guard and
 * the guard's own condition inverted).
 *
 * ROUTED THROUGH alp_hw_info_classify_manifest() ON PURPOSE, not a
 * hand-built (status, hw_rev-string) pair for every case: that function is
 * the SAME one alp_hw_info_read() calls in production (src/zephyr/
 * hw_info_zephyr.c), and it is what actually performs the magic +
 * schema_version + CRC32 validation -- reusing it here means the
 * "blank/bad-magic/bad-schema/bad-CRC all refuse" cases below prove the
 * REAL validation path this app's guard depends on, not a re-description
 * of it that could quietly drift from the real one. make_valid_aen_
 * manifest() below is the same shape as tests/zephyr/hw_info/src/main.c's
 * make_valid_manifest() (a sibling test binary, not reachable to share a
 * helper with directly), composing hw_rev the way scripts/program_eeprom.py
 * / metadata/e1m_modules/aen/hw-revisions.yaml actually do:
 * "<board_datecode>-<rev key>", e.g. "2626-r2".
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"
#include "hw_info_manifest.h"
#include "hw_rev_verdict.h"

static void make_valid_aen_manifest(alp_hw_info_eeprom_t *m, const char *hw_rev)
{
	memset(m, 0, sizeof(*m));
	m->magic          = ALP_HW_INFO_MAGIC;
	m->schema_version = ALP_HW_INFO_SCHEMA_VERSION;
	strcpy(m->family, "aen");
	strcpy(m->sku, "E1M-AEN801");
	strncpy(m->hw_rev, hw_rev, sizeof(m->hw_rev)); /* may leave it unterminated -- see callers */
	strcpy(m->serial, "ALP-AEN801-26W36-00007");
	m->mfg_year  = 2026;
	m->mfg_month = 9;
	m->mfg_day   = 1;
	m->crc32     = alp_hw_info_crc32((const uint8_t *)m, sizeof(*m) - sizeof(m->crc32));
}

ZTEST(alp_chips, test_hw_rev_verdict_confirms_r2)
{
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r2");
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_OK);

	zassert_true(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	             "an exact, CRC-valid \"2626-r2\" manifest must confirm IO8 safe");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_r1)
{
	/* The exact shape issue #2138 reports: r1 routes GPIO_30 to the
	 * carrier's SDIO mux SELECT, not I2S_EN. */
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r1");
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_OK);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "a CRC-valid \"2626-r1\" manifest must NOT confirm IO8 safe");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_blank_erased_eeprom)
{
	alp_hw_info_eeprom_t m;
	memset(&m, 0xFF, sizeof(m)); /* erased flash/EEPROM pattern */
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_NOT_PROVISIONED);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "an unprovisioned (erased) manifest must NOT confirm IO8 safe");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_bad_magic)
{
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r2");
	m.magic = 0xDEADBEEFu; /* neither ALP_HW_INFO_MAGIC nor an erased/zeroed pattern */
	m.crc32 = alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_NOT_PROVISIONED);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "a bad-magic manifest must NOT confirm IO8 safe, even with hw_rev=\"2626-r2\"");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_bad_schema_version)
{
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r2");
	m.schema_version = 99u; /* magic OK, body wrong */
	/* Recompute a VALID crc so this proves the schema check trips first,
	 * mirroring tests/zephyr/hw_info/src/main.c's own bad-schema case. */
	m.crc32 = alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_IO);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "a bad-schema_version manifest must NOT confirm IO8 safe, even with "
	              "hw_rev=\"2626-r2\"");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_bad_crc)
{
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r2");
	m.crc32 ^= 0xFFFFFFFFu; /* corrupt the checksum */
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_ERR_IO);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "a CRC-corrupt manifest must NOT confirm IO8 safe, even with "
	              "hw_rev=\"2626-r2\"");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_prefix_only_match)
{
	/* "2626-r" (no trailing rev digit) is a byte-for-byte PREFIX of
	 * "2626-r2", not the same string -- proves the compare is exact, not
	 * a startswith(). */
	alp_hw_info_eeprom_t m;
	make_valid_aen_manifest(&m, "2626-r");
	alp_hw_info_t info;
	memset(&info, 0, sizeof(info));
	alp_status_t rc = alp_hw_info_classify_manifest(&m, &info);
	zassert_equal(rc, ALP_OK);

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(rc, info.som_hw_rev),
	              "a \"2626-r\" prefix-only hw_rev must NOT confirm IO8 safe");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_unterminated_field)
{
	/* NOT routed through alp_hw_info_classify_manifest(), unlike every
	 * other case above -- its copy_field() helper (src/zephyr/
	 * hw_info_zephyr.c) bounds ANY source to dst_len - 1 bytes and always
	 * appends a NUL, so an 8-byte unterminated EEPROM field would arrive
	 * at aen_evkdemo_hw_rev_confirms_io8_safe() as a SAFELY-TRUNCATED,
	 * terminated "2626-r2" -- which legitimately matches, and would make
	 * this case indistinguishable from test_hw_rev_verdict_confirms_r2
	 * (confirmed: routing it through classify_manifest() first made this
	 * assertion fail wrongly, exactly that way). What this test actually
	 * has to prove is aen_evkdemo_hw_rev_confirms_io8_safe()'s OWN
	 * defensive bound on an already-8-byte-full, NUL-free buffer -- the
	 * shape its signature accepts and its file comment documents -- so it
	 * calls the function directly with a hand-built one instead. */
	char unterminated[ALP_HW_INFO_HW_REV_LEN];
	zassert_equal(sizeof(unterminated), (size_t)8, "test assumes ALP_HW_INFO_HW_REV_LEN == 8");
	memcpy(unterminated, "2626-r2X", sizeof(unterminated)); /* fills the field, no NUL */

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(ALP_OK, unterminated),
	              "an unterminated \"2626-r2X\" field must NOT confirm IO8 safe");
}

ZTEST(alp_chips, test_hw_rev_verdict_refuses_read_error)
{
	/* A transport-level failure (EEPROM unreachable, bus not configured,
	 * ...) never even reaches alp_hw_info_classify_manifest() in
	 * production -- alp_hw_info_read() returns the raw status straight
	 * from the I2C layer. Simulated directly here: the rc gate must
	 * refuse regardless of what hw_rev bytes happen to be sitting in an
	 * out-parameter that was never actually populated by a successful
	 * read. */
	char would_be_r2[ALP_HW_INFO_HW_REV_LEN];
	memcpy(would_be_r2, "2626-r2", sizeof(would_be_r2));

	zassert_false(aen_evkdemo_hw_rev_confirms_io8_safe(ALP_ERR_NOT_READY, would_be_r2),
	              "a read error must refuse regardless of hw_rev content");
}

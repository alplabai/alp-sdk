/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #1700 / ADR-0030: white-box native_sim coverage for the
 * SERAM-floor diagnostic warning in src/backends/soc_info/alif_se.c
 * (alif_se_seram_from_banner() / alif_se_warn_if_seram_below_floor()).
 * This is a MITIGATION, not a fix for the underlying clock drop -- it
 * only proves the SDK now surfaces a mismatched-pair module instead of
 * staying silent; it says nothing about whether the drop itself is
 * cured (that is the open, Alif-side half of #1700).
 *
 * Compiles the REAL src/backends/soc_info/alif_se.c translation unit
 * (via the #include below -- same technique as
 * tests/unit/se_cryptocell_hash_bounds/src/test_se_hash_bounds.c) and
 * calls the SHIPPED alif_se_seram_from_banner() / alif_se_read()
 * directly, since both are file-scope `static` and therefore visible
 * here after the #include.
 *
 * alif_se.c is gated behind CONFIG_ALP_SDK_SOC_INFO_ALIF_SE, which
 * `depends on` the AEN801/E8-only hal_alif Kconfig symbol
 * HAS_ALIF_SE_SERVICES and so can never be selected on native_sim
 * through the real Kconfig/zephyr_library path -- this test instead
 * fakes the macro directly at the preprocessor level (mirrors
 * test_se_hash_bounds.c's CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL fake)
 * and fakes <se_service.h> (see fake_se_service_include/).
 */

/* Faked purely at the preprocessor level -- no real
 * ALP_SDK_SOC_INFO_ALIF_SE Kconfig symbol exists in this image (see
 * this directory's CMakeLists.txt for why CONFIG_ALP_SDK is
 * deliberately never set here). Toggles ONLY alif_se.c's own
 * top-of-file `#if defined(...)` body guard. */
#define CONFIG_ALP_SDK_SOC_INFO_ALIF_SE 1

#include "../../../../src/backends/soc_info/alif_se.c"

#include <string.h>

#include <zephyr/ztest.h>

/* ------------------------------------------------------------------ */
/* se_service_* stubs -- controllable per test                        */
/* ------------------------------------------------------------------ */

static const char *fake_revision_banner = "SES A0 v1.110.0 Mar 4 2026";

int se_service_get_se_revision(uint8_t *rev)
{
	size_t len = strlen(fake_revision_banner);

	memset(rev, 0, VERSION_RESPONSE_LENGTH);
	memcpy(rev, fake_revision_banner, MIN(len, (size_t)VERSION_RESPONSE_LENGTH));
	return 0;
}

int se_service_get_device_part_number(uint32_t *out)
{
	*out = 0u;
	return 0;
}

int se_service_system_get_device_data(get_device_revision_data_t *out)
{
	memset((void *)out, 0, sizeof(*out));
	return 0;
}

int se_service_heartbeat(void)
{
	return 0;
}

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static void reset_state(void *fixture)
{
	(void)fixture;
	alif_se_seram_floor_warned = false;
	fake_revision_banner       = "SES A0 v1.110.0 Mar 4 2026";
}

ZTEST_SUITE(alif_se_seram_floor_warning, NULL, NULL, reset_state, NULL, NULL);

/* Alif name a SERAM release by the middle field of the banner -- prove
 * the parser reads that field on both bench-captured strings from
 * #1700 (docs/aen-se-services.md section 0.1). */
ZTEST(alif_se_seram_floor_warning, test_parses_middle_field_as_seram_version)
{
	uint32_t seram = 0u;

	zassert_true(alif_se_seram_from_banner("SES A0 v1.110.0 Mar 4 2026", &seram));
	zassert_equal(seram, 110u, "v1.110.0 is SERAM v110");

	zassert_true(alif_se_seram_from_banner("SES A0 v1.106.2 Jul 14 2025", &seram));
	zassert_equal(seram, 106u, "v1.106.2 is SERAM v106 -- the customer's #1700 module");
}

/* A banner shape the parser does not recognise must be left unflagged,
 * not guessed at -- the header comment's explicit contract. */
ZTEST(alif_se_seram_floor_warning, test_unrecognised_banner_does_not_parse)
{
	uint32_t seram = 999u;

	zassert_false(alif_se_seram_from_banner("garbage, no version here", &seram));
	zassert_equal(seram, 999u, "a parse miss must not touch the output");
}

/* The #1700 reproduction case: SERAM v106 (below the ADR-0030 v110
 * floor) must warn exactly once per read. */
ZTEST(alif_se_seram_floor_warning, test_read_below_floor_warns_once)
{
	fake_revision_banner = "SES A0 v1.106.2 Jul 14 2025";

	alp_soc_info_t out = { 0 };

	zassert_equal(alif_se_read(&out), ALP_OK);
	zassert_true(alif_se_seram_floor_warned, "v106 is below the v110 ADR-0030 floor");
	zassert_true(strstr(out.secure_fw_version, "v1.106.2") != NULL,
	             "secure_fw_version must still carry the full banner");

	/* A second read must not re-arm anything the warn function already
	 * fired -- the flag stays latched (verified indirectly: the flag
	 * remains true, and alif_se_read() itself never resets it). */
	zassert_equal(alif_se_read(&out), ALP_OK);
	zassert_true(alif_se_seram_floor_warned);
}

/* The healthy reference-board case (v110, matched pair): must NOT warn. */
ZTEST(alif_se_seram_floor_warning, test_read_at_floor_does_not_warn)
{
	fake_revision_banner = "SES A0 v1.110.0 Mar 4 2026";

	alp_soc_info_t out = { 0 };

	zassert_equal(alif_se_read(&out), ALP_OK);
	zassert_false(alif_se_seram_floor_warned, "v110 meets the ADR-0030 floor");
}

/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2469: the boot banner marks the core the image is built for, not the
 * board.yaml slice's core.  The E8 list below is what alp_orchestrate.py
 * emits for an app whose board.yaml declares only m55_hp.
 */

#include <zephyr/ztest.h>

#include "alp_soc_cpus.h"

#define E8_HP_SLICE \
	"M55-HP @400MHz (active)|rtss_hp + 2x Cortex-A32 @800MHz + M55-HE @160MHz|rtss_he"

static char buf[160];

ZTEST(soc_cpus_active_core, test_he_build_of_hp_slice_marks_he)
{
	alp_soc_cpus_format(
	    buf, sizeof(buf), E8_HP_SLICE, "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he");
	zassert_str_equal(buf, "M55-HE @160MHz (active) + M55-HP @400MHz + 2x Cortex-A32 @800MHz");
}

ZTEST(soc_cpus_active_core, test_matching_build_keeps_order)
{
	alp_soc_cpus_format(
	    buf, sizeof(buf), E8_HP_SLICE, "alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp");
	zassert_str_equal(buf, "M55-HP @400MHz (active) + 2x Cortex-A32 @800MHz + M55-HE @160MHz");
}

ZTEST(soc_cpus_active_core, test_no_matching_cluster_keeps_orchestrator_marker)
{
	alp_soc_cpus_format(buf, sizeof(buf), E8_HP_SLICE, "native_sim/native/64");
	zassert_str_equal(buf, "M55-HP @400MHz (active) + 2x Cortex-A32 @800MHz + M55-HE @160MHz");
}

ZTEST(soc_cpus_active_core, test_truncates_without_overrun)
{
	char small[10];

	alp_soc_cpus_format(small, sizeof(small), E8_HP_SLICE, "x/rtss_he");
	zassert_str_equal(small, "M55-HE @1");
}

ZTEST_SUITE(soc_cpus_active_core, NULL, NULL, NULL, NULL, NULL);

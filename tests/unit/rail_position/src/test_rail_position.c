/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rail_position (haversine chainage / segments / RMC).
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "rail_position.h"

ZTEST_SUITE(rail_position, NULL, NULL, NULL, NULL, NULL);

ZTEST(rail_position, test_haversine_one_degree_latitude)
{
	/* 1 deg of latitude is ~111.19 km on a sphere of R=6371 km. */
	double d = rail_pos_haversine_m(0.0, 0.0, 1.0, 0.0);
	zassert_within(d, 111195.0, 200.0, "1 deg latitude ~= 111.2 km");
}

ZTEST(rail_position, test_chainage_accumulates_and_bins)
{
	struct rail_pos_state st;
	rail_pos_init(&st, 25.0f);

	/* First fix seeds position, no advance. */
	bool adv = rail_pos_update(&st, 0.0, 0.0, true);
	zassert_false(adv, "first fix does not advance a segment");
	zassert_equal(st.segment_index, 0, "start in segment 0");

	/* Step ~115 m north (0.0010342 deg lat ~= 115 m at R=6371000, mid-segment-4). */
	adv = rail_pos_update(&st, 0.0010342, 0.0, true); /* ~115 m, seg 4 centre */
	zassert_within(st.chainage_m, 115.0, 2.0, "chainage ~115 m");
	zassert_equal(st.segment_index, 4, "115 m / 25 m = segment 4");
	zassert_true(adv, "crossed into a new segment");
}

ZTEST(rail_position, test_no_fix_holds_chainage)
{
	struct rail_pos_state st;
	rail_pos_init(&st, 25.0f);
	rail_pos_update(&st, 0.0, 0.0, true);
	rail_pos_update(&st, 0.0010342, 0.0, true);
	double held = st.chainage_m;

	bool adv = rail_pos_update(&st, 5.0, 5.0, false); /* no fix: ignored */
	zassert_false(adv, "no-fix update does not advance");
	zassert_equal(st.chainage_m, held, "no-fix update does not move chainage");
}

ZTEST(rail_position, test_parse_rmc)
{
	/* A valid $GNRMC: status A, lat 5919.9999 N, lon 01803.7440 E, 12.0 kn. */
	const char *s   = "$GNRMC,083559.00,A,5919.99990,N,01803.74400,E,12.0,0.0,250626,,,A*XX";
	double      lat = 0, lon = 0;
	float       spd = -1;
	bool        fix = false;
	bool        ok  = rail_pos_parse_rmc(s, &lat, &lon, &spd, &fix);
	zassert_true(ok, "RMC parsed");
	zassert_true(fix, "status A -> fix");
	zassert_within(lat, 59.3333, 0.01, "lat decimal degrees");
	zassert_within(lon, 18.0624, 0.01, "lon decimal degrees");
	zassert_within((double)spd, 6.17, 0.1, "12 knots -> ~6.17 m/s");
}

ZTEST(rail_position, test_parse_rmc_void_is_no_fix)
{
	const char *s   = "$GNRMC,083559.00,V,,,,,,,250626,,,N*XX";
	double      lat = 0, lon = 0;
	float       spd = -1;
	bool        fix = true;
	bool        ok  = rail_pos_parse_rmc(s, &lat, &lon, &spd, &fix);
	zassert_true(ok, "RMC recognised");
	zassert_false(fix, "status V -> no fix");
}

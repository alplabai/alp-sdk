/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_position implementation -- see rail_position.h.
 */
#include "rail_position.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RAIL_EARTH_R_M 6371000.0 /* mean Earth radius (haversine standard) */
#define RAIL_DEG2RAD   (M_PI / 180.0)
#define RAIL_KNOT_MPS  0.514444f

void rail_pos_init(struct rail_pos_state *st, float segment_len_m)
{
	memset(st, 0, sizeof(*st));
	st->segment_len_m = (segment_len_m > 0.0f) ? segment_len_m : 25.0f;
}

double rail_pos_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
	double dlat = (lat2 - lat1) * RAIL_DEG2RAD;
	double dlon = (lon2 - lon1) * RAIL_DEG2RAD;
	double a = sin(dlat / 2.0) * sin(dlat / 2.0) + cos(lat1 * RAIL_DEG2RAD) *
	                                                   cos(lat2 * RAIL_DEG2RAD) * sin(dlon / 2.0) *
	                                                   sin(dlon / 2.0);
	double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
	return RAIL_EARTH_R_M * c;
}

bool rail_pos_update(struct rail_pos_state *st, double lat, double lon, bool has_fix)
{
	if (!has_fix) {
		return false;
	}
	int32_t prev_seg = st->segment_index;
	if (st->have_last) {
		st->chainage_m += rail_pos_haversine_m(st->last_lat, st->last_lon, lat, lon);
		st->segment_index = (int32_t)(st->chainage_m / (double)st->segment_len_m);
	}
	st->last_lat  = lat;
	st->last_lon  = lon;
	st->have_last = true;
	return st->segment_index != prev_seg;
}

/* Convert ddmm.mmmm + hemisphere to signed decimal degrees. */
static double nmea_to_deg(const char *field, char hemi)
{
	if (field == NULL || field[0] == '\0') {
		return 0.0;
	}
	double v       = atof(field);
	double degrees = floor(v / 100.0);
	double minutes = v - degrees * 100.0;
	double dec     = degrees + minutes / 60.0;
	if (hemi == 'S' || hemi == 'W') {
		dec = -dec;
	}
	return dec;
}

bool rail_pos_parse_rmc(const char *nmea, double *lat, double *lon, float *speed_mps, bool *has_fix)
{
	if (nmea == NULL) {
		return false;
	}
	/* Accept any talker: $__RMC (e.g. GNRMC, GPRMC). */
	if (!(nmea[0] == '$' && nmea[3] == 'R' && nmea[4] == 'M' && nmea[5] == 'C')) {
		return false;
	}

	/* Tokenise a local copy on commas.  RMC fields:
	 * 0:$xxRMC 1:time 2:status 3:lat 4:N/S 5:lon 6:E/W 7:speed(kn) ... */
	char buf[128];
	strncpy(buf, nmea, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *fields[16] = { 0 };
	int   nf         = 0;
	char *p          = buf;
	fields[nf++]     = p;
	for (; *p && nf < 16; p++) {
		if (*p == ',') {
			*p           = '\0';
			fields[nf++] = p + 1;
		}
	}

	bool fix = (nf > 2 && fields[2][0] == 'A');
	if (has_fix) {
		*has_fix = fix;
	}
	if (fix && nf > 7) {
		if (lat) {
			*lat = nmea_to_deg(fields[3], fields[4][0]);
		}
		if (lon) {
			*lon = nmea_to_deg(fields[5], fields[6][0]);
		}
		if (speed_mps) {
			*speed_mps = (float)atof(fields[7]) * RAIL_KNOT_MPS;
		}
	}
	return true;
}

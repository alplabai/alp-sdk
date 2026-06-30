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

/*
 * Physical constants.
 *
 * RAIL_EARTH_R_M  Mean Earth radius in metres (6 371 000 m).
 *   The Earth is an oblate spheroid; its equatorial radius is
 *   ~6 378 137 m and its polar radius is ~6 356 752 m.  The MEAN
 *   radius (radius of a sphere of equal volume) is 6 371 008.8 m,
 *   rounded here to 6 371 000 m.  The haversine formula assumes a
 *   perfect sphere; the mean radius gives a maximum distance error
 *   of ~0.33 % compared with the WGS84 ellipsoid.  For rail survey
 *   purposes (segment lengths of 25 m, GPS accuracy of 2-5 m) this
 *   error is negligible.
 *
 * RAIL_DEG2RAD  Conversion factor from degrees to radians (pi/180).
 *   All trigonometric functions in <math.h> operate in radians; GNSS
 *   coordinates arrive in decimal degrees, so every latitude and
 *   longitude must be multiplied by this factor before use.
 *
 * RAIL_KNOT_MPS  Knots to metres-per-second conversion (0.514444 m/s/kn).
 *   One knot = one nautical mile per hour = 1852 m / 3600 s.
 *   NMEA $--RMC reports speed over ground in knots; multiplying by
 *   this constant converts to SI units (m/s) for the wavelength
 *   computation (lambda = speed_mps / dom_freq_hz).
 */
#define RAIL_EARTH_R_M 6371000.0 /* mean Earth radius (haversine standard) */
#define RAIL_DEG2RAD   (M_PI / 180.0)
#define RAIL_KNOT_MPS  0.514444f

/*
 * rail_pos_init -- initialise position-tracking state.
 *
 * Zeroes all fields and sets the segment length.  The default segment
 * length of 25 m is used when the caller passes zero or a negative
 * value; 25 m corresponds to a standard EN 13848 short-wavelength
 * measurement base and gives ~40 samples/km at typical survey speeds.
 */
void rail_pos_init(struct rail_pos_state *st, float segment_len_m)
{
	memset(st, 0, sizeof(*st));
	st->segment_len_m = (segment_len_m > 0.0f) ? segment_len_m : 25.0f;
}

/*
 * rail_pos_haversine_m -- great-circle distance between two WGS84 points.
 *
 * HAVERSINE FORMULA
 * -----------------
 * The haversine formula computes the shortest-arc distance on the
 * surface of a sphere (the great-circle distance).  For two points
 * (lat1, lon1) and (lat2, lon2) in decimal degrees:
 *
 *   dlat = (lat2 - lat1) * DEG2RAD
 *   dlon = (lon2 - lon1) * DEG2RAD
 *   a    = sin^2(dlat/2) + cos(lat1)*cos(lat2)*sin^2(dlon/2)
 *   c    = 2 * atan2( sqrt(a), sqrt(1-a) )
 *   d    = R * c
 *
 * The intermediate value 'a' is the square of the haversine of the
 * central angle; it lies in [0, 1].  Using atan2(sqrt(a), sqrt(1-a))
 * instead of asin(sqrt(a)) improves numerical stability for nearly
 * antipodal points where a approaches 1, and also avoids domain errors
 * if floating-point rounding pushes a slightly above 1.
 *
 * All four coordinate arguments are in DECIMAL DEGREES (not the NMEA
 * ddmm.mmmm format); call nmea_to_deg first when parsing NMEA strings.
 *
 * The function returns metres as a double to preserve sub-metre
 * precision for the chainage accumulation loop.
 */
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

/*
 * rail_pos_update -- feed one GNSS sample into the chainage accumulator.
 *
 * CHAINAGE ACCUMULATION
 * ---------------------
 * Chainage is the along-track running distance from the survey start
 * point, accumulated by summing haversine distances between successive
 * GNSS fixes.  Each new fix extends chainage by the distance from the
 * previous fix:
 *
 *   chainage_m += haversine(prev_lat, prev_lon, lat, lon)
 *
 * SEGMENT BINNING
 * ---------------
 * The segment index is simply the integer part of the chainage divided
 * by the segment length:
 *
 *   segment_index = floor( chainage_m / segment_len_m )
 *
 * The function returns true when segment_index advances (i.e., the
 * train crosses into the next segment), enabling the caller to trigger
 * a per-segment feature aggregation step without tracking the previous
 * index itself.
 *
 * NO-FIX HOLD
 * -----------
 * When has_fix is false (NMEA status 'V' = void), the sample is
 * ignored entirely: chainage and segment_index are not updated, and
 * the function returns false immediately.  This prevents GPS outages
 * (tunnels, bridges) from creating phantom distance increments that
 * would corrupt the chainage record.
 *
 * FIRST-FIX BOOTSTRAP
 * --------------------
 * On the very first call with a valid fix, have_last is false so no
 * distance is added; the fix is simply recorded as the starting point.
 * Distance accumulation begins on the second fix.
 */
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

/*
 * nmea_to_deg -- convert NMEA ddmm.mmmm + hemisphere to decimal degrees.
 *
 * NMEA COORDINATE ENCODING
 * -------------------------
 * NMEA 0183 encodes coordinates in the format ddmm.mmmm where:
 *   dd    = integer degrees (may be 2 or 3 digits for longitude)
 *   mm    = integer minutes
 *   .mmmm = decimal fraction of a minute
 *
 * Example: latitude "5930.1500" with hemisphere 'N' means
 *   59 degrees 30.1500 minutes North
 *   = 59 + 30.1500/60 = 59.5025 decimal degrees.
 *
 * CONVERSION STEPS
 * ----------------
 * 1. Parse the raw string as a floating-point number v (e.g., 5930.15).
 * 2. Extract integer degrees: deg = floor(v / 100).
 * 3. Extract minutes: min = v - deg * 100  (e.g., 30.15).
 * 4. Convert: dec = deg + min / 60.0.
 * 5. Apply sign: S (South) and W (West) hemispheres are negative.
 *
 * A NULL or empty field string returns 0.0 (coordinates unavailable).
 */
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

/*
 * rail_pos_parse_rmc -- parse an NMEA $--RMC sentence.
 *
 * RMC SENTENCE FORMAT
 * -------------------
 * RMC = "Recommended Minimum navigation information".  Example:
 *
 *   $GNRMC,123519,A,5930.1500,N,01831.7600,E,022.4,084.4,230394,003.1,W*6A
 *
 * Field index (0-based, comma-separated):
 *   [0]  Sentence ID ($GPRMC, $GNRMC, $GLRMC, ...)
 *   [1]  UTC time (hhmmss.ss)
 *   [2]  Status: 'A' = active (valid fix), 'V' = void (no fix)
 *   [3]  Latitude (ddmm.mmmm)
 *   [4]  N/S hemisphere
 *   [5]  Longitude (dddmm.mmmm)
 *   [6]  E/W hemisphere
 *   [7]  Speed over ground in KNOTS
 *   [8]  Track angle (degrees true)
 *   [9]  Date (ddmmyy)
 *   ...  (magnetic variation, checksum, etc.)
 *
 * TALKER PREFIX DETECTION
 * ------------------------
 * The function accepts any two-character talker prefix ($GP, $GN, $GL,
 * $GB, ...) by checking only positions 3-5 for 'R', 'M', 'C'.  This
 * makes the parser work with single-constellation GPS receivers ($GPRMC)
 * and multi-constellation GNSS chipsets ($GNRMC) without configuration.
 *
 * TOKENISATION
 * ------------
 * The sentence is copied to a local buffer and tokenised in-place by
 * replacing commas with NUL bytes.  This avoids heap allocation and is
 * safe on any platform; the copy also preserves the caller's string.
 *
 * SPEED CONVERSION
 * ----------------
 * NMEA reports speed in knots (nautical miles per hour).  The result is
 * converted to m/s by multiplying by RAIL_KNOT_MPS = 0.514444 m/s/kn
 * (1 nautical mile = 1852 m; 1 hour = 3600 s; 1852/3600 = 0.514444).
 */
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

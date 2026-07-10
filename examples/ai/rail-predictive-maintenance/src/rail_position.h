/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_position -- pure-C geotagging for the rail survey: turn GNSS
 * fixes into an along-track distance (chainage) and a fixed-length
 * segment index, and parse the minimum NMEA needed (RMC).  Arch-neutral
 * (stdint/math only); host-unit-tested.
 */
#ifndef RAIL_POSITION_H
#define RAIL_POSITION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Running geotag state.
 *
 * Holds the previous GNSS fix for incremental haversine distance steps,
 * the accumulated along-track distance (chainage), and the derived
 * segment index for per-segment feature aggregation.
 */
struct rail_pos_state {
	double  last_lat;      /**< Latitude of the previous valid fix (decimal degrees). */
	double  last_lon;      /**< Longitude of the previous valid fix (decimal degrees). */
	bool    have_last;     /**< False until the first valid fix has been received. */
	double  chainage_m;    /**< accumulated along-track distance. */
	int32_t segment_index; /**< floor(chainage / segment_len_m). */
	float   segment_len_m; /**< segment size (default 25 m). */
};

/**
 * Initialise position-tracking state.
 *
 * Zeroes all fields; @p segment_len_m <= 0 falls back to the default
 * 25 m segment length.
 */
void rail_pos_init(struct rail_pos_state *st, float segment_len_m);

/**
 * Great-circle distance between two WGS84 points in metres.
 *
 * Uses the haversine formula with mean Earth radius 6 371 000 m;
 * all four coordinates must be in decimal degrees (not NMEA ddmm.mmmm).
 */
double rail_pos_haversine_m(double lat1, double lon1, double lat2, double lon2);

/**
 * Feed one GNSS sample.  With @p has_fix, accumulates distance from the
 * previous fixed point into chainage and recomputes the segment index;
 * without a fix the sample is ignored (chainage holds).  Returns true
 * iff the segment index advanced.
 */
bool rail_pos_update(struct rail_pos_state *st, double lat, double lon, bool has_fix);

/**
 * Parse a $--RMC sentence.  On success sets decimal-degree @p lat/@p lon,
 * @p speed_mps, and @p has_fix (status 'A' = fix, 'V' = void).  Returns
 * false if the sentence is not an RMC line.
 */
bool rail_pos_parse_rmc(const char *nmea,
                        double     *lat,
                        double     *lon,
                        float      *speed_mps,
                        bool       *has_fix);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_POSITION_H */

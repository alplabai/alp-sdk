/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/temperature.h> -- Linux (Yocto) backend.
 *
 * alp_temperature_read_die_milli_c() reads the kernel's thermal zones
 * (`/sys/class/thermal/thermal_zoneN/{type,temp}`), which is how the
 * RZ/V2N's two on-die TSU units surface on Linux.  Zones are picked by
 * their `type` string ("cpu-thermal0", "cpu-thermal1" in the vendor
 * dtsi), never by index: the thermal_zoneN number follows registration
 * order, so a kernel or DT change can reshuffle it.  The kernel owns the
 * TSU; this file only reads sysfs.
 *
 * alp_temperature_read_milli_c() (the on-module ambient sensor) has no
 * Linux implementation: NOSUPPORT, kept separate per issue #2066.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

#ifndef ALP_THERMAL_SYSFS_ROOT
#define ALP_THERMAL_SYSFS_ROOT "/sys/class/thermal"
#endif

/* Zone types that are the SoC die.  Prefix match: the kernel appends the
 * unit number ("cpu-thermal0"). */
#define SOC_ZONE_TYPE_PREFIX "cpu-thermal"

/* Upper bound on thermal_zoneN indices probed; missing ones are skipped. */
#define MAX_ZONES 32

alp_status_t alp_temperature_read_milli_c(int32_t *milli_c)
{
	if (milli_c == NULL) return ALP_ERR_INVAL;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_temperature_read_die_milli_c(int32_t *milli_c)
{
	if (milli_c == NULL) return ALP_ERR_INVAL;

	int     matched = 0;
	int     read_ok = 0;
	int32_t hottest = 0;

	for (int i = 0; i < MAX_ZONES; i++) {
		char path[96];
		char type[48];

		snprintf(path, sizeof path, ALP_THERMAL_SYSFS_ROOT "/thermal_zone%d/type", i);
		FILE *f = fopen(path, "r");
		if (f == NULL) continue; /* hole or no such zone */
		char *got = fgets(type, sizeof type, f);
		fclose(f);
		if (got == NULL ||
		    strncmp(type, SOC_ZONE_TYPE_PREFIX, sizeof SOC_ZONE_TYPE_PREFIX - 1) != 0) {
			continue;
		}
		matched++;

		snprintf(path, sizeof path, ALP_THERMAL_SYSFS_ROOT "/thermal_zone%d/temp", i);
		f = fopen(path, "r");
		if (f == NULL) continue;
		long mc = 0;
		int  n  = fscanf(f, "%ld", &mc); /* sysfs temp is already milli-degC */
		fclose(f);
		if (n != 1) continue;

		if (!read_ok || (int32_t)mc > hottest) hottest = (int32_t)mc;
		read_ok = 1;
	}

	if (matched == 0) return ALP_ERR_NOSUPPORT;
	if (!read_ok) return ALP_ERR_IO;
	*milli_c = hottest;
	return ALP_OK;
}

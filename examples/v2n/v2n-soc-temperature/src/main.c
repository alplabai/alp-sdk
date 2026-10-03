/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-soc-temperature -- print the SoC die temperature once per second.
 *
 * Two different temperatures exist on a V2N SoM, and the SDK keeps them
 * apart on purpose:
 *
 *   - alp_temperature_read_milli_c()      the on-module AMBIENT sensor
 *                                         (see examples/v2n/v2n-temp-sensor
 *                                         for the chip-level read);
 *   - alp_temperature_read_soc_milli_c()  the processor's own JUNCTION
 *                                         temperature -- this example.
 *
 * Under Linux the SoC die temperature comes from the kernel's thermal
 * framework: the RZ/V2N's two on-die TSU units appear as thermal zones
 * whose `type` is "cpu-thermal0" / "cpu-thermal1".  The SDK picks zones
 * by that type string (the thermal_zoneN index follows probe order and
 * is not stable) and returns the hottest one, which is the number a
 * throttle or shutdown decision cares about.  The kernel keeps owning
 * the TSU; this app never touches its registers, so it can run next to
 * anything else on the A55.
 *
 * Nothing here has been run on silicon yet.
 */

#include <stdio.h>
#include <unistd.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

int main(void)
{
	printf("[soc-temp] v2n-soc-temperature\n");

	/* Ten samples, one second apart.  Real firmware would feed the
	 * value to a fan curve, a logger, or an over-temperature alert. */
	for (int i = 0; i < 10; ++i) {
		int32_t      milli_c = 0;
		alp_status_t s       = alp_temperature_read_soc_milli_c(&milli_c);

		if (s == ALP_ERR_NOSUPPORT) {
			/* No matching thermal zone: not a Linux build, or a
			 * kernel without the TSU driver.  Retrying cannot help. */
			printf("[soc-temp] no SoC thermal source on this build\n");
			return 1;
		}
		if (s != ALP_OK) {
			printf("[soc-temp] sample %d: read failed (status=%d)\n", i, (int)s);
		} else {
			/* Split the magnitude, not milli_c: integer division
			 * truncates toward zero, so -500 / 1000 == 0 would drop
			 * the sign of -0.5 degC.  Print "-" by hand instead. */
			int32_t abs_mc = milli_c < 0 ? -milli_c : milli_c;
			printf("[soc-temp] sample %d: %s%d.%03d degC\n",
			       i,
			       milli_c < 0 ? "-" : "",
			       (int)(abs_mc / 1000),
			       (int)(abs_mc % 1000));
		}
		sleep(1);
	}

	printf("[soc-temp] done\n");
	return 0;
}

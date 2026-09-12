/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-backed implementation of <alp/temperature.h>.
 *
 * Binds the metadata-emitted `alp-temp0` devicetree alias through the
 * upstream Zephyr sensor API -- deliberately NOT by opening the I2C bus
 * directly over `chips/tmp112` + `alp_i2c_open()`.  AEN's BRD_I2C is
 * standard-mode with no external pull-up
 * (metadata/e1m_modules/aen/on-module-links.yaml), and `alp_i2c`'s
 * `z_open` re-runs `i2c_configure()` with the caller's bitrate and has no
 * exclusivity check (src/backends/i2c/zephyr_drv.c) -- a portable open at
 * `alp_hw_info`'s 400 kHz would both silently reconfigure the bus under
 * an app's own handle and be a real electrical fault on this board.  The
 * DT-bound path here inherits the bus's own `clock-frequency` instead.
 * See issue #2066.
 *
 * Despite the `_zephyr` filename this file is OS-agnostic, like its
 * `hw_info_zephyr.c` sibling: every Zephyr symbol below is compiled out
 * (gated on CONFIG_SENSOR, only ever defined under a real Zephyr Kconfig
 * build) on a plain-CMake / baremetal / Yocto build, so a single
 * translation unit -- no dispatch registry, no ops header, no handle
 * pool -- covers every OS target, and "every non-Zephyr build returns
 * ALP_ERR_NOSUPPORT" falls out of that for free.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

#if defined(CONFIG_SENSOR)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#endif

/*
 * The on-module sensor, resolved via a dedicated DT alias so the SoM
 * preset's own device node decides presence -- never a compiled-in
 * vendor compatible string.  `alp-temp0` joins the existing alp-prefixed
 * portable-alias namespace (`alp-i2cN`, `alp-lprtc-counter`).
 *
 * Gated on BOTH CONFIG_SENSOR and the alias's own DT status: an app that
 * never turns on the sensor subsystem, or a board whose alias node is
 * absent/disabled, both collapse to the NOSUPPORT stub below -- same
 * "absent means NOSUPPORT" contract as CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
 * unset does for <alp/hw_info.h>.
 */
#if defined(CONFIG_SENSOR) && DT_NODE_HAS_STATUS(DT_ALIAS(alp_temp0), okay)
#define ALP_TEMPERATURE_SENSOR_ENABLED 1
static const struct device *const _temp_dev = DEVICE_DT_GET(DT_ALIAS(alp_temp0));
#else
#define ALP_TEMPERATURE_SENSOR_ENABLED 0
#endif

alp_status_t alp_temperature_read_milli_c(int32_t *milli_c)
{
	if (milli_c == NULL) return ALP_ERR_INVAL;

#if !ALP_TEMPERATURE_SENSOR_ENABLED
	/* No on-module sensor on this build: either the SoM preset declares
     * none (e.g. E1M-NX9101), the target's board tree doesn't emit the
     * alias yet (every non-AEN target today, including V2N -- see the
     * header's "Today's coverage" note), or the app never turned on
     * CONFIG_SENSOR. */
	return ALP_ERR_NOSUPPORT;
#else
	if (!device_is_ready(_temp_dev)) return ALP_ERR_NOT_READY;

	struct sensor_value val;
	int                 rc = sensor_sample_fetch_chan(_temp_dev, SENSOR_CHAN_AMBIENT_TEMP);
	if (rc == 0) {
		rc = sensor_channel_get(_temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	}
	if (rc != 0) return ALP_ERR_IO;

	/* Integer milli-degrees C -- the same conversion
     * examples/aen/aen-temp-sensor and the boot banner already use; no
     * float printf anywhere in this path. */
	*milli_c = (int32_t)sensor_value_to_milli(&val);
	return ALP_OK;
#endif
}

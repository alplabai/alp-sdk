/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-only implementation of <alp/temperature.h>.
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
 * The device handle is resolved at RUNTIME, never via `DEVICE_DT_GET()`.
 * `DEVICE_DT_GET()` emits a link-time reference to the generated
 * `__device_dts_ord_N` object for that node -- which exists only if some
 * driver actually instantiated it.  The `alp-temp0` node can be present
 * and `okay`, with `CONFIG_SENSOR=y`, while no driver binds it:
 * `scripts/gen_zephyr_board.py` emits `config TMP112` / `default n` into
 * the AEN801 board `Kconfig.defconfig` (issue #2043) because
 * `chips/tmp112/tmp112.c` and upstream `zephyr/drivers/sensor/ti/tmp112`
 * both define `tmp112_init()` and collide at link time; an app that does
 * not opt back in with its own `CONFIG_TMP112=y` (as
 * `examples/aen/aen-temp-sensor` does) builds with the node
 * un-instantiated.  `DEVICE_DT_GET()` there is an undefined reference --
 * but only in that exact combination, `CONFIG_SENSOR=y` AND the alias
 * present/`okay` AND no driver bound: the `#if` gate below already keeps
 * the reference out of any build failing one of the first two conditions
 * (that guard is necessary, just not sufficient); it is the third
 * condition this file has no compile-time way to see (issue #2066
 * follow-up).
 *
 * Nor is plain `device_get_binding()` a full fix by itself: Zephyr's own
 * `z_impl_device_get_binding()` (kernel/device.c) returns NULL both when
 * no device matches the name AND when a device matches but never became
 * ready -- e.g. a TMP112 that IS instantiated but whose init failed
 * because it NACKs at its configured address (the #1978 failure mode).
 * Folding that into the same `ALP_ERR_NOSUPPORT` as "no sensor on this
 * SoM" would make a fitted-but-dead sensor look identical, on the boot
 * banner -- the one place a human reads this -- to a SoM with no sensor
 * at all.  So this file scans the static device table itself and calls
 * `device_is_ready()` separately, keeping "not found" and "found but not
 * ready" distinguishable.  See `find_temp_dev()` below for the how/why of
 * that scan.  This deliberately does NOT gate on any vendor/MPN Kconfig
 * symbol (e.g. `CONFIG_TMP112`) -- that would pin the portable surface to
 * one part; see <alp/temperature.h>.
 *
 * UNLIKE its `hw_info_zephyr.c` sibling, this file is Zephyr-ONLY --
 * registered solely in zephyr/CMakeLists.txt, never in
 * src/common/CMakeLists.txt.  hw_info's EEPROM reader is built entirely
 * from already-portable primitives (alp_i2c_open() + the eeprom_24c128
 * driver), so it happens to compile clean on plain CMake too, taking the
 * NOSUPPORT branch when its Kconfig is unset.  This class has no portable
 * equivalent to fall back to -- the design deliberately binds through
 * Zephyr's own devicetree/sensor API, which plain CMake has no concept
 * of at all -- so baremetal/Yocto get a real stub instead
 * (src/common/stub/stub_temperature.c), the same split every other
 * class with a Zephyr-specific backend uses (stub_rtc.c, stub_wdt.c,
 * ...).  A `_zephyr`-suffixed file that quietly ALSO compiled under
 * plain CMake, relying on `#if defined(__ZEPHYR__)` guards to neuter
 * itself, was tried and reverted (#2066 review): every DT macro
 * reference has to be lexically INSIDE a `#if defined(__ZEPHYR__)`
 * block (nested, never `&&`-joined on the same line) for that trick to
 * be safe, and a single missed line breaks every plain-CMake build --
 * not worth the fragility when a stub is one small file.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

/*
 * The on-module sensor, resolved via a dedicated DT alias so the SoM
 * preset's own device node decides presence -- never a compiled-in
 * vendor compatible string.  `alp-temp0` joins the existing alp-prefixed
 * portable-alias namespace (`alp-i2cN`, `alp-lprtc-counter`).
 *
 * DT_HAS_ALIAS() is the preprocessor-safe existence check (Zephyr
 * devicetree.h's own convention: it tests a generated `..._EXISTS`
 * macro that is either defined or simply absent, never partially so);
 * DT_NODE_HAS_STATUS_OKAY() is only evaluated once that is already true,
 * matching the two-step idiom Zephyr v4.4.1 documents for exactly this
 * "alias may not exist at all" case.  Gated on BOTH CONFIG_SENSOR and
 * the alias's own DT status: an app that never turns on the sensor
 * subsystem, or a board whose alias node is absent/disabled, both
 * collapse to the NOSUPPORT branch below.  This is necessary but not
 * sufficient for a device to actually exist at runtime -- see the device
 * lookup in the function body below.
 */
#if defined(CONFIG_SENSOR) && DT_HAS_ALIAS(alp_temp0) && \
    DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(alp_temp0))
#include <zephyr/drivers/sensor.h>
#define ALP_TEMPERATURE_SENSOR_ENABLED 1

/*
 * Find the device by name without DEVICE_DT_GET()'s link-time ordinal
 * reference, and without folding "no such device" and "device not
 * ready" into one outcome the way device_get_binding() does -- see the
 * file header for why both of those matter here.
 *
 * z_device_get_all_static() and STRUCT_SECTION_FOREACH(device, ...) are
 * both underscore-private; there is no public Zephyr API for "does this
 * DT node have a device at all, ready or not".  z_device_get_all_static()
 * is used because it is the exact function z_impl_device_get_binding()
 * itself calls (kernel/device.c) to get the array it then scans -- this
 * does the identical by-name scan, just keeping the not-found and
 * not-ready outcomes apart instead of collapsing them to NULL.  Zephyr's
 * own device shell and PM subsystem call the same private function for
 * the same reason (subsys/shell/modules/device_service.c,
 * subsys/pm/device.c) -- this is not a first use of it.
 */
static const struct device *find_temp_dev(void)
{
	const char          *name = DEVICE_DT_NAME(DT_ALIAS(alp_temp0));
	const struct device *devlist;
	size_t               devcnt = z_device_get_all_static(&devlist);

	for (size_t i = 0; i < devcnt; i++) {
		if (strcmp(devlist[i].name, name) == 0) return &devlist[i];
	}
	return NULL;
}
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
	/* find_temp_dev() (above) covers "no driver ever instantiated the
	 * node" (e.g. CONFIG_TMP112=n, #2043's board default); the explicit
	 * device_is_ready() below covers "a driver did, but it never came
	 * up" (e.g. the TMP112 NACKs at its configured address, #1978) --
	 * kept separate on purpose so a fitted-but-dead sensor doesn't read
	 * as "no sensor on this SoM" on the banner. */
	const struct device *dev = find_temp_dev();
	if (dev == NULL) return ALP_ERR_NOSUPPORT;
	if (!device_is_ready(dev)) return ALP_ERR_NOT_READY;

	struct sensor_value val;
	int                 rc = sensor_sample_fetch_chan(dev, SENSOR_CHAN_AMBIENT_TEMP);
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	}
	if (rc != 0) return ALP_ERR_IO;

	/* Integer milli-degrees C -- the same conversion
     * examples/aen/aen-temp-sensor and the boot banner already use; no
     * float printf anywhere in this path. */
	*milli_c = (int32_t)sensor_value_to_milli(&val);
	return ALP_OK;
#endif
}

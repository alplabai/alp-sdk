/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display backlight default brightness: for every enabled
 * `alp,display-backlight` node, call led_set_brightness() on the LED it names
 * once, at APPLICATION init. It runs at a later priority than the display chain
 * (the display controller and a bridge such as the SN65DSI83 initialise at
 * CONFIG_APPLICATION_INIT_PRIORITY) and only when every device the node lists
 * in required-devices is ready: a panel that did not come up stays dark, with
 * the reason on the console, instead of being lit. Compiles to nothing when no
 * node exists (the RK055 shield has none: its P5_5 backlight enable is the
 * HX8394 driver's own bl-gpios).
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#define DT_DRV_COMPAT alp_display_backlight

/* After the display chain (CONFIG_APPLICATION_INIT_PRIORITY), still APPLICATION level. */
#define ALP_BL_PRIO 95
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < ALP_BL_PRIO,
             "the backlight must run after the display chain");

/* One required device: log and skip the whole node when it is not ready. */
#define ALP_BL_CHECK(node_id, prop, idx) \
	if (!device_is_ready(DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)))) { \
		printk("display : %s not ready -- backlight left off\n", \
		       DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))->name); \
		return 0; \
	}

#define ALP_BL_INIT(inst) \
	BUILD_ASSERT(DT_INST_PROP(inst, default_brightness) >= 0 && \
	                 DT_INST_PROP(inst, default_brightness) <= 100, \
	             "default-brightness is a percentage, 0..100"); \
	static int alp_bl_init_##inst(void) \
	{ \
		const struct device *led = DEVICE_DT_GET(DT_PARENT(DT_INST_PHANDLE(inst, led))); \
		int                  rc  = -ENODEV; \
\
		COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, required_devices), \
		            (DT_INST_FOREACH_PROP_ELEM(inst, required_devices, ALP_BL_CHECK)), \
		            ()) \
		if (device_is_ready(led)) { \
			rc = led_set_brightness(led, \
			                        DT_NODE_CHILD_IDX(DT_INST_PHANDLE(inst, led)), \
			                        DT_INST_PROP(inst, default_brightness)); \
		} \
		printk("display : backlight %u%% -> %d\n", \
		       (unsigned)DT_INST_PROP(inst, default_brightness), \
		       rc); \
		return 0; \
	} \
	SYS_INIT(alp_bl_init_##inst, APPLICATION, ALP_BL_PRIO);

DT_INST_FOREACH_STATUS_OKAY(ALP_BL_INIT)

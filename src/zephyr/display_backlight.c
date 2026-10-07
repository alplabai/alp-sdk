/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display backlight default brightness: for every enabled
 * `alp,display-backlight` node, call led_set_brightness() on the LED it names
 * once, at APPLICATION init -- after the PWM / GPIO controller and the
 * pwm-leds / gpio-leds driver are up, before main(). Compiles to nothing when
 * no node exists (the RK055 shield has none: its P5_5 backlight enable is the
 * HX8394 driver's own bl-gpios).
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#define DT_DRV_COMPAT alp_display_backlight

#define ALP_BL_INIT(inst) \
	static int alp_bl_init_##inst(void) \
	{ \
		const struct device *led = DEVICE_DT_GET(DT_PARENT(DT_INST_PHANDLE(inst, led))); \
		int                  rc  = -ENODEV; \
\
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
	SYS_INIT(alp_bl_init_##inst, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

DT_INST_FOREACH_STATUS_OKAY(ALP_BL_INIT)

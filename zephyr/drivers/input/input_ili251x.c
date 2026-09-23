/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR-0017-ADJACENT — BENCH-UNVERIFIED ======
 * Ilitek ILI251x capacitive touch controller (ILI2511 on the Riverdi
 * RVT121HVDFWCA0-B panel). No upstream Zephyr driver, hal_alif library, or
 * opt-in vendor-fork driver exists for this part -- input_gt911.c and
 * input_ili2132a.c (upstream Zephyr, different Ilitek protocol) are used
 * only as the STRUCTURAL template (k_work-deferred I2C read, reset/int
 * gpio handling, CONFIG_INPUT_*_INTERRUPT knob); no code, names, or
 * comments are copied from either. The register map and report layout
 * below are protocol facts derived from the BEHAVIOUR of the mainline
 * Linux `ilitek,ili251x` driver (GPL-2.0) -- authored from observed
 * protocol behaviour, not from a public datasheet (none is available for
 * this part), per ADR 0017's "author from spec" tier for a novel-to-alp-sdk
 * IC with no consumable driver; no code or comments were copied from it.
 * Not yet run against real silicon; verify on an E1M-EVK + Riverdi panel
 * before relying on it.
 */

#define DT_DRV_COMPAT ilitek_ili251x

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ili251x, CONFIG_INPUT_LOG_LEVEL);

#include "input_ili251x_report.h"

/* Registers (protocol facts -- see the file header comment). */
#define ILI251X_REG_TOUCH_DATA 0x10
#define ILI251X_REG_PANEL_INFO 0x20
#define ILI251X_REG_FW_VERSION 0x40
#define ILI251X_REG_GET_MODE   0xC0

#define ILI251X_MODE_APPLICATION 0x5A
#define ILI251X_MODE_BOOTLOADER  0x55

#define ILI251X_TOUCH_REPORT_LEN       31
#define ILI251X_TOUCH_CONTINUATION_LEN 20
#define ILI251X_PANEL_INFO_LEN         10
#define ILI251X_PANEL_INFO_INVALID     0xFFFFU

/* Info/config registers need a settle delay between the address write and
 * the data read; the touch-data register does not (protocol facts).
 */
#define ILI251X_INFO_REG_DELAY K_MSEC(5)

/* Reset pulse + settle timing (protocol facts). */
#define ILI251X_RESET_ASSERT_MS  15
#define ILI251X_RESET_RELEASE_MS 300
#define ILI251X_POST_PROBE_MS    200

/* Polling-mode backoff while the touch-data I2C read keeps failing (a wedged
 * bus, or no controller at all) -- see ili251x_work_handler()'s comment.
 */
#define ILI251X_ERROR_POLL_MS 1000

struct ili251x_config {
	/* Must be first: input_touchscreen_report_pos()/INPUT_TOUCH_STRUCT_CHECK
	 * require the common touchscreen config at offset 0.
	 */
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec                     bus;
	struct gpio_dt_spec                    reset_gpio; /* optional: {0} if reset-gpios absent */
	struct gpio_dt_spec                    int_gpio;   /* optional: {0} if int-gpios absent */
};

/*
 * Probe runs as a sequence of short work items instead of one long one, so
 * no single item blocks the system workqueue for the controller's ~500 ms
 * combined settle time -- see ili251x_probe_step()'s comment.
 */
enum ili251x_probe_stage {
	ILI251X_PROBE_RELEASE_RESET, /* release reset-gpios, wait RESET_RELEASE_MS */
	ILI251X_PROBE_SETTLE,        /* wait POST_PROBE_MS before touching the bus */
	ILI251X_PROBE_READ,          /* read mode/panel-info/fw-version; retried on failure */
};

struct ili251x_data {
	const struct device     *dev;
	struct k_work_delayable  work;
	uint16_t                 ctrl_width;  /* controller-native X resolution, from reg 0x20 */
	uint16_t                 ctrl_height; /* controller-native Y resolution, from reg 0x20 */
	bool                     was_pressed;
	bool                     probed; /* ili251x_probe_step() has reached ILI251X_PROBE_READ and
	                                    * succeeded; ctrl_width/ctrl_height are valid. */
	enum ili251x_probe_stage probe_stage;
	bool i2c_error; /* touch-data read is currently failing (log-once + backoff). */
#ifdef CONFIG_INPUT_ILI251X_INTERRUPT
	struct gpio_callback int_gpio_cb;
#endif
};

INPUT_TOUCH_STRUCT_CHECK(struct ili251x_config);

/*
 * Register read = write the 1-byte register address, then a SEPARATE I2C
 * read (protocol facts: not a repeated-start i2c_write_read_dt()). Info/
 * config registers want a short settle delay between the two transfers;
 * the touch-data register does not (pass K_NO_WAIT).
 */
static int
ili251x_reg_read(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len, k_timeout_t delay)
{
	const struct ili251x_config *cfg = dev->config;
	int                          ret;

	ret = i2c_write_dt(&cfg->bus, &reg, sizeof(reg));
	if (ret < 0) {
		return ret;
	}

	if (K_TIMEOUT_EQ(delay, K_NO_WAIT) == false) {
		k_sleep(delay);
	}

	return i2c_read_dt(&cfg->bus, buf, len);
}

/*
 * Scale a controller-native coordinate to the DT-configured display size,
 * then apply invert/swap ourselves.
 *
 * input_touchscreen_report_pos() (zephyr/include/zephyr/input/input_touch.h)
 * asserts `inverted_x == (screen_width > 0)`: it only tolerates a nonzero
 * screen-width/screen-height when that axis is ALSO inverted. We want
 * screen-width/screen-height as the scale target on every report, inverted
 * or not, so calling that helper directly would trip its assert on the
 * common (non-inverted) case. Do the same invert/swap math ourselves and
 * report through input_report_abs(), like upstream's own ili2132a driver
 * does.
 */
static void ili251x_report_touch(const struct device *dev, const struct ili251x_touch_report *r)
{
	const struct ili251x_config *cfg  = dev->config;
	struct ili251x_data         *data = dev->data;
	uint32_t                     x    = r->x;
	uint32_t                     y    = r->y;

	if (data->ctrl_width > 0 && cfg->common.screen_width > 0) {
		x = (uint32_t)r->x * cfg->common.screen_width / data->ctrl_width;
	}
	if (data->ctrl_height > 0 && cfg->common.screen_height > 0) {
		y = (uint32_t)r->y * cfg->common.screen_height / data->ctrl_height;
	}

	if (cfg->common.inverted_x && cfg->common.screen_width > 0) {
		x = cfg->common.screen_width - x;
	}
	if (cfg->common.inverted_y && cfg->common.screen_height > 0) {
		y = cfg->common.screen_height - y;
	}

	if (cfg->common.swapped_x_y) {
		input_report_abs(dev, INPUT_ABS_Y, x, false, K_FOREVER);
		input_report_abs(dev, INPUT_ABS_X, y, false, K_FOREVER);
	} else {
		input_report_abs(dev, INPUT_ABS_X, x, false, K_FOREVER);
		input_report_abs(dev, INPUT_ABS_Y, y, false, K_FOREVER);
	}
}

/*
 * Read + report contact 0. Returns 0 and sets *pressed on a successful
 * read (even if nothing is touching); a negative errno on I2C failure.
 */
static int ili251x_process(const struct device *dev, bool *pressed)
{
	const struct ili251x_config *cfg  = dev->config;
	struct ili251x_data         *data = dev->data;
	uint8_t                      buf[ILI251X_TOUCH_REPORT_LEN];
	uint8_t                      continuation[ILI251X_TOUCH_CONTINUATION_LEN];
	struct ili251x_touch_report  report;
	int                          ret;

	ret = ili251x_reg_read(dev, ILI251X_REG_TOUCH_DATA, buf, sizeof(buf), K_NO_WAIT);
	if (ret < 0) {
		/*
		 * A wedged bus (or no controller at all) fails every poll forever;
		 * logging LOG_ERR on every one of them at CONFIG_INPUT_ILI251X_PERIOD_MS
		 * (15 ms default) floods the log for no new information after the
		 * first occurrence.  Log once on the failing transition, then drop
		 * to LOG_DBG; ili251x_work_handler() also backs the poll period off
		 * while this is true.
		 */
		if (!data->i2c_error) {
			LOG_ERR("touch-data read failed: %d", ret);
			data->i2c_error = true;
		} else {
			LOG_DBG("touch-data read still failing: %d", ret);
		}
		return ret;
	}
	if (data->i2c_error) {
		LOG_INF("touch-data read recovered");
		data->i2c_error = false;
	}

	/*
	 * byte0==2 means contacts 6-9 follow as a 20-byte continuation --
	 * a plain read, no register write (protocol facts). v1 is
	 * single-touch (contact 0 only, always inside the first 31 bytes),
	 * so the continuation bytes themselves are unused -- but the read
	 * still has to happen, or the controller's next register write
	 * lands mid-continuation-packet and desyncs the bus.
	 */
	if (buf[0] == 2) {
		ret = i2c_read_dt(&cfg->bus, continuation, sizeof(continuation));
		if (ret < 0) {
			LOG_ERR("touch-data continuation read failed: %d", ret);
			return ret;
		}
	}

	ili251x_parse_contact0(buf, sizeof(buf), &report);

	/*
	 * gt911 pattern: on EVERY pressed sample, report ABS_X/ABS_Y (sync=false,
	 * inside ili251x_report_touch()) then BTN_TOUCH=1 with sync=true -- not
	 * just on the press transition.  A drag holds `pressed` true across many
	 * samples; if BTN_TOUCH only went out on a state CHANGE, only the first
	 * sample of a drag would ever carry sync=true and every ABS update after
	 * it would sit unsynced, which LVGL (and the Zephyr input subsystem in
	 * general) never commits.  On release, BTN_TOUCH=0 goes out exactly once
	 * (guarded by the state check) -- there is no ABS position to repeat.
	 */
	if (report.pressed) {
		ili251x_report_touch(dev, &report);
		input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
	} else if (data->was_pressed) {
		input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
	}
	data->was_pressed = report.pressed;
	*pressed          = report.pressed;

	return 0;
}

/*
 * The reset-release settle wait, the post-probe settle wait, and the mode/
 * panel-info/fw-version reads used to run inline in ili251x_init(),
 * blocking boot for ~530 ms (15 ms assert + 300 ms release settle + 200 ms
 * post-probe + 3x 5 ms register-read delays).  They run here instead, as a
 * short ILI251X_PROBE_* stage per ili251x_work_handler() invocation, each
 * ending in a k_work_reschedule() rather than a k_sleep() -- so no single
 * work item blocks the system workqueue's cooperative thread for more than
 * one register access at a time, and init() only pays the much shorter
 * 15 ms assert-hold cost before returning -- see ili251x_init()'s comment.
 *
 * ILI251X_PROBE_READ is the only stage that can fail (an I2C read error).
 * On failure this leaves data->probed false -- ctrl_width/ctrl_height stay
 * at their zero reset value, so ili251x_report_touch()'s scaling is skipped
 * rather than dividing by a resolution that was never read -- and reschedules
 * itself at ILI251X_ERROR_POLL_MS to retry the reads (reset stays released
 * and the settle waits are not repeated). Only a successful mode + panel-info
 * read sets data->probed = true and hands off to the normal poll loop.
 */
static void ili251x_probe_step(const struct device *dev)
{
	const struct ili251x_config *cfg  = dev->config;
	struct ili251x_data         *data = dev->data;
	uint8_t                      mode;
	uint8_t                      fw_version;
	uint8_t                      panel_info[ILI251X_PANEL_INFO_LEN];
	int                          ret;

	switch (data->probe_stage) {
	case ILI251X_PROBE_RELEASE_RESET:
		if (cfg->reset_gpio.port != NULL) {
			/* Release the reset asserted in ili251x_init(), then wait the
			 * controller's post-reset settle time before talking on the bus.
			 */
			ret = gpio_pin_set_dt(&cfg->reset_gpio, 0);
			if (ret < 0) {
				LOG_ERR("could not release reset gpio: %d", ret);
			}
			data->probe_stage = ILI251X_PROBE_SETTLE;
			k_work_reschedule(&data->work, K_MSEC(ILI251X_RESET_RELEASE_MS));
			return;
		}
		data->probe_stage = ILI251X_PROBE_SETTLE;
		__fallthrough;
	case ILI251X_PROBE_SETTLE:
		/* Wanted even without a reset line: the controller needs ~200 ms
		 * after probe before its info registers are readable.
		 */
		data->probe_stage = ILI251X_PROBE_READ;
		k_work_reschedule(&data->work, K_MSEC(ILI251X_POST_PROBE_MS));
		return;
	case ILI251X_PROBE_READ:
		ret = ili251x_reg_read(
		    dev, ILI251X_REG_GET_MODE, &mode, sizeof(mode), ILI251X_INFO_REG_DELAY);
		if (ret < 0) {
			LOG_ERR("mode read failed: %d", ret);
			k_work_reschedule(&data->work, K_MSEC(ILI251X_ERROR_POLL_MS));
			return;
		}
		if (mode == ILI251X_MODE_BOOTLOADER) {
			LOG_WRN("controller is in bootloader mode (0x%02x)", mode);
		} else if (mode != ILI251X_MODE_APPLICATION) {
			LOG_WRN("unexpected mode register value 0x%02x", mode);
		}

		ret = ili251x_reg_read(
		    dev, ILI251X_REG_PANEL_INFO, panel_info, sizeof(panel_info), ILI251X_INFO_REG_DELAY);
		if (ret < 0) {
			LOG_ERR("panel-info read failed: %d", ret);
			k_work_reschedule(&data->work, K_MSEC(ILI251X_ERROR_POLL_MS));
			return;
		}
		data->ctrl_width  = sys_get_le16(&panel_info[0]);
		data->ctrl_height = sys_get_le16(&panel_info[2]);
		if (data->ctrl_width == 0 || data->ctrl_width == ILI251X_PANEL_INFO_INVALID) {
			LOG_WRN("invalid panel X resolution from controller, using configured size");
			data->ctrl_width = cfg->common.screen_width;
		}
		if (data->ctrl_height == 0 || data->ctrl_height == ILI251X_PANEL_INFO_INVALID) {
			LOG_WRN("invalid panel Y resolution from controller, using configured size");
			data->ctrl_height = cfg->common.screen_height;
		}

		ret = ili251x_reg_read(
		    dev, ILI251X_REG_FW_VERSION, &fw_version, sizeof(fw_version), ILI251X_INFO_REG_DELAY);
		if (ret == 0) {
			LOG_INF("firmware version 0x%02x", fw_version);
		}

		data->probed = true;
		k_work_reschedule(&data->work, K_NO_WAIT);
		return;
	}
}

static void ili251x_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork   = k_work_delayable_from_work(work);
	struct ili251x_data     *data    = CONTAINER_OF(dwork, struct ili251x_data, work);
	bool                     pressed = false;
	int                      ret;

	if (!data->probed) {
		ili251x_probe_step(data->dev);
		return;
	}

	ret = ili251x_process(data->dev, &pressed);

#ifdef CONFIG_INPUT_ILI251X_INTERRUPT
	/*
	 * IRQ mode: the controller does not re-interrupt while a contact
	 * stays down, so keep polling at the configured period until it
	 * releases (protocol facts: "keep polling while any contact is
	 * down; stop when none"). A read error also stops the loop rather
	 * than spinning on a wedged bus.
	 */
	if (ret == 0 && pressed) {
		k_work_reschedule(dwork, K_MSEC(CONFIG_INPUT_ILI251X_PERIOD_MS));
	}
#else
	ARG_UNUSED(pressed);
	/*
	 * Polling mode: fixed period forever, there is no INT line to tell
	 * us when to stop (this is the E1M-EVK's mode -- the touch INT pad
	 * is owned by the CC3501E co-processor, not the Alif core).  Back off
	 * to ILI251X_ERROR_POLL_MS while the I2C read keeps failing instead
	 * of spinning a wedged bus every CONFIG_INPUT_ILI251X_PERIOD_MS (15 ms
	 * default) forever; ili251x_process() itself throttles the repeated
	 * LOG_ERR down to LOG_DBG for the same reason and restores the normal
	 * period as soon as a read succeeds again.
	 */
	k_work_reschedule(
	    dwork, ret == 0 ? K_MSEC(CONFIG_INPUT_ILI251X_PERIOD_MS) : K_MSEC(ILI251X_ERROR_POLL_MS));
#endif
}

#ifdef CONFIG_INPUT_ILI251X_INTERRUPT
static void ili251x_isr_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct ili251x_data *data = CONTAINER_OF(cb, struct ili251x_data, int_gpio_cb);

	ARG_UNUSED(pins);
	k_work_reschedule(&data->work, K_NO_WAIT);
}
#endif

static int ili251x_init(const struct device *dev)
{
	const struct ili251x_config *cfg  = dev->config;
	struct ili251x_data         *data = dev->data;
	int                          ret;

	if (!i2c_is_ready_dt(&cfg->bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	data->dev = dev;

	if (cfg->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
			LOG_ERR("reset gpio not ready");
			return -ENODEV;
		}

		/*
		 * Active-low reset: assert (drive low) here, synchronously -- the
		 * 12-15 ms hold is short enough not to matter for boot time.
		 * Releasing it, the much longer 300 ms post-reset settle, and the
		 * three info-register reads (another ~200+ ms) run as staged work
		 * items in ili251x_probe_step() instead, off the init thread, so
		 * this function does not block boot for the whole ~530 ms sequence
		 * -- see that function's comment.
		 */
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("could not configure reset gpio: %d", ret);
			return ret;
		}
		k_sleep(K_MSEC(ILI251X_RESET_ASSERT_MS));
	}

	k_work_init_delayable(&data->work, ili251x_work_handler);

	if (cfg->int_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->int_gpio)) {
			LOG_ERR("int gpio not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("could not configure int gpio: %d", ret);
			return ret;
		}
	}

#ifdef CONFIG_INPUT_ILI251X_INTERRUPT
	if (cfg->int_gpio.port == NULL) {
		LOG_ERR("CONFIG_INPUT_ILI251X_INTERRUPT needs int-gpios in the devicetree node");
		return -EINVAL;
	}

	gpio_init_callback(&data->int_gpio_cb, ili251x_isr_handler, BIT(cfg->int_gpio.pin));
	ret = gpio_add_callback(cfg->int_gpio.port, &data->int_gpio_cb);
	if (ret < 0) {
		LOG_ERR("could not set gpio callback: %d", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("could not configure interrupt: %d", ret);
		return ret;
	}
#endif
	/* Kick the first work item (the first ili251x_probe_step() stage,
	 * eventually followed by the first poll/re-poll) onto the system
	 * workqueue right away rather than waiting for the first IRQ --
	 * interrupt mode still needs the controller probed once even if no
	 * touch has happened yet.
	 */
	k_work_reschedule(&data->work, K_NO_WAIT);

	return 0;
}

#define ILI251X_INIT(index) \
	static const struct ili251x_config ili251x_config_##index = { \
		.common     = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index), \
		.bus        = I2C_DT_SPEC_INST_GET(index), \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(index, reset_gpios, { 0 }), \
		.int_gpio   = GPIO_DT_SPEC_INST_GET_OR(index, int_gpios, { 0 }), \
	}; \
	static struct ili251x_data ili251x_data_##index; \
	DEVICE_DT_INST_DEFINE(index, \
	                      ili251x_init, \
	                      NULL, \
	                      &ili251x_data_##index, \
	                      &ili251x_config_##index, \
	                      POST_KERNEL, \
	                      CONFIG_INPUT_INIT_PRIORITY, \
	                      NULL);

DT_INST_FOREACH_STATUS_OKAY(ILI251X_INIT)

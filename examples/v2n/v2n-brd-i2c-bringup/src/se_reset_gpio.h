/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * se_reset_gpio.h -- pulse the OPTIGA Trust M's SE_RST line from Linux.
 *
 * SE_RST is wired to the GD32 supervisor, not to a SoC pin.  On a running
 * V2N/V2M image the kernel's `alplab,gd32-bridge-gpio` driver owns the GD32's
 * BRD_I2C address (0x70), so an app must NOT open the bridge itself: a raw
 * I2C_RDWR frame ignores that binding and interleaves with the driver's own
 * traffic.  Instead the driver exports SE_RST as GPIO line "se-rst" on the
 * chip labelled "gd32-bridge-gpio" (meta-alp-sdk's e1m-v2n-som.dtsi), and
 * this header drives it through the standard Linux GPIO character device:
 * find the chip by label, the line by name, request it as an output, then
 * set the value.  Value 1 = assert = hold the Trust M in reset; the driver
 * sends the bridge CMD_SE_RESET frame for it and never replays it, so a
 * bridge reset leaves the part released.
 *
 * No libgpiod: the raw GPIO_V2 ioctls from <linux/gpio.h> are enough and
 * keep the app free of an extra dependency.
 *
 * Usage (see main.c):
 *   se_reset_gpio_t rst;
 *   bool have = se_reset_gpio_open(&rst) == 0;
 *   optiga_trust_m_init_with_reset(&se, bus, addr,
 *                                  have ? se_reset_gpio_hook : NULL,
 *                                  have ? &rst : NULL);
 *   if (have) se_reset_gpio_close(&rst);
 * If the image has no "se-rst" line (an older kernel), open fails and the
 * caller passes no hook: a plain probe, no reset.
 */

#ifndef SE_RESET_GPIO_H
#define SE_RESET_GPIO_H

#include <fcntl.h>
#include <linux/gpio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <alp/peripheral.h>

#define SE_RESET_GPIO_CHIP_LABEL "gd32-bridge-gpio"
#define SE_RESET_GPIO_LINE_NAME  "se-rst"
/* Upper bound on /dev/gpiochipN to scan; a V2N has well under this. */
#define SE_RESET_GPIO_MAX_CHIPS 32

typedef struct {
	int line_fd; /* GPIO_V2_GET_LINE_IOCTL request fd; -1 when closed */
} se_reset_gpio_t;

/* Find the "se-rst" line on the bridge gpiochip and request it as an output
 * that starts released (value 0).  Returns 0, or -1 if the chip or line is
 * absent or the request fails. */
static inline int se_reset_gpio_open(se_reset_gpio_t *rst)
{
	rst->line_fd = -1;
	for (int n = 0; n < SE_RESET_GPIO_MAX_CHIPS; n++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/gpiochip%d", n);
		int chip_fd = open(path, O_RDWR | O_CLOEXEC);
		if (chip_fd < 0) continue;

		struct gpiochip_info ci;
		if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &ci) < 0 ||
		    strcmp(ci.label, SE_RESET_GPIO_CHIP_LABEL) != 0) {
			close(chip_fd);
			continue;
		}
		for (uint32_t line = 0; line < ci.lines; line++) {
			struct gpio_v2_line_info li;
			memset(&li, 0, sizeof li);
			li.offset = line;
			if (ioctl(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) < 0 ||
			    strcmp(li.name, SE_RESET_GPIO_LINE_NAME) != 0) {
				continue;
			}
			struct gpio_v2_line_request req;
			memset(&req, 0, sizeof req);
			req.offsets[0]                  = line;
			req.num_lines                   = 1;
			req.config.flags                = GPIO_V2_LINE_FLAG_OUTPUT;
			req.config.num_attrs            = 1;
			req.config.attrs[0].mask        = 1; /* applies to line 0 of this request */
			req.config.attrs[0].attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
			req.config.attrs[0].attr.values = 0; /* start released */
			snprintf(req.consumer, sizeof req.consumer, "alp-se-reset");
			int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
			close(chip_fd);
			if (rc < 0) return -1;
			rst->line_fd = req.fd;
			return 0;
		}
		close(chip_fd);
	}
	return -1;
}

/* optiga_trust_m_reset_fn_t: @p user is a se_reset_gpio_t *.  Each call is
 * one bridge I2C frame in the kernel driver, so the pulse width the Trust M
 * driver sleeps between assert and release is a floor, not an exact time. */
static inline alp_status_t se_reset_gpio_hook(void *user, bool assert)
{
	se_reset_gpio_t           *rst = user;
	struct gpio_v2_line_values v;
	v.mask = 1;
	v.bits = assert ? 1 : 0;
	return ioctl(rst->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? ALP_ERR_IO : ALP_OK;
}

/* Release SE_RST (the line keeps its last value after close) and free the
 * request. */
static inline void se_reset_gpio_close(se_reset_gpio_t *rst)
{
	if (rst->line_fd < 0) return;
	(void)se_reset_gpio_hook(rst, false);
	close(rst->line_fd);
	rst->line_fd = -1;
}

#endif /* SE_RESET_GPIO_H */

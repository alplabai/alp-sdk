/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for <zephyr/usb/usb_device.h> (needs USB_DEVICE_STACK, absent on
 * native_sim): only the two calls src/backends/usb/zephyr_drv.c makes.
 */
#ifndef FAKE_ZEPHYR_USB_DEVICE_H
#define FAKE_ZEPHYR_USB_DEVICE_H

static inline int usb_enable(void *status_cb)
{
	(void)status_cb;
	return 0;
}

static inline int usb_disable(void)
{
	return 0;
}

#endif /* FAKE_ZEPHYR_USB_DEVICE_H */

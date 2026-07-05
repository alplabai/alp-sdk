/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * xHCI USB host enumeration on the E1M-AEN801, driving the board USB2_SELECT mux.
 *
 * The E8 has one USB (USB2); its D+/D- pass through mux U47 to the USB-C
 * connector (CON_USB2 = J13).  U47's select `USB2_SELECT` = E1M IO11 = CC3501E
 * GPIO pad 2 (a coprocessor GPIO).  So before the E8 xHCI can see a device on
 * J13, the CC3501E must drive that GPIO to route the mux.  We bring up the
 * CC3501E bridge, drive GPIO pad 2 over the SPI GPIO-proxy, then run the host:
 * uhc_init (DWC3 + xHCI first light) + uhc_enable (run + rings + Enable Slot +
 * port reset + Address Device + EP0 GET_DESCRIPTOR).  Device descriptor lands in
 * the driver's g_..._data_0.fl snapshot.  USB_ID = 0 (P2 jumper) sets host role.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/printk.h>

#include <alp/chips/cc3501e.h>
#include "cc3501e_bridge.h"

#ifndef USB2_SEL_LVL
#define USB2_SEL_LVL 1
#endif
#define CC3501E_USB2_SELECT_PAD 2u

/* SWD-readable witness for the CC3501E mux-drive (magic "USMX"). */
volatile struct {
	uint32_t magic;
	int32_t  bringup;
	int32_t  gpio_cfg;
	int32_t  gpio_wr;
	int32_t  level;
} g_usbmux __attribute__((used)) = { .magic = 0x55534d58u, .level = USB2_SEL_LVL };

static int fl_cb(const struct device *dev, const struct uhc_event *const event)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(event);
	return 0;
}

int main(void)
{
	static cc3501e_t fw;

	printk("=== AEN801 xHCI USB host: CC3501E USB2_SELECT mux + enumerate ===\n");

	/* 1. Bring up the CC3501E bridge and route the USB2_SELECT mux to J13. */
	alp_status_t bs = cc3501e_bridge_bringup(&fw);

	g_usbmux.bringup = (int32_t)bs;
	if (bs == ALP_OK) {
		g_usbmux.gpio_cfg = (int32_t)cc3501e_gpio_configure(&fw,
		                                                    CC3501E_USB2_SELECT_PAD,
		                                                    ALP_CC3501E_GPIO_DIR_OUTPUT,
		                                                    ALP_CC3501E_GPIO_PULL_NONE,
		                                                    200u);
		g_usbmux.gpio_wr =
		    (int32_t)cc3501e_gpio_write(&fw, CC3501E_USB2_SELECT_PAD, (USB2_SEL_LVL != 0), 200u);
		printk("USB2_SELECT (CC35 GPIO2) cfg=%d wr=%d level=%d\n",
		       g_usbmux.gpio_cfg,
		       g_usbmux.gpio_wr,
		       USB2_SEL_LVL);
		k_busy_wait(200000); /* mux + connect debounce */
	} else {
		printk("WARN: CC3501E link down (%d) -> mux not set\n", (int)bs);
	}

	/* 2. USB host first-light + enumeration. */
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0));

	if (!device_is_ready(dev)) {
		printk("RESULT FAIL: uhc device not ready\n");
		return 0;
	}
	if (uhc_init(dev, fl_cb, NULL) != 0) {
		printk("RESULT FAIL: uhc_init/first-light\n");
		return 0;
	}
	int en = uhc_enable(dev);

	printk("RESULT: uhc_enable rc=%d -- enum_stage/descriptor in g_..._data_0.fl\n", en);
	return 0;
}

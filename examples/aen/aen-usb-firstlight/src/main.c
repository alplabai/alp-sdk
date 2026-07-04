/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * xHCI USB host FIRST-LIGHT bench app for the E1M-AEN801 (Ensemble E8).
 *
 * Proves the controller is reachable: uhc_init() runs the driver's DWC3
 * host-mode init (clock + PHY POR + core/PHY soft-reset + GCTL PrtCapDir=host)
 * then the xHCI USBCMD.HCRST + USBSTS.CNR poll, and snapshots the capability
 * registers.  PASS = the reset settled and the caps are sane (HCIVERSION >=
 * 0x0100); the driver LOG_INF prints CAPLENGTH/HCIVERSION/HCSPARAMS1 (MaxSlots,
 * MaxPorts) -- the real values the ring/slot/enumeration path must be sized from.
 * No USB device need be attached for first light.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/printk.h>

static int fl_cb(const struct device *dev, const struct uhc_event *const event)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(event);
	return 0;
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0));

	printk("=== AEN801 xHCI USB host first-light ===\n");
	if (!device_is_ready(dev)) {
		printk("RESULT FAIL: uhc device not ready\n");
		return 0;
	}

	int rc = uhc_init(dev, fl_cb, NULL);

	if (rc == 0) {
		printk("RESULT PASS: xHCI first-light OK -- DWC3 host init + HCRST "
		       "settled, capability registers read (see the LOG line above)\n");
	} else {
		printk("RESULT FAIL: uhc_init/first-light rc=%d "
		       "(controller not reachable / not clocked / reset stuck)\n", rc);
	}
	return 0;
}

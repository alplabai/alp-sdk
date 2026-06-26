/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * usb-host-storage -- open the USB host role via the portable <alp/usb.h>
 * surface and enumerate an attached USB mass-storage device on the
 * E1M-AEN401 M55-HP.
 *
 * Build:
 *   west build -b alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp \
 *       examples/peripheral-io/usb-host-storage -d /tmp/usb_host
 *
 * Bring-up status:
 *   The DWC2-host uhc driver (uhc_dwc2_alif) is a SKELETON: the usbh
 *   stack registers the controller and calls the op table, but the actual
 *   bus-reset / channel-programming / transfer-completion sequences are
 *   bench-gated (TODO(aen401-bench) markers inside the driver).  This
 *   example compiles + links the full host path end-to-end; live USB
 *   enumeration requires bench bring-up.
 */
#include <stdio.h>

#include <zephyr/kernel.h>

#include <alp/usb.h>

int main(void)
{
	printf("== usb-host-storage (E1M-AEN401 M55) ==\n");

	alp_usb_host_t *host = alp_usb_host_open();
	if (host == NULL) {
		printf("alp_usb_host_open: no host backend available "
		       "(check CONFIG_USB_HOST_STACK + alif,dwc2-uhc DT node)\n");
		return 1;
	}

	if (alp_usb_host_enable(host) != ALP_OK) {
		printf("alp_usb_host_enable failed\n");
		alp_usb_host_close(host);
		return 1;
	}

	printf("USB host enabled -- attach a mass-storage device\n");
	printf("(enumeration is bench-gated; DWC2 bring-up required)\n");

	/*
	 * TODO(aen401-bench): on real silicon, wait here for a connect event
	 * from the usbh subsystem, then mount the MSC LUN via Zephyr's
	 * usb_host MSC class driver and list the root directory.
	 */
	k_sleep(K_SECONDS(2));

	alp_usb_host_disable(host);
	alp_usb_host_close(host);

	printf("USB host closed.\n");
	return 0;
}

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
 *   The xHCI uhc driver (uhc_xhci_alif) implements the full uhc_api --
 *   register bring-up, ring/transfer processing, root-hub enumeration,
 *   bus ops, and an event-ring IRQ (see the driver's own header comment).
 *   This example only drives the open/enable/disable/close lifecycle
 *   through the portable <alp/usb.h> surface, which does not yet wire
 *   endpoint I/O through to a mass-storage class driver (issue #388 is
 *   the software-completion tracking issue; that class-driver wiring is
 *   separate follow-on work).  Register bring-up was bench-proven on an
 *   E8 EVK; everything past that (transfers, bus suspend/resume, the
 *   ISR) has not itself been bench-run, and end-to-end enumeration on
 *   this EVK also needs a D+/D- signal-path fix -- see issue #388.
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
		       "(check CONFIG_USB_HOST_STACK + alif,xhci-uhc DT node)\n");
		return 1;
	}

	if (alp_usb_host_enable(host) != ALP_OK) {
		printf("alp_usb_host_enable failed\n");
		alp_usb_host_close(host);
		return 1;
	}

	printf("USB host enabled -- attach a mass-storage device\n");
	printf("(this example doesn't mount it yet -- see the file header)\n");

	/*
	 * Not yet wired: mounting the MSC LUN via Zephyr's usb_host MSC class
	 * driver and listing the root directory needs the <alp/usb.h>
	 * surface's own endpoint I/O, which does not exist yet (separate
	 * follow-on work from issue #388 -- see the file header comment).
	 */
	k_sleep(K_SECONDS(2));

	alp_usb_host_disable(host);
	alp_usb_host_close(host);

	printf("USB host closed.\n");
	return 0;
}

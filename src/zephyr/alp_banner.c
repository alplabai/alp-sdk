/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Alp SDK boot-identity banner.
 *
 * Prints one line at APPLICATION-level init identifying the SDK, the
 * SoM, and the manufacturer:
 *
 *   Alp SDK 0.6.0  |  E1M-AEN801 r1  |  (c) Alp Lab AB
 *
 * The board field is data-driven: when CONFIG_ALP_SDK_HW_INFO is
 * enabled and the SoM identity EEPROM is readable, it shows the LIVE
 * manifest SKU + hardware revision (the EEPROM travels with the SoM,
 * so it is the module's true identity -- see <alp/hw_info.h>).  When
 * the EEPROM is absent, unreadable, or not provisioned, it falls back
 * to the build-time board name (CONFIG_BOARD).
 *
 * Compiled only when CONFIG_ALP_SDK_BANNER=y (the whole TU is gated in
 * CMake).  Uses printk so it lands on whatever console backend the
 * app wired -- including the bench RAM console (ram_console_buf).
 */

#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_ALP_SDK_HW_INFO)
#include <alp/hw_info.h> /* alp_hw_info_read(), alp_hw_info_t, ALP_OK */
#endif

static int alp_sdk_banner(void)
{
#if defined(CONFIG_ALP_SDK_HW_INFO)
	alp_hw_info_t info;

	/*
	 * Best-effort: a missing/unprovisioned/unreadable EEPROM just
	 * means we fall through to the build-time name -- the banner is
	 * never allowed to fail a boot.
	 */
	if (alp_hw_info_read(&info) == ALP_OK && info.som_sku[0] != '\0') {
		printk("Alp SDK %s  |  %s %s  |  (c) Alp Lab AB\n",
		       CONFIG_ALP_SDK_VERSION,
		       info.som_sku,
		       info.som_hw_rev);
		return 0;
	}
#endif

	printk("Alp SDK %s  |  %s  |  (c) Alp Lab AB\n",
	       CONFIG_ALP_SDK_VERSION,
	       CONFIG_BOARD);
	return 0;
}

/*
 * APPLICATION level so the console (UART or RAM console) is already
 * up; default priority so it prints after device init but before the
 * app's main().
 */
SYS_INIT(alp_sdk_banner, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

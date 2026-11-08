/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr USB device backend enumerates with compile-time descriptors and
 * has no runtime overlay, so z_dev_open() must refuse a config that asks for a
 * different VID/PID/bcd_device/string (ALP_ERR_NOSUPPORT) and must keep opening
 * ALP_USB_DEVICE_CONFIG_DEFAULT (#2932).
 *
 * The real src/backends/usb/zephyr_drv.c is #included with CONFIG_ALP_SDK_USB
 * defined, and z_dev_open()/z_dev_close() are called directly.
 */

#define CONFIG_ALP_SDK_USB 1

#include "../../../../src/backends/usb/zephyr_drv.c"

#include <zephyr/ztest.h>

ZTEST_SUITE(usb_zephyr_descriptor, NULL, NULL, NULL, NULL, NULL);

static alp_status_t open_cfg(const alp_usb_device_config_t *cfg)
{
	alp_usb_dev_state_t st = { 0 };
	alp_status_t        rc = z_dev_open(cfg, &st, NULL);
	if (rc == ALP_OK) z_dev_close(&st);
	return rc;
}

ZTEST(usb_zephyr_descriptor, test_default_config_opens)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	zassert_equal(open_cfg(&cfg), ALP_OK);
	cfg.bcd_device = 0; /* 0 is also accepted as "unset" */
	zassert_equal(open_cfg(&cfg), ALP_OK);
}

ZTEST(usb_zephyr_descriptor, test_non_default_vid_refused)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	cfg.vendor_id               = 0x1234;
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);
}

ZTEST(usb_zephyr_descriptor, test_non_default_pid_refused)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	cfg.product_id              = 0x5678;
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);
}

ZTEST(usb_zephyr_descriptor, test_non_default_bcd_device_refused)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_HID);
	cfg.bcd_device              = 0x0200;
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);
}

ZTEST(usb_zephyr_descriptor, test_strings_refused)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	cfg.manufacturer            = "Alp Lab";
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);

	cfg         = (alp_usb_device_config_t)ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	cfg.product = "dev";
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);

	cfg        = (alp_usb_device_config_t)ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	cfg.serial = "0001";
	zassert_equal(open_cfg(&cfg), ALP_ERR_NOSUPPORT);
}

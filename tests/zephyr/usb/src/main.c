/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/usb.h> under native_sim.  No USB controller
 * present; verify the NOSUPPORT contract + every NULL-arg branch.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/usb.h"

ZTEST_SUITE(alp_usb, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_usb, test_device_open_no_backend_returns_null)
{
    alp_usb_device_config_t cfg = {
        .device_class = ALP_USB_DEVICE_CDC_ACM, .vendor_id = 0x1234, .product_id = 0x5678};
    alp_usb_dev_t *d = alp_usb_device_open(&cfg);
    zassert_is_null(d);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_usb, test_device_open_null_invalid)
{
    zassert_is_null(alp_usb_device_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_usb, test_device_lifecycle_null_handle_safe)
{
    uint8_t buf[8] = {0};
    size_t  got    = 999;
    zassert_equal(alp_usb_device_enable(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_usb_device_disable(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_usb_device_write(NULL, buf, sizeof(buf), 100), ALP_ERR_NOT_READY);
    zassert_equal(alp_usb_device_read(NULL, buf, sizeof(buf), &got, 100), ALP_ERR_NOT_READY);
    zassert_equal(got, 0);
    alp_usb_device_close(NULL);
}

ZTEST(alp_usb, test_host_open_no_backend_returns_null)
{
    alp_usb_host_t *h = alp_usb_host_open();
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_usb, test_host_lifecycle_null_handle_safe)
{
    zassert_equal(alp_usb_host_enable(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_usb_host_disable(NULL), ALP_ERR_NOT_READY);
    alp_usb_host_close(NULL);
}

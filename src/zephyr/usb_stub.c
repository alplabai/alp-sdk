/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/usb.h>.  Real Zephyr USB device + host impl
 * lands v0.3.  Same shape as iot_stub.c.
 */

#include <stddef.h>

#include "alp/usb.h"

/* ------------------------------------------------------------------ */
/* Device role                                                         */
/* ------------------------------------------------------------------ */

alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_usb_device_enable(alp_usb_dev_t *dev) {
    (void)dev;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_device_disable(alp_usb_dev_t *dev) {
    (void)dev;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_device_write(alp_usb_dev_t *dev,
                                  const uint8_t *data, size_t len,
                                  uint32_t timeout_ms) {
    (void)dev; (void)data; (void)len; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_device_read(alp_usb_dev_t *dev,
                                 uint8_t *data, size_t len,
                                 size_t *out_len,
                                 uint32_t timeout_ms) {
    (void)dev; (void)data; (void)len; (void)timeout_ms;
    if (out_len != NULL) *out_len = 0;
    return ALP_ERR_NOSUPPORT;
}

void alp_usb_device_close(alp_usb_dev_t *dev) {
    (void)dev;
}

/* ------------------------------------------------------------------ */
/* Host role                                                           */
/* ------------------------------------------------------------------ */

alp_usb_host_t *alp_usb_host_open(void) {
    return NULL;
}

alp_status_t alp_usb_host_enable(alp_usb_host_t *host) {
    (void)host;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_host_disable(alp_usb_host_t *host) {
    (void)host;
    return ALP_ERR_NOSUPPORT;
}

void alp_usb_host_close(alp_usb_host_t *host) {
    (void)host;
}

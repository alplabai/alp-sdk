/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb.h
 * @brief ALP SDK USB abstraction (device and host).
 *
 * v0.3 deliverable.  v0.1 ships the public surface as a stub;
 * every entry point returns ALP_ERR_NOSUPPORT and `*_open` returns
 * NULL.
 *
 * Backends:
 *   - **Zephyr**   : USB device stack (`usb_dc_*`, USB-C if applicable);
 *                    USB host stack (`usbh_*`).  Lands v0.3.
 *   - **Yocto**    : `/dev/usb*` userspace via `libusb`; gadget mode
 *                    via configfs.
 *   - **Baremetal**: Vendor HAL USB peripheral driver.
 *
 * The shape splits along the device/host boundary, but most
 * embedded apps only ever do one role.  We expose both because
 * the EVK supports USB host (for mass storage / keyboards) on top
 * of the standard USB-C device role.
 *
 * Only the most-common device classes are wrapped at the SDK
 * surface (CDC-ACM virtual COM, MSC mass storage, HID).  Other
 * classes go through the vendor escape hatch at
 * `<alp/vendors/.../usb.h>`.
 */

#ifndef ALP_USB_H
#define ALP_USB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Standard device classes supported by the v0.3 wrapper. */
typedef enum {
    ALP_USB_DEVICE_CDC_ACM = 0,    /**< Virtual COM port. */
    ALP_USB_DEVICE_MSC     = 1,    /**< Mass-storage.  Backed by `<alp/storage.h>`. */
    ALP_USB_DEVICE_HID     = 2     /**< HID (keyboard / mouse / generic). */
} alp_usb_device_class_t;

typedef struct alp_usb_dev alp_usb_dev_t;

typedef struct {
    alp_usb_device_class_t  device_class;
    uint16_t                vendor_id;
    uint16_t                product_id;
    uint16_t                bcd_device;
    const char             *manufacturer;
    const char             *product;
    const char             *serial;
} alp_usb_device_config_t;

/**
 * @brief Acquire a USB device-role handle and present @p cfg's
 *        descriptor to the host on enumerate.
 */
alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg);

alp_status_t   alp_usb_device_enable(alp_usb_dev_t *dev);
alp_status_t   alp_usb_device_disable(alp_usb_dev_t *dev);

/** Send @p len bytes through the device's primary endpoint
 *  (CDC-ACM TX, HID IN, MSC bulk-in). */
alp_status_t   alp_usb_device_write(alp_usb_dev_t *dev,
                                    const uint8_t *data, size_t len,
                                    uint32_t timeout_ms);

/** Receive up to @p len bytes from the device's primary endpoint
 *  (CDC-ACM RX, HID OUT, MSC bulk-out). */
alp_status_t   alp_usb_device_read(alp_usb_dev_t *dev,
                                   uint8_t *data, size_t len,
                                   size_t *out_len,
                                   uint32_t timeout_ms);

void           alp_usb_device_close(alp_usb_dev_t *dev);

/* ------------------------------------------------------------------ */
/* USB host                                                            */
/* ------------------------------------------------------------------ */

typedef struct alp_usb_host alp_usb_host_t;

/** Acquire the USB host singleton. */
alp_usb_host_t *alp_usb_host_open(void);

alp_status_t    alp_usb_host_enable(alp_usb_host_t *host);
alp_status_t    alp_usb_host_disable(alp_usb_host_t *host);

void            alp_usb_host_close(alp_usb_host_t *host);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_USB_H */

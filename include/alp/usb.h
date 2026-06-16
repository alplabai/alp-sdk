/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb.h
 * @brief Alp SDK USB abstraction (device and host).
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
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.3 placeholder; surface skeleton only.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_USB_H
#define ALP_USB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/cap_instance.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Standard device classes supported by the v0.3 wrapper. */
typedef enum {
	ALP_USB_DEVICE_CDC_ACM = 0, /**< Virtual COM port. */
	ALP_USB_DEVICE_MSC     = 1, /**< Mass-storage.  Backed by `<alp/storage.h>`. */
	ALP_USB_DEVICE_HID     = 2  /**< HID (keyboard / mouse / generic). */
} alp_usb_device_class_t;

typedef struct alp_usb_dev alp_usb_dev_t;

typedef struct {
	alp_usb_device_class_t device_class;
	uint16_t               vendor_id;
	uint16_t               product_id;
	uint16_t               bcd_device;
	const char            *manufacturer;
	const char            *product;
	const char            *serial;
} alp_usb_device_config_t;

/**
 * @brief Acquire a USB device-role handle and present @p cfg's
 *        descriptor to the host on enumerate.
 *
 * @param[in] cfg  Device class + VID/PID + strings.  Must be non-NULL.
 *
 * @return Open handle on success; NULL with @ref alp_last_error set
 *         to @ref ALP_ERR_INVAL / @ref ALP_ERR_NOSUPPORT.
 */
alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg);

/**
 * @brief Attach the USB device to the bus (presents the descriptor on enumerate).
 *
 * @param[in] dev  Handle from @ref alp_usb_device_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_usb_device_enable(alp_usb_dev_t *dev);

/**
 * @brief Detach the USB device from the bus.  Pair with @ref alp_usb_device_enable.
 *
 * @param[in] dev  Handle from @ref alp_usb_device_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_usb_device_disable(alp_usb_dev_t *dev);

/**
 * @brief Send @p len bytes through the device's primary endpoint
 *        (CDC-ACM TX, HID IN, MSC bulk-in).
 *
 * @param[in] dev         Handle from @ref alp_usb_device_open.
 * @param[in] data        Bytes to send.  Must be non-NULL when @p len > 0.
 * @param[in] len         Byte count.
 * @param[in] timeout_ms  Max wait for the host to drain.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY (not enumerated) /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t
alp_usb_device_write(alp_usb_dev_t *dev, const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Receive up to @p len bytes from the device's primary endpoint
 *        (CDC-ACM RX, HID OUT, MSC bulk-out).
 *
 * @param[in]  dev         Handle from @ref alp_usb_device_open.
 * @param[out] data        Destination buffer.
 * @param[in]  len         Capacity of @p data.
 * @param[out] out_len     Receives the byte count actually read.
 * @param[in]  timeout_ms  Max wait.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_usb_device_read(
    alp_usb_dev_t *dev, uint8_t *data, size_t len, size_t *out_len, uint32_t timeout_ms);

/**
 * @brief Release the USB device handle.  Idempotent on NULL.
 *
 * @param[in] dev  Handle from @ref alp_usb_device_open, or NULL.
 */
void alp_usb_device_close(alp_usb_dev_t *dev);

/* ------------------------------------------------------------------ */
/* USB host                                                            */
/* ------------------------------------------------------------------ */

typedef struct alp_usb_host alp_usb_host_t;

/**
 * @brief Acquire the USB host singleton.
 *
 * @return Open handle on success; NULL with @ref alp_last_error
 *         set to @ref ALP_ERR_NOSUPPORT (no host stack wired) or
 *         @ref ALP_ERR_BUSY (already opened).
 */
alp_usb_host_t *alp_usb_host_open(void);

/**
 * @brief Start the host-role controller (enables enumeration of attached devices).
 *
 * @param[in] host  Handle from @ref alp_usb_host_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_usb_host_enable(alp_usb_host_t *host);

/**
 * @brief Stop the host-role controller.  Pair with @ref alp_usb_host_enable.
 *
 * @param[in] host  Handle from @ref alp_usb_host_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_usb_host_disable(alp_usb_host_t *host);

/**
 * @brief Release the USB host handle.  Idempotent on NULL.
 *
 * @param[in] host  Handle from @ref alp_usb_host_open, or NULL.
 */
void alp_usb_host_close(alp_usb_host_t *host);

/**
 * @brief Query the capabilities of an opened USB device handle.
 *
 * @param dev  Handle from @ref alp_usb_device_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p dev is NULL.
 */
const alp_capabilities_t *alp_usb_capabilities(const alp_usb_dev_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_USB_H */

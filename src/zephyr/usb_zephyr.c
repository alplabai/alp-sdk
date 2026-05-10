/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/usb.h>.
 *
 * Replaces the v0.1 NOSUPPORT stub.  Wraps Zephyr's USB device
 * stack (usb_enable / usb_disable, plus per-class config fragments
 * declared as Kconfig options on the consuming app -- USBD_CDC_ACM,
 * USBD_MSC, USBD_HID -- so this wrapper stays vendor-neutral).
 *
 * The CDC-ACM / MSC / HID device-class-specific endpoint plumbing
 * (read/write of the primary endpoints) lands in v0.3.x.  v0.3
 * ships the lifecycle (open / enable / disable / close) on top of
 * the unified usbd_*  API so apps can compile against the surface
 * today.
 *
 * Gated on CONFIG_ALP_SDK_USB.  When OFF, NULL/NOSUPPORT.
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/usb.h"
#include "handles.h"

#if defined(CONFIG_ALP_SDK_USB)
#include <zephyr/usb/usb_device.h>
#endif

#ifndef CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES
#define CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES 1
#endif

struct alp_usb_dev {
    bool in_use;
#if defined(CONFIG_ALP_SDK_USB)
    alp_usb_device_class_t cls;
    bool                   enabled;
#endif
};

struct alp_usb_host {
    bool in_use;
};

#if defined(CONFIG_ALP_SDK_USB)
static struct alp_usb_dev  g_usb_dev_pool[CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES];

static struct alp_usb_dev *usb_dev_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_usb_dev_pool); ++i) {
        if (!g_usb_dev_pool[i].in_use) {
            memset(&g_usb_dev_pool[i], 0, sizeof(g_usb_dev_pool[i]));
            g_usb_dev_pool[i].in_use = true;
            return &g_usb_dev_pool[i];
        }
    }
    return NULL;
}

static alp_status_t errno_to_alp(int err)
{
    switch (err) {
    case 0:
        return ALP_OK;
    case -EINVAL:
        return ALP_ERR_INVAL;
    case -EBUSY:
        return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:
        return ALP_ERR_TIMEOUT;
    case -EIO:
        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:
        return ALP_ERR_NOSUPPORT;
    case -ENOMEM:
        return ALP_ERR_NOMEM;
    default:
        return ALP_ERR_IO;
    }
}
#endif /* CONFIG_ALP_SDK_USB */

/* ================================================================== */
/* Device                                                              */
/* ================================================================== */

alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_USB)
    if (cfg->device_class > ALP_USB_DEVICE_HID) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    struct alp_usb_dev *d = usb_dev_pool_acquire();
    if (d == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    d->cls = cfg->device_class;
    /* The USB device stack reads its descriptor table from
     * Zephyr's static device descriptors; v0.3.x adds a runtime
     * patch that overlays cfg->vendor_id / product_id / strings
     * into the descriptors before usb_enable.  For v0.3 the open
     * succeeds and enable() drives the stack with the
     * compile-time descriptors. */
    return d;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_usb_device_enable(alp_usb_dev_t *dev)
{
    if (dev == NULL || !dev->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_USB)
    if (dev->enabled) return ALP_OK;
    int err = usb_enable(NULL);
    if (err == 0 || err == -EALREADY) {
        dev->enabled = true;
        return ALP_OK;
    }
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_usb_device_disable(alp_usb_dev_t *dev)
{
    if (dev == NULL || !dev->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_USB)
    if (!dev->enabled) return ALP_OK;
    int err = usb_disable();
    if (err == 0) dev->enabled = false;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_usb_device_write(alp_usb_dev_t *dev, const uint8_t *data, size_t len,
                                  uint32_t timeout_ms)
{
    if (dev == NULL || !dev->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    (void)timeout_ms;
    /* Per-class endpoint write -- CDC-ACM via uart_*; MSC via the
     * mass-storage stack; HID via hid_int_ep_write.  v0.3.x lands
     * the routing once the per-class wrappers are wired. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_device_read(alp_usb_dev_t *dev, uint8_t *data, size_t len, size_t *out_len,
                                 uint32_t timeout_ms)
{
    if (out_len != NULL) *out_len = 0;
    if (dev == NULL || !dev->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

void alp_usb_device_close(alp_usb_dev_t *dev)
{
    if (dev == NULL || !dev->in_use) return;
#if defined(CONFIG_ALP_SDK_USB)
    if (dev->enabled) {
        (void)usb_disable();
        dev->enabled = false;
    }
    dev->in_use = false;
#endif
}

/* ================================================================== */
/* Host                                                                */
/* ================================================================== */

alp_usb_host_t *alp_usb_host_open(void)
{
    alp_z_clear_last_error();
    /* Zephyr 3.7's usbh_* host stack is in tree but the SoC-side
     * controller drivers are still landing on a per-vendor basis.
     * The wrapper shape stands; v0.3.x flips the body once Alif's
     * controller exposes a stable `usbh_init` entry. */
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_usb_host_enable(alp_usb_host_t *host)
{
    if (host == NULL || !host->in_use) return ALP_ERR_NOT_READY;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_usb_host_disable(alp_usb_host_t *host)
{
    if (host == NULL || !host->in_use) return ALP_ERR_NOT_READY;
    return ALP_ERR_NOSUPPORT;
}

void alp_usb_host_close(alp_usb_host_t *host)
{
    if (host == NULL || !host->in_use) return;
    host->in_use = false;
}

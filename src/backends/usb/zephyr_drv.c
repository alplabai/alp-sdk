/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr USBD-stack backend for the <alp/usb.h> surface.
 * Lifted verbatim from src/zephyr/usb_zephyr.c (the legacy v0.3
 * wrapper) into a registry-shaped backend.  Registers as
 * silicon_ref="*" at priority 100 -- mirrors the design spec
 * Section 2 backend matrix (zephyr_drv wins on every SoC unless a
 * more specific backend registers).
 *
 * Device side: usb_enable / usb_disable lifecycle on top of
 * Zephyr's USB device stack.  Per-class endpoint read/write
 * (CDC-ACM / MSC / HID) lands once the per-class wrappers are
 * wired -- v0.3 contract preserved as-is.
 *
 * Host side: NOSUPPORT.  Zephyr 3.7's usbh_* host stack is in tree
 * but the SoC-side controller drivers are still landing on a
 * per-vendor basis; the wrapper shape stands and the body flips
 * once Alif's controller exposes a stable usbh_init entry.
 *
 * Gated on CONFIG_ALP_SDK_USB -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/usb.h>

#include "usb_ops.h"

#if defined(CONFIG_ALP_SDK_USB)
#include <zephyr/usb/usb_device.h>
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

struct usb_dev_be {
    alp_usb_device_class_t cls;
    bool                   enabled;
};

#ifndef CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES
#define CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES 2
#endif

static struct usb_dev_be _be_pool[CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES];
static bool              _be_pool_in_use[CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES];

static struct usb_dev_be *_be_alloc(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(_be_pool); ++i) {
        if (!_be_pool_in_use[i]) {
            memset(&_be_pool[i], 0, sizeof(_be_pool[i]));
            _be_pool_in_use[i] = true;
            return &_be_pool[i];
        }
    }
    return NULL;
}

static void _be_free(struct usb_dev_be *p)
{
    if (p == NULL) return;
    for (size_t i = 0; i < ARRAY_SIZE(_be_pool); ++i) {
        if (&_be_pool[i] == p) {
            _be_pool_in_use[i] = false;
            return;
        }
    }
}

#if defined(CONFIG_ALP_SDK_USB)
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
/* Device-side ops                                                     */
/* ================================================================== */

static alp_status_t z_dev_open(const alp_usb_device_config_t *cfg,
                               alp_usb_dev_state_t *st,
                               alp_capabilities_t *caps_out)
{
    if (cfg == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_USB)
    if (cfg->device_class > ALP_USB_DEVICE_HID) return ALP_ERR_INVAL;
    struct usb_dev_be *be = _be_alloc();
    if (be == NULL) return ALP_ERR_NOMEM;
    be->cls = cfg->device_class;
    be->enabled = false;
    st->be_data = be;
    /* The USB device stack reads its descriptor table from
     * Zephyr's static device descriptors; v0.3.x adds a runtime
     * patch that overlays cfg->vendor_id / product_id / strings
     * into the descriptors before usb_enable.  For now open
     * succeeds and enable() drives the stack with the
     * compile-time descriptors. */
    caps_out->flags = 0u;
    return ALP_OK;
#else
    (void)st;
    caps_out->flags = 0u;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_dev_enable(alp_usb_dev_state_t *st)
{
#if defined(CONFIG_ALP_SDK_USB)
    struct usb_dev_be *be = (struct usb_dev_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (be->enabled) return ALP_OK;
    int err = usb_enable(NULL);
    if (err == 0 || err == -EALREADY) {
        be->enabled = true;
        return ALP_OK;
    }
    return errno_to_alp(err);
#else
    (void)st;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_dev_disable(alp_usb_dev_state_t *st)
{
#if defined(CONFIG_ALP_SDK_USB)
    struct usb_dev_be *be = (struct usb_dev_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (!be->enabled) return ALP_OK;
    int err = usb_disable();
    if (err == 0) be->enabled = false;
    return errno_to_alp(err);
#else
    (void)st;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_dev_write(alp_usb_dev_state_t *st,
                                const uint8_t *data, size_t len,
                                uint32_t timeout_ms)
{
    (void)st;
    (void)data;
    (void)len;
    (void)timeout_ms;
    /* Per-class endpoint write -- CDC-ACM via uart_*; MSC via the
     * mass-storage stack; HID via hid_int_ep_write.  v0.3.x lands
     * the routing once the per-class wrappers are wired. */
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_dev_read(alp_usb_dev_state_t *st,
                               uint8_t *data, size_t len,
                               size_t *out_len, uint32_t timeout_ms)
{
    (void)st;
    (void)data;
    (void)len;
    (void)timeout_ms;
    if (out_len != NULL) *out_len = 0;
    return ALP_ERR_NOSUPPORT;
}

static void z_dev_close(alp_usb_dev_state_t *st)
{
#if defined(CONFIG_ALP_SDK_USB)
    struct usb_dev_be *be = (struct usb_dev_be *)st->be_data;
    if (be == NULL) return;
    if (be->enabled) {
        (void)usb_disable();
        be->enabled = false;
    }
    _be_free(be);
    st->be_data = NULL;
#else
    (void)st;
#endif
}

/* ================================================================== */
/* Host-side ops                                                       */
/* ================================================================== */

static alp_status_t z_host_open(alp_usb_host_state_t *st,
                                alp_capabilities_t *caps_out)
{
    (void)st;
    caps_out->flags = 0u;
    /* Zephyr 3.7's usbh_* host stack is in tree but the SoC-side
     * controller drivers are still landing on a per-vendor basis.
     * The wrapper shape stands; v0.3.x flips the body once Alif's
     * controller exposes a stable `usbh_init` entry. */
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_host_enable(alp_usb_host_state_t *st)
{
    (void)st;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_host_disable(alp_usb_host_state_t *st)
{
    (void)st;
    return ALP_ERR_NOSUPPORT;
}

static void z_host_close(alp_usb_host_state_t *st)
{
    (void)st;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_usb_ops_t _ops = {
    .dev_open    = z_dev_open,
    .dev_enable  = z_dev_enable,
    .dev_disable = z_dev_disable,
    .dev_write   = z_dev_write,
    .dev_read    = z_dev_read,
    .dev_close   = z_dev_close,

    .host_open    = z_host_open,
    .host_enable  = z_host_enable,
    .host_disable = z_host_disable,
    .host_close   = z_host_close,
};

ALP_BACKEND_REGISTER(usb, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});

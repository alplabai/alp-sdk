/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace GPIO backend for <alp/peripheral.h>'s alp_gpio_* surface.
 *
 * Binds against the kernel's GPIO character-device v2 ABI
 * (introduced in Linux 5.10) at `/dev/gpiochipN`.  The v2 ABI
 * supersedes the deprecated `/sys/class/gpio` sysfs interface
 * (deprecated in Linux 4.8, slated for removal) and the v1
 * chardev ABI (still functional but missing bias / debounce /
 * realtime-clock features).  No libgpiod dependency -- the ioctls
 * are invoked directly against the kernel UAPI in `<linux/gpio.h>`,
 * matching the pattern of peripheral_i2c.c / peripheral_spi.c /
 * peripheral_uart.c.
 *
 * Pin-id encoding
 * ---------------
 * Linux GPIO is two-dimensional: each chip (`/dev/gpiochipN`)
 * carries up to ~256 lines, identified by a `(chip, line_offset)`
 * pair.  The ALP API exposes a flat `uint32_t pin_id`, so we
 * pack the pair as:
 *
 *     pin_id = (chip << 16) | line_offset
 *
 * giving room for 65536 chips * 65536 lines.  Studio-side pin
 * allocators must emit this packed form on Yocto targets.
 * Example: chip 1, line 5 -> `pin_id = 0x00010005`.
 *
 * IRQ support
 * -----------
 * The v2 ABI returns a file descriptor that can be `poll()`'d
 * for line events (rising / falling / both edges, with optional
 * realtime-clock timestamps).  Wiring that into the ALP callback
 * model needs a background dispatcher thread (pthread) -- parked
 * for a follow-up so this commit doesn't pull pthread into every
 * Yocto build.  alp_gpio_irq_enable / alp_gpio_irq_disable
 * therefore return ALP_ERR_NOSUPPORT for now.
 *
 * Compiled only on Linux hosts/targets.
 */

#if !defined(__linux__)
#error "peripheral_gpio.c (yocto backend) requires a Linux target"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/gpio.h>

#include "alp/peripheral.h"
#include "alp_internal.h"
#include "yocto_errno.h"

#ifndef ALP_SDK_YOCTO_MAX_GPIO_HANDLES
#define ALP_SDK_YOCTO_MAX_GPIO_HANDLES 16
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define ALP_GPIO_PIN_CHIP(pin_id) (((pin_id) >> 16) & 0xFFFFu)
#define ALP_GPIO_PIN_LINE(pin_id) ((pin_id)&0xFFFFu)

struct alp_gpio {
    bool     in_use;
    int      line_fd; /* GPIO_V2_GET_LINE_IOCTL-returned request fd */
    uint32_t pin_id;
    bool     is_output;
};

static struct alp_gpio  g_gpio_pool[ALP_SDK_YOCTO_MAX_GPIO_HANDLES];

static struct alp_gpio *pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_gpio_pool); ++i) {
        if (!g_gpio_pool[i].in_use) {
            memset(&g_gpio_pool[i], 0, sizeof(g_gpio_pool[i]));
            g_gpio_pool[i].in_use  = true;
            g_gpio_pool[i].line_fd = -1;
            return &g_gpio_pool[i];
        }
    }
    return NULL;
}

static void pool_release(struct alp_gpio *h)
{
    if (h == NULL) {
        return;
    }
    if (h->line_fd >= 0) {
        (void)close(h->line_fd);
        h->line_fd = -1;
    }
    h->in_use = false;
}

static uint64_t pull_to_flags(alp_gpio_pull_t pull)
{
    switch (pull) {
    case ALP_GPIO_PULL_UP:
        return GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    case ALP_GPIO_PULL_DOWN:
        return GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    case ALP_GPIO_PULL_NONE:
    default:
        return GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    }
}

alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
    uint16_t chip_idx    = (uint16_t)ALP_GPIO_PIN_CHIP(pin_id);
    uint16_t line_offset = (uint16_t)ALP_GPIO_PIN_LINE(pin_id);

    char     chip_path[32];
    int      n = snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_idx);
    if (n < 0 || (size_t)n >= sizeof(chip_path)) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    int chip_fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        return NULL;
    }

    /* Default to input on open; alp_gpio_configure switches direction
     * post-acquire if the caller wants output. */
    struct gpio_v2_line_request req = {0};
    req.offsets[0]                  = line_offset;
    req.num_lines                   = 1;
    req.config.flags                = GPIO_V2_LINE_FLAG_INPUT;
    (void)snprintf(req.consumer, sizeof(req.consumer), "alp-sdk");

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        (void)close(chip_fd);
        return NULL;
    }
    /* The line request owns its own fd; the chip fd is no longer
     * needed. */
    (void)close(chip_fd);

    struct alp_gpio *h = pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        (void)close(req.fd);
        return NULL;
    }
    h->line_fd   = req.fd;
    h->pin_id    = pin_id;
    h->is_output = false;
    return h;
}

alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
    if (pin == NULL || !pin->in_use) {
        return ALP_ERR_INVAL;
    }
    struct gpio_v2_line_config cfg = {0};
    cfg.flags                      = pull_to_flags(pull);
    if (dir == ALP_GPIO_OUTPUT) {
        cfg.flags |= GPIO_V2_LINE_FLAG_OUTPUT;
    } else {
        cfg.flags |= GPIO_V2_LINE_FLAG_INPUT;
    }
    if (ioctl(pin->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    pin->is_output = (dir == ALP_GPIO_OUTPUT);
    return ALP_OK;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
    if (pin == NULL || !pin->in_use) {
        return ALP_ERR_INVAL;
    }
    if (!pin->is_output) {
        /* Writing to an input line is a configuration mistake;
         * the kernel would refuse the ioctl anyway. */
        return ALP_ERR_INVAL;
    }
    struct gpio_v2_line_values vals = {
        .bits = level ? 1ULL : 0ULL, .mask = 1ULL, /* line 0 in this request */
    };
    if (ioctl(pin->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    return ALP_OK;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
    if (pin == NULL || !pin->in_use || level == NULL) {
        return ALP_ERR_INVAL;
    }
    struct gpio_v2_line_values vals = {
        .bits = 0,
        .mask = 1ULL,
    };
    if (ioctl(pin->line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    *level = (vals.bits & 1ULL) != 0;
    return ALP_OK;
}

alp_status_t alp_gpio_irq_enable(alp_gpio_t *pin, alp_gpio_edge_t edge, alp_gpio_cb_t cb,
                                 void *user)
{
    (void)pin;
    (void)edge;
    (void)cb;
    (void)user;
    /* GPIO_V2 line events are exposed through the request fd's
     * read() interface -- wiring callback dispatch needs a
     * background thread to poll() the fd and invoke the user
     * callback.  Parked until a Yocto caller actually needs it
     * so this commit doesn't pull pthread into every build. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin)
{
    (void)pin;
    return ALP_ERR_NOSUPPORT;
}

void alp_gpio_close(alp_gpio_t *pin)
{
    pool_release(pin);
}

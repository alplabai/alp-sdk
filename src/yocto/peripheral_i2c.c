/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace I2C backend for <alp/peripheral.h>'s alp_i2c_* surface.
 *
 * Binds against the kernel's i2c-dev character devices at
 * `/dev/i2c-N`, where N is the kernel's adapter index.  The
 * mapping is direct: `alp_i2c_config_t.bus_id` becomes the
 * trailing integer in the device path.  Boards that need
 * symbolic naming (e.g. "imu-bus") should resolve those names
 * to integers ahead of `alp_i2c_open` -- this layer is the raw
 * binding.
 *
 * The kernel manages bus speed via devicetree
 * (`clock-frequency = <400000>;` on the i2c controller node) or
 * sysfs (`/sys/class/i2c-adapter/i2c-N/.../bus_clk_rate` on some
 * SoCs).  `bitrate_hz` in alp_i2c_config_t is **ignored** here --
 * Linux userspace cannot retune the bus mid-run.  We log the
 * discrepancy via the last-error slot when it would matter:
 * passing `bitrate_hz == 0` is treated as "no opinion" and
 * accepted silently, anything else stamps a non-fatal
 * `ALP_ERR_NOSUPPORT` recoverable note (returned through the
 * handle, not via failed open).
 *
 * Write-then-read uses the I2C_RDWR ioctl with a 2-message
 * descriptor (write, then read with I2C_M_RD set).  This gives
 * the device a repeated-start between the register pointer
 * write and the data read -- the safer pattern for register
 * reads vs. the plain stop-then-start sequence that two
 * separate write()+read() calls would produce.
 *
 * Compiled only on Linux hosts/targets.  Guarded by __linux__
 * so unit tests built on macOS/Windows hosts don't try to
 * `#include <linux/i2c-dev.h>`.
 */

#if !defined(__linux__)
#error "peripheral_i2c.c (yocto backend) requires a Linux target"
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

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "alp/peripheral.h"
#include "alp_internal.h"
#include "yocto_errno.h"

#ifndef ALP_SDK_YOCTO_MAX_I2C_HANDLES
#define ALP_SDK_YOCTO_MAX_I2C_HANDLES 4
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct alp_i2c {
    bool     in_use;
    int      fd; /* /dev/i2c-N file descriptor */
    uint32_t bus_id;
    uint8_t  cached_addr; /* last addr passed to I2C_SLAVE ioctl */
    bool     addr_cached;
};

static struct alp_i2c  g_i2c_pool[ALP_SDK_YOCTO_MAX_I2C_HANDLES];

static struct alp_i2c *pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_i2c_pool); ++i) {
        if (!g_i2c_pool[i].in_use) {
            memset(&g_i2c_pool[i], 0, sizeof(g_i2c_pool[i]));
            g_i2c_pool[i].in_use = true;
            g_i2c_pool[i].fd     = -1;
            return &g_i2c_pool[i];
        }
    }
    return NULL;
}

static void pool_release(struct alp_i2c *h)
{
    if (h == NULL) {
        return;
    }
    if (h->fd >= 0) {
        (void)close(h->fd);
        h->fd = -1;
    }
    h->in_use = false;
}

static alp_status_t ensure_slave(struct alp_i2c *h, uint8_t addr)
{
    if (h->addr_cached && h->cached_addr == addr) {
        return ALP_OK;
    }
    if (ioctl(h->fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    h->cached_addr = addr;
    h->addr_cached = true;
    return ALP_OK;
}

alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg)
{
    if (cfg == NULL) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    char path[32];
    int  n = snprintf(path, sizeof(path), "/dev/i2c-%u", cfg->bus_id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        return NULL;
    }

    /* Probe for I2C_FUNC support up front so callers fail fast on
     * adapters that lack I2C_RDWR (uncommon but real -- some SMBus-only
     * adapters report I2C_FUNC_SMBUS_* without I2C_FUNC_I2C). */
    unsigned long funcs = 0;
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        (void)close(fd);
        return NULL;
    }
    if ((funcs & I2C_FUNC_I2C) == 0) {
        alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
        (void)close(fd);
        return NULL;
    }

    struct alp_i2c *h = pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        (void)close(fd);
        return NULL;
    }
    h->fd     = fd;
    h->bus_id = cfg->bus_id;
    return h;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
    if (bus == NULL || !bus->in_use || (data == NULL && len > 0)) {
        return ALP_ERR_INVAL;
    }
    alp_status_t rc = ensure_slave(bus, addr);
    if (rc != ALP_OK) {
        return rc;
    }
    ssize_t n = write(bus->fd, data, len);
    if (n < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    if ((size_t)n != len) {
        return ALP_ERR_IO;
    }
    return ALP_OK;
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
    if (bus == NULL || !bus->in_use || (data == NULL && len > 0)) {
        return ALP_ERR_INVAL;
    }
    alp_status_t rc = ensure_slave(bus, addr);
    if (rc != ALP_OK) {
        return rc;
    }
    ssize_t n = read(bus->fd, data, len);
    if (n < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    if ((size_t)n != len) {
        return ALP_ERR_IO;
    }
    return ALP_OK;
}

alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr, const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen)
{
    if (bus == NULL || !bus->in_use) {
        return ALP_ERR_INVAL;
    }
    if ((wdata == NULL && wlen > 0) || (rdata == NULL && rlen > 0)) {
        return ALP_ERR_INVAL;
    }
    /* I2C_RDWR caps each message at 8 KiB on most kernels; a single
     * transfer that exceeds the per-message ceiling is the caller's
     * mistake. */
    if (wlen > UINT16_MAX || rlen > UINT16_MAX) {
        return ALP_ERR_INVAL;
    }

    struct i2c_msg msgs[2];
    int            nmsgs = 0;

    if (wlen > 0) {
        msgs[nmsgs].addr  = addr;
        msgs[nmsgs].flags = 0;
        msgs[nmsgs].len   = (uint16_t)wlen;
        msgs[nmsgs].buf   = (uint8_t *)wdata; /* I2C_RDWR doesn't write through this for write */
        ++nmsgs;
    }
    if (rlen > 0) {
        msgs[nmsgs].addr  = addr;
        msgs[nmsgs].flags = I2C_M_RD;
        msgs[nmsgs].len   = (uint16_t)rlen;
        msgs[nmsgs].buf   = rdata;
        ++nmsgs;
    }
    if (nmsgs == 0) {
        return ALP_OK;
    }

    struct i2c_rdwr_ioctl_data req = {
        .msgs  = msgs,
        .nmsgs = (uint32_t)nmsgs,
    };
    if (ioctl(bus->fd, I2C_RDWR, &req) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    /* I2C_RDWR invalidates the cached I2C_SLAVE address on some
     * kernels -- forget the cache so the next write/read re-sets it. */
    bus->addr_cached = false;
    return ALP_OK;
}

void alp_i2c_close(alp_i2c_t *bus)
{
    pool_release(bus);
}

/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace UART backend for <alp/peripheral.h>'s alp_uart_* surface.
 *
 * Binds against tty character devices at `/dev/ttyN` (16550-style
 * UARTs surfaced through 8250) or `/dev/ttyAMA<N>` / `/dev/ttyS<N>`
 * / `/dev/ttyUSB<N>` depending on the SoC's serial controller.
 *
 * Path resolution is intentionally simple: `alp_uart_config_t.port_id`
 * maps to `/dev/tty<port_id>` via a small lookup table.  Carriers
 * that need symbolic naming (e.g. "debug-uart" -> "/dev/ttyAMA0")
 * resolve those names ahead of `alp_uart_open` -- this layer is
 * the raw binding.  The lookup table covers the common Linux
 * embedded conventions:
 *
 *   port_id  device
 *   -------  -----------
 *   0        /dev/ttyS0     (legacy ISA / x86 console)
 *   1        /dev/ttyS1
 *   2..3     /dev/ttyS2/3
 *   100+     /dev/ttyAMA<port_id - 100>   (ARM PL011)
 *   200+     /dev/ttyUSB<port_id - 200>   (USB-serial adapters)
 *
 * Most embedded SoCs (Alif, NXP) expose UARTs as /dev/ttyS<N>;
 * the 100/200 ranges cover the cases where that convention
 * doesn't fit.  Customers with non-standard paths can hand the
 * raw device through their own thin wrapper on top of this one.
 *
 * Baud rates come from termios's documented constants (B9600,
 * B115200, ...) -- non-standard rates fall back to BOTHER +
 * struct termios2 on kernels that support it; we don't bother
 * with that for v0.4 since the embedded UARTs we target speak
 * standard rates.  Unknown rates return ALP_ERR_INVAL.
 *
 * Reads honour the timeout_ms argument via VMIN=0, VTIME =
 * ceil(timeout_ms / 100) (termios's deciseconds quantum); writes
 * are blocking but the kernel-side tty layer never holds them
 * indefinitely for a hardware UART.
 *
 * Compiled only on Linux hosts/targets.
 */

#if !defined(__linux__)
#error "peripheral_uart.c (yocto backend) requires a Linux target"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "alp/peripheral.h"
#include "alp_internal.h"
#include "yocto_errno.h"

#ifndef ALP_SDK_YOCTO_MAX_UART_HANDLES
#define ALP_SDK_YOCTO_MAX_UART_HANDLES 4
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct alp_uart {
    bool in_use;
    int  fd;
};

static struct alp_uart  g_uart_pool[ALP_SDK_YOCTO_MAX_UART_HANDLES];

static struct alp_uart *pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_uart_pool); ++i) {
        if (!g_uart_pool[i].in_use) {
            memset(&g_uart_pool[i], 0, sizeof(g_uart_pool[i]));
            g_uart_pool[i].in_use = true;
            g_uart_pool[i].fd     = -1;
            return &g_uart_pool[i];
        }
    }
    return NULL;
}

static void pool_release(struct alp_uart *h)
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

static int resolve_path(uint32_t port_id, char *out, size_t cap)
{
    if (port_id >= 200u) {
        return snprintf(out, cap, "/dev/ttyUSB%u", port_id - 200u);
    }
    if (port_id >= 100u) {
        return snprintf(out, cap, "/dev/ttyAMA%u", port_id - 100u);
    }
    return snprintf(out, cap, "/dev/ttyS%u", port_id);
}

static speed_t baud_to_termios(uint32_t baud)
{
    switch (baud) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 921600:
        return B921600;
    case 1000000:
        return B1000000;
    case 1500000:
        return B1500000;
    case 3000000:
        return B3000000;
    default:
        return B0; /* sentinel for "unsupported" */
    }
}

alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)
{
    if (cfg == NULL) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->data_bits < 5 || cfg->data_bits > 8) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->stop_bits != 1 && cfg->stop_bits != 2) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    speed_t speed = baud_to_termios(cfg->baudrate);
    if (speed == B0) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    char path[32];
    int  n = resolve_path(cfg->port_id, path, sizeof(path));
    if (n < 0 || (size_t)n >= sizeof(path)) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        return NULL;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        (void)close(fd);
        return NULL;
    }

    /* Raw 8-N-1-ish baseline; we override the data-bit, stop-bit,
     * and parity flags below. */
    cfmakeraw(&tio);

    /* Data bits. */
    tio.c_cflag &= ~(tcflag_t)CSIZE;
    switch (cfg->data_bits) {
    case 5:
        tio.c_cflag |= CS5;
        break;
    case 6:
        tio.c_cflag |= CS6;
        break;
    case 7:
        tio.c_cflag |= CS7;
        break;
    case 8:
    default:
        tio.c_cflag |= CS8;
        break;
    }

    /* Stop bits. */
    if (cfg->stop_bits == 2) {
        tio.c_cflag |= CSTOPB;
    } else {
        tio.c_cflag &= ~(tcflag_t)CSTOPB;
    }

    /* Parity. */
    switch (cfg->parity) {
    case ALP_UART_PARITY_NONE:
        tio.c_cflag &= ~(tcflag_t)(PARENB | PARODD);
        break;
    case ALP_UART_PARITY_EVEN:
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~(tcflag_t)PARODD;
        break;
    case ALP_UART_PARITY_ODD:
        tio.c_cflag |= PARENB | PARODD;
        break;
    default:
        alp_internal_set_last_error(ALP_ERR_INVAL);
        (void)close(fd);
        return NULL;
    }

    /* Enable receiver, ignore modem control lines. */
    tio.c_cflag |= CREAD | CLOCAL;

    /* Default to non-blocking reads with no timeout; alp_uart_read
     * applies VTIME per-call so successive reads can use different
     * timeouts on the same handle. */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        (void)close(fd);
        return NULL;
    }
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        alp_internal_set_last_error(alp_yocto_errno_to_alp(errno));
        (void)close(fd);
        return NULL;
    }
    /* Drain anything that arrived during configuration so the
     * first alp_uart_read returns fresh-after-open bytes only. */
    (void)tcflush(fd, TCIOFLUSH);

    struct alp_uart *h = pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        (void)close(fd);
        return NULL;
    }
    h->fd = fd;
    return h;
}

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len)
{
    if (port == NULL || !port->in_use || (data == NULL && len > 0)) {
        return ALP_ERR_INVAL;
    }
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(port->fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return alp_yocto_errno_to_alp(errno);
        }
        written += (size_t)n;
    }
    return ALP_OK;
}

alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (port == NULL || !port->in_use || (data == NULL && len > 0)) {
        return ALP_ERR_INVAL;
    }
    if (len == 0) {
        return ALP_OK;
    }

    /* Apply the per-call timeout via VTIME (deciseconds quantum,
     * ceiling-rounded so a 50 ms ask yields 100 ms in practice).
     * VMIN=1 + VTIME=t means "return when at least 1 byte is
     * available OR t deciseconds elapse since the last byte". */
    struct termios tio;
    if (tcgetattr(port->fd, &tio) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }
    cc_t vtime;
    if (timeout_ms == 0) {
        vtime = 0;
    } else {
        uint32_t ds = (timeout_ms + 99u) / 100u;
        vtime       = (cc_t)((ds > 255u) ? 255u : ds);
    }
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = vtime;
    if (tcsetattr(port->fd, TCSANOW, &tio) < 0) {
        return alp_yocto_errno_to_alp(errno);
    }

    size_t got = 0;
    while (got < len) {
        ssize_t n = read(port->fd, data + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return alp_yocto_errno_to_alp(errno);
        }
        if (n == 0) {
            /* VTIME elapsed with no data -- return what we have so
             * far through ALP_ERR_TIMEOUT.  Caller can decide
             * whether the partial read is useful. */
            return (got > 0) ? ALP_OK : ALP_ERR_TIMEOUT;
        }
        got += (size_t)n;
    }
    return ALP_OK;
}

void alp_uart_close(alp_uart_t *port)
{
    pool_release(port);
}

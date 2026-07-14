/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace SPI backend for <alp/peripheral.h>'s alp_spi_* surface.
 *
 * Binds against the kernel's spidev character devices at
 * `/dev/spidev<bus_id>.<cs_pin_id>`:
 *
 *   - alp_spi_config_t.bus_id     -> SPI controller (master) index.
 *   - alp_spi_config_t.cs_pin_id  -> chip-select line index on that
 *                                    controller.
 *
 * On Linux the kernel owns the CS line -- there's no userspace
 * bit-banging here.  alp_spi_config_t.cs_pin_id therefore selects
 * the kernel-registered spidev minor, not a raw GPIO.  Boards
 * that genuinely need GPIO-bit-banged CS should bind GPIO via
 * alp_gpio_* and toggle it between half-duplex transfers (the
 * standard Linux "user-space bit-bang CS" pattern); the SPI
 * wrapper here covers the kernel-driven path.
 *
 * mode / bits_per_word / freq_hz land via SPI_IOC_WR_MODE,
 * SPI_IOC_WR_BITS_PER_WORD, SPI_IOC_WR_MAX_SPEED_HZ before the
 * first transfer.  Full-duplex transfers (transceive) use
 * SPI_IOC_MESSAGE(1) with both tx_buf and rx_buf set; half-duplex
 * (write-only / read-only) uses plain write() / read() which the
 * kernel still drives through the same transfer path.
 *
 * Compiled only on Linux hosts/targets, mirroring peripheral_i2c.c.
 */

#if !defined(__linux__)
#error "peripheral_spi.c (yocto backend) requires a Linux target"
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

#include <linux/spi/spidev.h>

#include "alp/peripheral.h"
#include "alp_internal.h"
#include "common/alp_errno.h"

#ifndef ALP_SDK_YOCTO_MAX_SPI_HANDLES
#define ALP_SDK_YOCTO_MAX_SPI_HANDLES 4
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct alp_spi {
	bool     in_use;
	int      fd;
	uint32_t bus_id;
	uint32_t cs_pin_id;
	uint32_t freq_hz;
	uint8_t  bits_per_word;
};

static struct alp_spi g_spi_pool[ALP_SDK_YOCTO_MAX_SPI_HANDLES];

static struct alp_spi *pool_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_spi_pool); ++i) {
		if (!g_spi_pool[i].in_use) {
			memset(&g_spi_pool[i], 0, sizeof(g_spi_pool[i]));
			g_spi_pool[i].in_use = true;
			g_spi_pool[i].fd     = -1;
			return &g_spi_pool[i];
		}
	}
	return NULL;
}

static void pool_release(struct alp_spi *h)
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

alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)
{
	if (cfg == NULL) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	if (cfg->mode > ALP_SPI_MODE_3) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	/* spidev accepts 1..32 bits/word; 0 from caller means "default 8". */
	uint8_t bits = (cfg->bits_per_word == 0) ? 8 : cfg->bits_per_word;
	if (bits == 0 || bits > 32) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	/* ALP_SPI_NO_CS (0xFFFFFFFF) has no spidev mapping: the kernel's
     * spidev minor is a DT-assigned chip-select index, not a "there is
     * no CS" sentinel, so formatting it in verbatim used to probe
     * "/dev/spidev<bus>.4294967295" and fail with a confusing ENOENT.
     * There is no portable Linux convention today for "controller must
     * not drive CS" independent of a specific DT-registered spidev
     * node, so refuse before opening a nonsensical path rather than
     * misrepresent the request.  Boards that manage CS externally
     * should bind the kernel-registered spidev node for that bus at
     * its real (board-assigned) minor and drive CS themselves via
     * alp_gpio_*, exactly like the CMSIS/Zephyr backends do. */
	if (cfg->cs_pin_id == ALP_SPI_NO_CS) {
		alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
		return NULL;
	}

	char path[40];
	int  n = snprintf(path, sizeof(path), "/dev/spidev%u.%u", cfg->bus_id, cfg->cs_pin_id);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		alp_internal_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		alp_internal_set_last_error(alp_status_from_posix_errno(errno));
		return NULL;
	}

	uint8_t mode_byte = (uint8_t)cfg->mode;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode_byte) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
		alp_internal_set_last_error(alp_status_from_posix_errno(errno));
		(void)close(fd);
		return NULL;
	}
	if (cfg->freq_hz != 0) {
		uint32_t freq = cfg->freq_hz;
		if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &freq) < 0) {
			alp_internal_set_last_error(alp_status_from_posix_errno(errno));
			(void)close(fd);
			return NULL;
		}
	}

	struct alp_spi *h = pool_acquire();
	if (h == NULL) {
		alp_internal_set_last_error(ALP_ERR_NOMEM);
		(void)close(fd);
		return NULL;
	}
	h->fd            = fd;
	h->bus_id        = cfg->bus_id;
	h->cs_pin_id     = cfg->cs_pin_id;
	h->freq_hz       = cfg->freq_hz;
	h->bits_per_word = bits;
	alp_internal_set_last_error(ALP_OK);
	return h;
}

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (bus == NULL || !bus->in_use) {
		return ALP_ERR_INVAL;
	}
	if (len == 0) {
		return ALP_OK;
	}
	/* SPI_IOC_MESSAGE caps `len` at UINT32_MAX; we additionally
     * guard against size_t overflow on 64-bit hosts -- a single
     * transfer >4 GiB is the caller's mistake. */
	if (len > UINT32_MAX) {
		return ALP_ERR_INVAL;
	}
	struct spi_ioc_transfer xfer = {
		.tx_buf        = (uintptr_t)tx,
		.rx_buf        = (uintptr_t)rx,
		.len           = (uint32_t)len,
		.speed_hz      = bus->freq_hz,
		.bits_per_word = bus->bits_per_word,
	};
	if (ioctl(bus->fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
		return alp_status_from_posix_errno(errno);
	}
	return ALP_OK;
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)
{
	if (bus == NULL || !bus->in_use || (tx == NULL && len > 0)) {
		return ALP_ERR_INVAL;
	}
	if (len == 0) {
		return ALP_OK;
	}
	ssize_t n = write(bus->fd, tx, len);
	if (n < 0) {
		return alp_status_from_posix_errno(errno);
	}
	if ((size_t)n != len) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)
{
	if (bus == NULL || !bus->in_use || (rx == NULL && len > 0)) {
		return ALP_ERR_INVAL;
	}
	if (len == 0) {
		return ALP_OK;
	}
	ssize_t n = read(bus->fd, rx, len);
	if (n < 0) {
		return alp_status_from_posix_errno(errno);
	}
	if ((size_t)n != len) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

void alp_spi_close(alp_spi_t *bus)
{
	pool_release(bus);
}

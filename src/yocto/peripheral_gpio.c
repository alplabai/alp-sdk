/*
 * Copyright 2026 Alp Lab AB
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
 * realtime-clock timestamps).  A single shared pthread runs the
 * poll() loop across every pin that has IRQ enabled, dispatching
 * to the user callback when an edge fires.  A wake-eventfd lets
 * mutators (irq_enable / irq_disable / close) interrupt the
 * poll() so the dispatcher re-snapshots its slot table.
 *
 * Callbacks run on the dispatcher thread while the dispatcher
 * mutex is held -- callers MUST NOT call alp_gpio_irq_disable
 * or alp_gpio_close from within a callback (deadlock).  The
 * dispatcher starts lazily on the first alp_gpio_irq_enable
 * and runs for the lifetime of the process.
 *
 * Compiled only on Linux hosts/targets.  Requires pthread (-lpthread
 * on glibc, link in src/yocto/CMakeLists.txt).
 */

#if !defined(__linux__)
#error "peripheral_gpio.c (yocto backend) requires a Linux target"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
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
	bool          in_use;
	int           line_fd; /* GPIO_V2_GET_LINE_IOCTL-returned request fd */
	uint32_t      pin_id;
	bool          is_output;
	bool          irq_enabled; /* dispatcher tracks this slot if true */
	alp_gpio_cb_t irq_cb;      /* invoked by dispatcher on edge event */
	void         *irq_user;
};

static struct alp_gpio g_gpio_pool[ALP_SDK_YOCTO_MAX_GPIO_HANDLES];

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

	char chip_path[32];
	int  n = snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_idx);
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
	struct gpio_v2_line_request req = { 0 };
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
	struct gpio_v2_line_config cfg = { 0 };
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

/* ================================================================== */
/* IRQ dispatcher                                                      */
/*                                                                     */
/* One background pthread owns the poll() loop across every pin that   */
/* has IRQ enabled.  A wake-eventfd lets `_enable` / `_disable` /      */
/* close() interrupt the poll() so the thread re-snapshots the slot    */
/* table and picks up the change.                                      */
/*                                                                     */
/* Callbacks run on the dispatcher thread while the dispatcher mutex   */
/* is held -- callers MUST NOT call alp_gpio_irq_disable or            */
/* alp_gpio_close from within a callback (it would deadlock).          */
/* Document this in the public alp_gpio_irq_enable contract.           */
/*                                                                     */
/* The thread is started lazily on first irq_enable and runs for the   */
/* lifetime of the process -- no teardown plumbing is required for     */
/* v0.4 since the slot count is small and the thread is mostly         */
/* sleeping inside poll().                                             */
/* ================================================================== */

static struct {
	pthread_mutex_t mu;
	pthread_t       thread;
	int             wake_fd; /* eventfd, written by mutators */
	bool            started;
} g_irq = {
	.mu      = PTHREAD_MUTEX_INITIALIZER,
	.wake_fd = -1,
	.started = false,
};

static void irq_wake(void)
{
	if (g_irq.wake_fd < 0) {
		return;
	}
	uint64_t v = 1;
	(void)write(g_irq.wake_fd, &v, sizeof(v));
}

static void *irq_dispatcher(void *arg)
{
	(void)arg;
	struct pollfd    fds[ALP_SDK_YOCTO_MAX_GPIO_HANDLES + 1];
	struct alp_gpio *slot_pins[ALP_SDK_YOCTO_MAX_GPIO_HANDLES];
	/* We deliberately drain only one event per slot per wake-up so a
     * pin that fires faster than we dispatch doesn't starve siblings;
     * the kernel queue keeps subsequent events buffered. */
	struct gpio_v2_line_event ev;
	while (1) {
		pthread_mutex_lock(&g_irq.mu);
		size_t nfds    = 1;
		fds[0].fd      = g_irq.wake_fd;
		fds[0].events  = POLLIN;
		fds[0].revents = 0;
		for (size_t i = 0; i < ARRAY_SIZE(g_gpio_pool); ++i) {
			struct alp_gpio *p = &g_gpio_pool[i];
			if (!p->in_use || !p->irq_enabled || p->line_fd < 0) {
				continue;
			}
			if (nfds > ARRAY_SIZE(fds)) {
				break;
			}
			fds[nfds].fd        = p->line_fd;
			fds[nfds].events    = POLLIN;
			fds[nfds].revents   = 0;
			slot_pins[nfds - 1] = p;
			++nfds;
		}
		pthread_mutex_unlock(&g_irq.mu);

		int rc = poll(fds, nfds, -1);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			/* Fatal poll() error -- exit the dispatcher.  Future
             * irq_enable calls will start it lazily again. */
			pthread_mutex_lock(&g_irq.mu);
			g_irq.started = false;
			pthread_mutex_unlock(&g_irq.mu);
			return NULL;
		}

		if (fds[0].revents & POLLIN) {
			uint64_t drain;
			(void)read(g_irq.wake_fd, &drain, sizeof(drain));
		}

		/* Re-acquire the mutex to invoke callbacks safely against
         * concurrent irq_disable / close.  Callers are documented as
         * forbidden to call irq_disable / close from within a
         * callback; with that contract the lock-during-callback
         * pattern is deadlock-free. */
		pthread_mutex_lock(&g_irq.mu);
		for (size_t i = 1; i < nfds; ++i) {
			if (!(fds[i].revents & POLLIN)) {
				continue;
			}
			struct alp_gpio *p = slot_pins[i - 1];
			if (!p->in_use || !p->irq_enabled || p->line_fd != fds[i].fd) {
				/* Slot disabled while we were sleeping; skip. */
				continue;
			}
			ssize_t r = read(p->line_fd, &ev, sizeof(ev));
			if (r != (ssize_t)sizeof(ev)) {
				continue;
			}
			if (p->irq_cb != NULL) {
				p->irq_cb((alp_gpio_t *)p, p->irq_user);
			}
		}
		pthread_mutex_unlock(&g_irq.mu);
	}
}

static alp_status_t irq_start_dispatcher_locked(void)
{
	if (g_irq.started) {
		return ALP_OK;
	}
	g_irq.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (g_irq.wake_fd < 0) {
		return alp_yocto_errno_to_alp(errno);
	}
	int rc = pthread_create(&g_irq.thread, NULL, irq_dispatcher, NULL);
	if (rc != 0) {
		(void)close(g_irq.wake_fd);
		g_irq.wake_fd = -1;
		return alp_yocto_errno_to_alp(rc);
	}
	/* Best-effort detach so we don't leak the join handle; we never
     * stop the thread explicitly. */
	(void)pthread_detach(g_irq.thread);
	g_irq.started = true;
	return ALP_OK;
}

static uint64_t edge_to_flags(alp_gpio_edge_t edge)
{
	switch (edge) {
	case ALP_GPIO_EDGE_RISING:
		return GPIO_V2_LINE_FLAG_EDGE_RISING;
	case ALP_GPIO_EDGE_FALLING:
		return GPIO_V2_LINE_FLAG_EDGE_FALLING;
	case ALP_GPIO_EDGE_BOTH:
		return GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
	case ALP_GPIO_EDGE_NONE:
	default:
		return 0;
	}
}

alp_status_t
alp_gpio_irq_enable(alp_gpio_t *pin, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)
{
	if (pin == NULL || !pin->in_use || cb == NULL) {
		return ALP_ERR_INVAL;
	}
	uint64_t edge_flags = edge_to_flags(edge);
	if (edge_flags == 0) {
		/* No edge selected -- caller must pick rising / falling /
         * both.  Treat NONE as a configuration mistake rather than
         * silently doing nothing. */
		return ALP_ERR_INVAL;
	}

	/* Re-configure the line as input + edge-detect.  Output and
     * edge-detect flags are mutually exclusive at the kernel layer. */
	struct gpio_v2_line_config cfg = { 0 };
	cfg.flags                      = GPIO_V2_LINE_FLAG_INPUT | edge_flags;
	if (ioctl(pin->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) < 0) {
		return alp_yocto_errno_to_alp(errno);
	}

	pthread_mutex_lock(&g_irq.mu);
	alp_status_t rc = irq_start_dispatcher_locked();
	if (rc != ALP_OK) {
		pthread_mutex_unlock(&g_irq.mu);
		return rc;
	}
	pin->irq_cb      = cb;
	pin->irq_user    = user;
	pin->irq_enabled = true;
	pin->is_output   = false;
	pthread_mutex_unlock(&g_irq.mu);

	irq_wake();
	return ALP_OK;
}

alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin)
{
	if (pin == NULL || !pin->in_use) {
		return ALP_ERR_INVAL;
	}
	pthread_mutex_lock(&g_irq.mu);
	pin->irq_enabled = false;
	pin->irq_cb      = NULL;
	pin->irq_user    = NULL;
	pthread_mutex_unlock(&g_irq.mu);
	irq_wake();

	/* Best-effort: reconfigure back to plain input so subsequent
     * read()s don't fight buffered edge events.  Failures here are
     * non-fatal -- the dispatcher already won't dispatch to this
     * pin. */
	struct gpio_v2_line_config cfg = { 0 };
	cfg.flags                      = GPIO_V2_LINE_FLAG_INPUT;
	(void)ioctl(pin->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg);
	return ALP_OK;
}

void alp_gpio_close(alp_gpio_t *pin)
{
	if (pin == NULL) {
		return;
	}
	/* Detach from the dispatcher before releasing the fd so the
     * dispatcher's next snapshot pass observes the cleared slot
     * before any subsequent reuse of the pool entry. */
	if (pin->in_use && pin->irq_enabled) {
		pthread_mutex_lock(&g_irq.mu);
		pin->irq_enabled = false;
		pin->irq_cb      = NULL;
		pin->irq_user    = NULL;
		pthread_mutex_unlock(&g_irq.mu);
		irq_wake();
	}
	pool_release(pin);
}

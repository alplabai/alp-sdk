/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N supervisor singleton -- see v2n_supervisor.h for the contract.
 *
 * Compiled in only when CONFIG_ALP_SDK_V2N_SUPERVISOR=y.  The
 * acquire / release helpers are still declared on every build (the
 * !V2N stub at the bottom returns NOSUPPORT) so peripheral backends
 * can dispatch unconditionally.
 *
 * Concurrency model:
 *   * `g_v2n.lock` serialises both the (rare) lazy-init sequence and
 *     each (frequent) bridge op.  A single mutex is enough -- bridge
 *     ops are short (~1 ms typical for an SPI ping, ~5 ms for an
 *     I2C exchange) and steady-state contention between callers is
 *     bounded by the bridge's own one-op-at-a-time discipline.
 *   * Callers MUST pass a non-zero acquire timeout via Kconfig so a
 *     thread waiting on a hung first-init doesn't pile up forever.
 *   * Latching policy: `tried_init` is set to `true` only when init
 *     completed (success) OR when no bus is configured at compile
 *     time (the failure mode that can't recover by retrying).
 *     Transient runtime failures (alp_spi_open hiccup, gd32g553_init
 *     handshake timeout) keep `tried_init = false`, so the next
 *     acquire retries the bus open + handshake.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)

#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID (-1)
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID (-1)
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR GD32G553_BRIDGE_DEFAULT_I2C_ADDR
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ 10000000
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ 400000
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS 100
#endif

#define V2N_BOTH_BUSES_DISABLED \
	((CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID < 0) && \
	 (CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID < 0))

static struct {
	bool           tried_init;
	alp_status_t   init_status;
	alp_spi_t     *spi;
	alp_i2c_t     *i2c;
	gd32g553_t     ctx;
	struct k_mutex lock;
} g_v2n;

static int v2n_supervisor_sys_init(void)
{
	k_mutex_init(&g_v2n.lock);
	return 0;
}
SYS_INIT(v2n_supervisor_sys_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* Runs under g_v2n.lock.  On the first call, opens the configured
 * buses and runs the GD32 handshake; on subsequent calls either
 * short-circuits with the cached success (`tried_init` latched) or
 * retries the open/handshake (transient failure path). */
static alp_status_t try_init_locked(void)
{
	if (g_v2n.tried_init) return g_v2n.init_status;
	g_v2n.init_status = ALP_ERR_NOT_READY;

#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID >= 0)
	if (g_v2n.spi == NULL) {
		g_v2n.spi = alp_spi_open(&(alp_spi_config_t){
		    .bus_id        = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID,
		    .freq_hz       = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ,
		    .mode          = ALP_SPI_MODE_0,
		    .bits_per_word = 8u,
		    /* CS routing comes from the board's spi-controller DT
             * node (`cs-gpios` on the SPI bus + DT alias on the
             * supervisor chip-select).  alp_spi_open() resolves it
             * via the standard Zephyr device path. */
		});
	}
	/* A failed SPI open is non-fatal -- the I2C management path may
     * still come up.  gd32g553_init() requires only one of the two
     * transports. */
#endif

#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID >= 0)
	if (g_v2n.i2c == NULL) {
		g_v2n.i2c = alp_i2c_open(&(alp_i2c_config_t){
		    .bus_id     = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID,
		    .bitrate_hz = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ,
		});
	}
#endif

	if (g_v2n.spi == NULL && g_v2n.i2c == NULL) {
		/* Neither bus opened.  Two sub-cases:
         *   - Both Kconfig bus IDs are -1: this build genuinely
         *     can't talk to a GD32; latch tried_init so future
         *     acquires return NOT_READY without hitting the open
         *     attempts above.
         *   - At least one bus ID is set but alp_*_open() failed:
         *     transient (DT not yet probed, controller late).
         *     Leave tried_init=false; the next acquire retries. */
		g_v2n.init_status = ALP_ERR_NOT_READY;
		if (V2N_BOTH_BUSES_DISABLED) g_v2n.tried_init = true;
		return g_v2n.init_status;
	}

	const alp_status_t s = gd32g553_init(
	    &g_v2n.ctx, g_v2n.spi, g_v2n.i2c, (uint8_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR);
	if (s != ALP_OK) {
		/* Tear the bus handles back down -- a failed handshake means
         * we won't issue further bridge calls, and leaving the buses
         * open would pin pool slots that other code could use.
         * Don't latch tried_init: the GD32 may come up after a power
         * cycle / fresh-flash and a future acquire should pick it up. */
		if (g_v2n.spi != NULL) {
			alp_spi_close(g_v2n.spi);
			g_v2n.spi = NULL;
		}
		if (g_v2n.i2c != NULL) {
			alp_i2c_close(g_v2n.i2c);
			g_v2n.i2c = NULL;
		}
		g_v2n.init_status = s;
		return s;
	}

	g_v2n.init_status = ALP_OK;
	g_v2n.tried_init  = true; /* latch the success */
	return ALP_OK;
}

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
	if (ctx_out == NULL) return ALP_ERR_INVAL;
	*ctx_out = NULL;

	/* Bounded wait: a thread stuck inside gd32g553_init() against a
     * hung GD32 holds the mutex for its entire blocking window; new
     * acquirers must not pile up indefinitely behind it. */
	const int locked =
	    k_mutex_lock(&g_v2n.lock, K_MSEC(CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS));
	if (locked != 0) return ALP_ERR_BUSY;

	const alp_status_t s = try_init_locked();
	if (s != ALP_OK) {
		k_mutex_unlock(&g_v2n.lock);
		return s;
	}
	*ctx_out = &g_v2n.ctx;
	return ALP_OK;
}

void alp_z_v2n_supervisor_release(void)
{
	/* Pairs with a successful acquire.  Releases are no-ops on
     * builds where the supervisor isn't compiled in (see the !V2N
     * stub below); the dispatcher branches that call release-without-
     * a-prior-acquire (the unconditional release on the !V2N path)
     * stay safe. */
	k_mutex_unlock(&g_v2n.lock);
}

alp_status_t alp_z_v2n_supervisor_brd_i2c_acquire(alp_i2c_t **i2c_out)
{
	if (i2c_out == NULL) return ALP_ERR_INVAL;
	*i2c_out = NULL;

#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID < 0)
	/* No I²C bus configured for the supervisor build -- DA9292
     * driver + any other BRD_I²C consumer has no handle to borrow.
     * Surface NOSUPPORT so callers compile against this helper
     * unconditionally + branch on the runtime result. */
	return ALP_ERR_NOSUPPORT;
#else
	const int locked =
	    k_mutex_lock(&g_v2n.lock, K_MSEC(CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS));
	if (locked != 0) return ALP_ERR_BUSY;

	/* Lazy-init drives the same bus-open path as the GD32 acquire
     * helper -- this is fine because the BRD_I²C handle is exactly
     * the one the supervisor opened.  Init failure tears the locks
     * back down before returning. */
	const alp_status_t s = try_init_locked();
	if (s != ALP_OK) {
		k_mutex_unlock(&g_v2n.lock);
		return s;
	}
	if (g_v2n.i2c == NULL) {
		/* try_init_locked() returned OK but no I²C handle -- the
         * SPI path satisfied gd32g553_init() on this build.  BRD_I²C
         * consumers genuinely can't proceed; release + report. */
		k_mutex_unlock(&g_v2n.lock);
		return ALP_ERR_NOT_READY;
	}
	*i2c_out = g_v2n.i2c;
	return ALP_OK;
#endif
}

void alp_z_v2n_supervisor_brd_i2c_release(void)
{
#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID < 0)
	/* No mutex was taken on the acquire path -- nothing to do. */
#else
	k_mutex_unlock(&g_v2n.lock);
#endif
}

void alp_z_v2n_supervisor_invalidate(void)
{
	/* Take the mutex for the duration of the bus teardown so a
     * concurrent acquire() can't re-init mid-close.  Bounded wait
     * matches the regular acquire timeout to avoid piling up
     * behind a hung sleep handler. */
	const int locked =
	    k_mutex_lock(&g_v2n.lock, K_MSEC(CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS));
	if (locked != 0) {
		/* Couldn't take the lock; another thread is mid-bridge-op.
         * The invalidate is best-effort -- the in-flight thread's
         * call may fail naturally if the GD32 is unresponsive
         * post-wake, and the failure path will leave tried_init
         * clear so the next caller re-inits.  No useful action
         * here besides giving up. */
		return;
	}
	if (g_v2n.spi != NULL) {
		alp_spi_close(g_v2n.spi);
		g_v2n.spi = NULL;
	}
	if (g_v2n.i2c != NULL) {
		alp_i2c_close(g_v2n.i2c);
		g_v2n.i2c = NULL;
	}
	g_v2n.tried_init  = false;
	g_v2n.init_status = ALP_ERR_NOT_READY;
	/* Don't touch g_v2n.ctx -- gd32g553_init() will overwrite it
     * on the next acquire.  Leaving the prior struct contents
     * around is harmless because tried_init=false makes any read
     * of it unreachable. */
	k_mutex_unlock(&g_v2n.lock);
}

#else /* !CONFIG_ALP_SDK_V2N_SUPERVISOR -- stubs let backends compile unconditionally. */

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
	if (ctx_out != NULL) *ctx_out = NULL;
	return ALP_ERR_NOSUPPORT;
}

void alp_z_v2n_supervisor_release(void)
{
	/* Nothing to release. */
}

void alp_z_v2n_supervisor_invalidate(void)
{
	/* Nothing to invalidate -- no supervisor compiled in. */
}

alp_status_t alp_z_v2n_supervisor_brd_i2c_acquire(alp_i2c_t **i2c_out)
{
	if (i2c_out != NULL) *i2c_out = NULL;
	return ALP_ERR_NOSUPPORT;
}

void alp_z_v2n_supervisor_brd_i2c_release(void)
{
	/* Nothing to release. */
}

#endif /* CONFIG_ALP_SDK_V2N_SUPERVISOR */

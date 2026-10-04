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
 * SPI-only: RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive (maintainer
 * decision, metadata/e1m_modules/v2n/core-ownership.yaml) -- the CM33
 * never masters it, so this singleton has no I2C transport branch.
 *
 * Concurrency model:
 *   * `g_v2n.lock` serialises both the (rare) lazy-init sequence and
 *     each (frequent) bridge op.  A single mutex is enough -- bridge
 *     ops are short (~1 ms typical for an SPI ping) and steady-state
 *     contention between callers is bounded by the bridge's own
 *     one-op-at-a-time discipline.
 *   * Callers MUST pass a non-zero acquire timeout via Kconfig so a
 *     thread waiting on a hung first-init doesn't pile up forever.
 *   * Latching policy: `tried_init` is set to `true` only when init
 *     completed (success) OR when no bus is configured at compile
 *     time (the failure mode that can't recover by retrying).
 *     Transient runtime failures (alp_spi_open hiccup, gd32g553_init
 *     handshake timeout) keep `tried_init = false`, so the next
 *     acquire retries the bus open + handshake.  An init that ends
 *     ALP_ERR_BUSY (bridge alive, mid-OTA-trial) arms a hold-off
 *     (CONFIG_ALP_SDK_V2N_SUPERVISOR_BUSY_HOLDOFF_MS) during which
 *     acquirers get ALP_ERR_BUSY without re-running the ~2 s init.
 *
 * ATTN (GD32 bridge protocol v0.15, docs/gd32-bridge-protocol.md §3.17):
 *   The GD32 drives PA14 -- Renesas P71, the `attn` pad of the board's
 *   dedicated `alp,gd32-pads` devicetree node, opened as
 *   GD32G553_PAD_ID_ATTN -- HIGH when a reply is armed.  EVERY init
 *   (re)configures P71 as an INPUT with a rising-edge IRQ before the
 *   handshake (host rules H1/H2) -- never trusting an earlier setup, an SWD
 *   session in between may have turned the pad into an output -- and
 *   registers a time-stamping edge hook (the ISR records the cycle counter
 *   and gives a semaphore; the hook takes it: no polling), so the chip driver
 *   can request the ATTN link feature and discard edges older than a request.
 *   P71 is NEVER driven as an output here.  When the pad is not published, or
 *   the platform GPIO driver has no interrupt for it, the hook is simply
 *   absent and the link keeps the 0.14 staging gap + re-read ladder.
 *
 * SWD exclusion:
 *   P71 is also the GD32's SWCLK, which chips/gd32_swd drives during a
 *   recovery session.  gd32_swd_session_notify() (strong override below)
 *   closes the bridge link and disables the ATTN IRQ when a session starts,
 *   and until the session ends every acquire() -- and so every bridge
 *   command -- answers ALP_ERR_BUSY and nothing re-initialises or
 *   renegotiates the link.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)

#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID (-1)
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_BUSY_HOLDOFF_MS
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_BUSY_HOLDOFF_MS 1000
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ 10000000
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS 100
#endif

/* GD32 from CM33 is SPI-only: RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
 * (metadata/e1m_modules/v2n/core-ownership.yaml) -- the CM33 must never
 * master it, so there is no I2C transport branch here at all. */
#define V2N_SPI_BUS_DISABLED (CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID < 0)

#include "alp/chips/gd32_swd.h"        /* gd32_swd_session_notify() */
#include "../backends/gpio/gpio_ops.h" /* alp_z_gpio_open_internal() */

static struct {
	bool           tried_init;
	alp_status_t   init_status;
	int64_t        busy_until_ms; /* hold-off after an init that saw BUSY */
	alp_spi_t     *spi;
	gd32g553_t     ctx;
	struct k_mutex lock;
	/* ATTN (P71) input: the handle is kept, but the pad is re-configured
	 * (input + rising-edge IRQ) on EVERY init. */
	alp_gpio_t       *attn_pin;
	struct k_sem      attn_sem;
	volatile uint32_t attn_edge_t;  /* cycle counter at the last edge (ISR-written) */
	atomic_t          swd_session;  /* an SWD session owns P70/P71/P74 */
	bool              force_reinit; /* a session start could not close the link */
} g_v2n;

/* ---- ATTN hook (edge latch = semaphore) ------------------------------- */

static void attn_isr(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
	g_v2n.attn_edge_t = k_cycle_get_32(); /* stamp first, then wake the waiter */
	k_sem_give(&g_v2n.attn_sem);          /* ISR-safe */
}

/* The clock edges are stamped on; the chip driver compares the two. */
static uint32_t attn_now(void *user)
{
	(void)user;
	return k_cycle_get_32();
}

/* Block (IRQ + semaphore, never polling) until a rising edge was latched, and
 * report its time-stamp so the driver can tell a stale edge from the reply's. */
static alp_status_t attn_wait(void *user, uint32_t timeout_ms, uint32_t *t_edge)
{
	(void)user;
	if (k_sem_take(&g_v2n.attn_sem, K_MSEC(timeout_ms)) != 0) return ALP_ERR_TIMEOUT;
	*t_edge = g_v2n.attn_edge_t;
	return ALP_OK;
}

static alp_status_t attn_read_level(void *user, bool *high)
{
	(void)user;
	return alp_gpio_read(g_v2n.attn_pin, high);
}

/* Discard every latched edge: called right before the driver reads the clock
 * for a request, so no old stamp can alias a fresh one after a counter wrap. */
static void attn_drain(void *user)
{
	(void)user;
	k_sem_reset(&g_v2n.attn_sem);
}

static const gd32g553_attn_hook_t g_attn_hook = {
	.now        = attn_now,
	.wait       = attn_wait,
	.read_level = attn_read_level,
	.drain      = attn_drain,
};

/* Rules H1/H2: P71 is an input with a rising-edge IRQ before ATTN is ever
 * requested.  Run on EVERY init: an SWD session may have driven the pad as an
 * output since the last one.  Best effort -- any failure leaves the hook
 * absent for this init. */
static bool attn_setup(void)
{
	if (!IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR_ATTN)) return false;
	if (g_v2n.attn_pin == NULL) {
		/* internal opener: the portable alp_gpio_open() refuses the GD32 pad ids */
		g_v2n.attn_pin = alp_z_gpio_open_internal(GD32G553_PAD_ID_ATTN);
		if (g_v2n.attn_pin == NULL) return false; /* board publishes no ATTN pad */
	}
	(void)alp_gpio_irq_disable(g_v2n.attn_pin);
	k_sem_reset(&g_v2n.attn_sem);
	if (alp_gpio_configure(g_v2n.attn_pin, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE) != ALP_OK ||
	    alp_gpio_irq_enable(g_v2n.attn_pin, ALP_GPIO_EDGE_RISING, attn_isr, NULL) != ALP_OK) {
		/* No interrupt for this pad (the platform GPIO driver has no ICU
		 * TINT route): ATTN stays off. */
		alp_gpio_close(g_v2n.attn_pin);
		g_v2n.attn_pin = NULL;
		return false;
	}
	return true;
}

static int v2n_supervisor_sys_init(void)
{
	k_mutex_init(&g_v2n.lock);
	k_sem_init(&g_v2n.attn_sem, 0, 1); /* before any ATTN IRQ can fire */
	return 0;
}
SYS_INIT(v2n_supervisor_sys_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* Runs under g_v2n.lock.  On the first call, opens the configured
 * buses and runs the GD32 handshake; on subsequent calls either
 * short-circuits with the cached success (`tried_init` latched) or
 * retries the open/handshake (transient failure path). */
static alp_status_t try_init_locked(void)
{
	if (atomic_get(&g_v2n.swd_session) != 0) return ALP_ERR_BUSY; /* SWD owns P71 */
	if (g_v2n.force_reinit) {
		/* An SWD session started while a bridge command held the lock, so the
		 * link could not be closed then.  The session may have reset the GD32 and
		 * left P71 an output: drop the stale link and re-run the full init
		 * (handshake, negotiation and attn_setup()) now. */
		if (g_v2n.spi != NULL) {
			alp_spi_close(g_v2n.spi);
			g_v2n.spi = NULL;
		}
		g_v2n.tried_init   = false;
		g_v2n.init_status  = ALP_ERR_NOT_READY;
		g_v2n.force_reinit = false;
	}
	if (g_v2n.tried_init) return g_v2n.init_status;
	/* A bridge that answered BUSY is alive but mid-OTA-trial; each
	 * gd32g553_init() against it spends its whole ~2 s retry budget
	 * under g_v2n.lock.  Answer BUSY straight away until the hold-off
	 * expires instead of making every acquirer pay that again. */
	if (k_uptime_get() < g_v2n.busy_until_ms) return ALP_ERR_BUSY;
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
	/* GD32 from CM33 is SPI-only (see V2N_SPI_BUS_DISABLED above) --
     * no I2C open branch here. */
#endif

	if (g_v2n.spi == NULL) {
		/* Bus not opened.  Two sub-cases:
         *   - The Kconfig bus ID is -1: this build genuinely can't
         *     talk to a GD32; latch tried_init so future acquires
         *     return NOT_READY without hitting the open attempt above.
         *   - The bus ID is set but alp_spi_open() failed: transient
         *     (DT not yet probed, controller late).  Leave
         *     tried_init=false; the next acquire retries. */
		g_v2n.init_status = ALP_ERR_NOT_READY;
		if (V2N_SPI_BUS_DISABLED) g_v2n.tried_init = true;
		return g_v2n.init_status;
	}

	const alp_status_t s = gd32g553_init_ex(&g_v2n.ctx,
	                                        g_v2n.spi,
	                                        NULL,
	                                        GD32G553_BRIDGE_DEFAULT_I2C_ADDR,
	                                        attn_setup() ? &g_attn_hook : NULL);
	if (s != ALP_OK) {
		/* Tear the bus handle back down -- a failed handshake means
         * we won't issue further bridge calls, and leaving the bus
         * open would pin a pool slot other code could use.  Don't
         * latch tried_init: the GD32 may come up after a power cycle /
         * fresh-flash and a future acquire should pick it up. */
		if (g_v2n.spi != NULL) {
			alp_spi_close(g_v2n.spi);
			g_v2n.spi = NULL;
		}
		g_v2n.init_status = s;
		if (s == ALP_ERR_BUSY) {
			g_v2n.busy_until_ms = k_uptime_get() + CONFIG_ALP_SDK_V2N_SUPERVISOR_BUSY_HOLDOFF_MS;
		}
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
	/* An SWD session owns the GD32 pads: no bridge command may run. */
	if (atomic_get(&g_v2n.swd_session) != 0) return ALP_ERR_BUSY;

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
	g_v2n.tried_init  = false;
	g_v2n.init_status = ALP_ERR_NOT_READY;
	/* Don't touch g_v2n.ctx -- gd32g553_init() will overwrite it
     * on the next acquire.  Leaving the prior struct contents
     * around is harmless because tried_init=false makes any read
     * of it unreachable. */
	k_mutex_unlock(&g_v2n.lock);
}

/* Strong override of chips/gd32_swd's weak hook.  An SWD session starts: block
 * every new bridge command (acquire() answers BUSY), stop ATTN interrupts (the
 * pad is about to become the SWD clock output, which would fire the rising-edge
 * IRQ at SWCLK rate), and close the SPI link so nothing is left half-open and
 * the next init renegotiates from scratch.  The session ends: allow init
 * again; the next acquire() re-opens the bus and re-runs the handshake, whose
 * attn_setup() puts P71 back to input + IRQ. */
void gd32_swd_session_notify(bool active)
{
	if (!active) {
		atomic_set(&g_v2n.swd_session, 0);
		return;
	}
	atomic_set(&g_v2n.swd_session, 1); /* new acquires now fail first */
	if (g_v2n.attn_pin != NULL) (void)alp_gpio_irq_disable(g_v2n.attn_pin);
	if (k_mutex_lock(&g_v2n.lock, K_MSEC(CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS)) == 0) {
		if (g_v2n.spi != NULL) {
			alp_spi_close(g_v2n.spi);
			g_v2n.spi = NULL;
		}
		g_v2n.tried_init  = false;
		g_v2n.init_status = ALP_ERR_NOT_READY;
		k_mutex_unlock(&g_v2n.lock);
	} else {
		/* Could not take the lock (a bridge command is in flight): remember
		 * that the link is stale so the next use after the session re-inits. */
		g_v2n.force_reinit = true;
	}
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

#endif /* CONFIG_ALP_SDK_V2N_SUPERVISOR */

/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion` -- one portable command surface over two different
 * companions: the GD32 supervisor singleton on V2N, an app-registered
 * CC3501E on Alif.  ver/ping are portable; gpio is V2N-only.
 *
 * Core TU (#673 Phase 2 split): the companion context
 * (companion_cc3501e / alp_console_companion_set), the Alif bridge-bus
 * mutex, the async event-delivery plumbing, and the ver / ping / reset /
 * bench commands.  The remaining command groups register themselves
 * from their own TUs onto the (alp, companion) dynamic subcommand set
 * declared here: alp_console_companion_wifi.c, _ble.c, _diag.c, _ota.c,
 * _sock.c (Alif CC3501E) and alp_console_companion_gpio.c (V2N GD32).
 * Shared cross-TU state lives in alp_console_companion_internal.h.
 */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/console.h>
#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"
#include "alp_console_companion_internal.h"

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#include "../v2n_supervisor.h"
#endif

/* ---- Alif: app-registered CC3501E handle -------------------------------- */
cc3501e_t *companion_cc3501e;

void alp_console_companion_set(cc3501e_t *ctx)
{
	companion_cc3501e = ctx;
}

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* Bridge-bus serialisation (Alif CC3501E).  The shell thread (companion command
 * bodies) and the async-connect result thread BOTH drive the inter-chip bridge,
 * and cc3501e_request is not internally locked -- two concurrent transactions
 * would interleave on the SPI bus and desync the link.  Every bridge
 * access from this split (each command body + the result thread's status poll)
 * takes this mutex. */
K_MUTEX_DEFINE(companion_bus_lock);
#endif

static int cmd_companion_ver(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}

	gd32g553_version_t v;

	s = gd32g553_get_version(ctx, &v);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "GD32 supervisor fw v%u.%u.%u", v.major, v.minor, v.patch);
	return 0;
#else
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered (call alp_console_companion_set)");
		return -ENODEV;
	}

	uint16_t ver = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_get_version(companion_cc3501e, &ver);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "CC3501E protocol v%u", (unsigned int)ver);
	return 0;
#endif
}

static int cmd_companion_ping(const struct shell *sh, size_t argc, char **argv)
{
#if IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gd32g553_t  *ctx;
	alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);

	if (s != ALP_OK) {
		shell_error(sh, "supervisor acquire failed (%d)", (int)s);
		return -EIO;
	}
	s = gd32g553_ping(ctx);
	alp_z_v2n_supervisor_release();
	shell_print(sh, "ping %s", s == ALP_OK ? "OK" : "FAIL");
	return s == ALP_OK ? 0 : -EIO;
#else
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered (call alp_console_companion_set)");
		return -ENODEV;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_ping(companion_cc3501e);
	k_mutex_unlock(&companion_bus_lock);
	shell_print(sh, "ping %s", s == ALP_OK ? "OK" : "FAIL");
	return s == ALP_OK ? 0 : -EIO;
#endif
}

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- async EVT_* delivery (task #17): host-polled event queue ------------ *
 * The CC3501E firmware queues async events (Wi-Fi connect/disconnect, ...) that
 * it CANNOT push -- the CC35 GPIO17 -> Alif P2_6 attention line is a bodge not
 * routed on the stock EVK.  So the PRIMARY, benchable mechanism is this low-rate
 * poll: a background thread calls cc3501e_poll_events() every ~500 ms, which
 * drains the firmware ring (CMD_GET_PENDING_EVENTS) and invokes the callback
 * below once per queued event -- printing it so the bench can SEE an async event
 * (connect / disconnect the Wi-Fi and watch "[event] wifi ..." appear).  The
 * opt-in interrupt path (CONFIG_ALP_SDK_CC3501E_EVENT_IRQ, below) drives the
 * SAME drain from a P2_6 edge instead, for a bodged unit. */
#define ALP_COMPANION_EVENT_POLL_MS 500

/* Runs on the driver's RX/poll context (here: the event-poll thread or the IRQ
 * workqueue) while the bridge bus lock is held.  Print-only -- printk goes to
 * the active console backend, so it is safe off the shell thread. */
static void companion_event_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	ARG_UNUSED(payload);
	ARG_UNUSED(user);
	switch (opcode) {
	case ALP_CC3501E_EVT_WIFI_CONNECTED:
		printk("[event] wifi connected\n");
		break;
	case ALP_CC3501E_EVT_WIFI_DISCONNECTED:
		printk("[event] wifi disconnected\n");
		break;
	case ALP_CC3501E_EVT_BLE_CONNECTED:
		printk("[event] ble connected\n");
		break;
	case ALP_CC3501E_EVT_BLE_DISCONNECTED:
		printk("[event] ble disconnected\n");
		break;
	case ALP_CC3501E_EVT_GPIO_INTERRUPT:
		printk("[event] gpio interrupt\n");
		break;
	default:
		printk("[event] opcode 0x%02x (len %u)\n", opcode, (unsigned int)len);
		break;
	}
}

/* Drain + dispatch pending events under the bridge bus lock.  Shared by the
 * timer-poll thread and the opt-in IRQ workqueue.  No-op until a companion is
 * registered (the cb is attached lazily on first poll). */
static bool companion_event_cb_set;

static void companion_drain_events(void)
{
	if (companion_cc3501e == NULL) {
		return;
	}
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	if (!companion_event_cb_set) {
		(void)cc3501e_set_event_callback(companion_cc3501e, companion_event_cb, NULL);
		companion_event_cb_set = true;
	}
	(void)cc3501e_poll_events(companion_cc3501e);
	k_mutex_unlock(&companion_bus_lock);
}

static void companion_event_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_msleep(ALP_COMPANION_EVENT_POLL_MS);
		companion_drain_events();
	}
}

/* Low priority (7), like the connect worker: the shell stays above it.  Costs a
 * light GET_PENDING_EVENTS round-trip every 500 ms once a companion is
 * registered, and nothing (a sleep) before that. */
K_THREAD_DEFINE(companion_event_tid, 1024, companion_event_thread, NULL, NULL, NULL, 7, 0, 0);

#if IS_ENABLED(CONFIG_ALP_SDK_CC3501E_EVENT_IRQ)
/* ---- opt-in interrupt path (bodged unit only) --------------------------- *
 * HW-GATED + default-off.  Needs the CC35 GPIO17 -> Alif P2_6 attention wire
 * (a bodge, absent on the stock EVK) AND CC35 firmware driving GPIO17 when an
 * event is pending; a board with the bodge provides a `cc3501e-attn` DT alias
 * pointing at P2_6 (gpio2 pin 6).  On each edge the ISR schedules a workqueue
 * item (NOT the drain itself -- cc3501e_poll_events does SPI I/O and takes a
 * mutex, neither ISR-safe) that runs the same companion_drain_events() as the
 * timer poll.  This coexists with the timer poll (which stays the default,
 * benchable path); the edge just makes delivery immediate on a bodged unit. */
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec companion_attn = GPIO_DT_SPEC_GET(DT_ALIAS(cc3501e_attn), gpios);
static struct gpio_callback      companion_attn_cb_data;

static void companion_event_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	companion_drain_events();
}
static K_WORK_DEFINE(companion_event_work, companion_event_work_fn);

static void companion_attn_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Defer the SPI drain to a workqueue -- never do bridge I/O in the ISR. */
	k_work_submit(&companion_event_work);
}

static int companion_event_irq_init(void)
{
	if (!gpio_is_ready_dt(&companion_attn)) {
		return -ENODEV;
	}
	int rc = gpio_pin_configure_dt(&companion_attn, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}
	/* Edge-triggered: the CC35 pulses GPIO17 when an event is pending.  ACTIVE
	 * covers whichever polarity the bodge/overlay declares in the DT flags. */
	rc = gpio_pin_interrupt_configure_dt(&companion_attn, GPIO_INT_EDGE_TO_ACTIVE);
	if (rc != 0) {
		return rc;
	}
	gpio_init_callback(&companion_attn_cb_data, companion_attn_isr, BIT(companion_attn.pin));
	return gpio_add_callback(companion_attn.port, &companion_attn_cb_data);
}
SYS_INIT(companion_event_irq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_ALP_SDK_CC3501E_EVENT_IRQ */

/* ---- bridge throughput bench ------------------------------------------- */
/* NOTE: the old `settle` tuning command was removed with the fixed req/reply
 * settle gaps -- the current cc3501e driver rendezvouses on the READY line
 * (see cc3501e.c / the `ready` gpio) instead of a tunable fixed delay, so
 * there is no longer a settle knob to get/set. */
static int cmd_companion_bench(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long n = 200u;
	if (argc >= 2 && (alp_console_parse_ulong(argv[1], &n) != 0 || n == 0u || n > 100000u)) {
		shell_error(sh, "usage: alp companion bench [1..100000 ops]");
		return -EINVAL;
	}
	uint16_t     ver   = 0;
	unsigned int fails = 0;
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	int64_t t0 = k_uptime_get();
	for (unsigned long i = 0; i < n; i++) {
		if (cc3501e_get_version(companion_cc3501e, &ver) != ALP_OK) {
			fails++;
		}
	}
	int64_t dt = k_uptime_get() - t0;
	k_mutex_unlock(&companion_bus_lock);
	if (dt <= 0) {
		dt = 1;
	}
	shell_print(sh,
	            "bench: %lu GET_VERSION ops in %lld ms = %lld us/op, %lld ops/s (fails=%u)",
	            n,
	            (long long)dt,
	            (long long)((dt * 1000) / (long long)n),
	            (long long)(((long long)n * 1000) / dt),
	            fails);
	return 0;
}

/* ---- CC3501E soft reset (Alif companion) -------------------------------- */
static int cmd_companion_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	/* Soft reset: the firmware acks then DEFERRED-reboots.  The bridge link drops
	 * afterwards -- the app must cc3501e_reset()/sync + re-register to talk again. */
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	alp_status_t s = cc3501e_soft_reset(companion_cc3501e);
	k_mutex_unlock(&companion_bus_lock);

	if (s != ALP_OK) {
		shell_error(sh, "soft reset failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "soft reset requested -- firmware reboots; link will drop");
	return 0;
}
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */

/* `alp companion` itself: a decentralized dynamic subcommand set (Zephyr's
 * SHELL_SUBCMD_SET_CREATE / SHELL_SUBCMD_ADD section mechanism), so each
 * command-group TU in this split adds its own group without this core file
 * knowing about it.  ver/ping/reset/bench are the only leaf commands the
 * core registers directly; wifi/ble/diag/ota/sock (Alif) and gpio (V2N)
 * register themselves from their own TUs. */
SHELL_SUBCMD_SET_CREATE(alp_companion_subcmds, (alp, companion));

SHELL_SUBCMD_ADD((alp, companion),
                 ver,
                 NULL,
                 "companion firmware version",
                 cmd_companion_ver,
                 1,
                 0);
SHELL_SUBCMD_ADD((alp, companion), ping, NULL, "liveness round-trip", cmd_companion_ping, 1, 0);
#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
SHELL_SUBCMD_ADD((alp, companion),
                 reset,
                 NULL,
                 "soft-reset the CC3501E firmware (in-band; link drops)",
                 cmd_companion_reset,
                 1,
                 0);
SHELL_SUBCMD_ADD((alp, companion),
                 bench,
                 NULL,
                 "bench [n] -- time n GET_VERSION round-trips",
                 cmd_companion_bench,
                 1,
                 1);
#endif

SHELL_SUBCMD_ADD((alp),
                 companion,
                 &alp_companion_subcmds,
                 "Companion chip bridge (GD32 / CC3501E)",
                 NULL,
                 1,
                 0);

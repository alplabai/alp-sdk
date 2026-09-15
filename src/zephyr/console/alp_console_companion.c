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

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* Shared by `alp companion linklog` and the recovery callback below (issue
 * #2136): dump ctx->link_log oldest-first, one hex line per entry. @p sh
 * NULL means "print via printk instead of a shell" -- the recovery callback
 * has no shell handle, only whatever console backend printk currently
 * targets, same as its own recovery-line print just below. */
/* No fixed-size local buffer for these -- printed directly through shell_print
 * / printk instead of snprintf'd first, so there is no truncation budget to
 * mis-size against the legend text below (-Werror=format-truncation). */
#define COMPANION_LINK_LOG_HDR_FMT \
	"cc3501e: link_log %u/%u entries, fail_streak=%u (legend: ts_ms cmd phase status" \
	" flags hdr[4] reply_hdr[4] recover_attempts; phase 1=req_hdr 2=req_payload" \
	" 3=reply_hdr 4=reply_payload; flags 1=ready_before 2=ready_after 4=ready_proven" \
	" 8=hdr_valid 0x10=reply_hdr_valid -- hdr[4]/reply_hdr[4] are MEANINGLESS," \
	" all-zero, unless their own VALID bit is set)"
/* #2136 review (minor): the "cc3501e: " prefix now lives IN the format string
 * itself, not concatenated separately at each call site -- the printk call
 * below used to add it and the shell_print call did not, so a grep/parser
 * written against a printk (recovery-callback) capture silently matched
 * nothing on an `alp companion linklog` (shell) capture. Both paths use this
 * SAME format now, so both agree. */
#define COMPANION_LINK_LOG_LINE_FMT \
	"cc3501e: %08x %02x %u %d %02x %02x%02x%02x%02x %02x%02x%02x%02x %u"

/* @p fail_streak is the caller's choice of WHICH streak to show: the live
 * value (cc3501e_link_log_fail_streak(), for `alp companion linklog`) or the
 * one frozen at the start of the most recent recovery
 * (cc3501e_link_log_recover_streak(), for the recovery-notify dump below) --
 * see cc3501e_link_log_recover_streak()'s own doc comment for why those
 * differ (#2136 review, minor: the live field always read 0 by the time a
 * recovery dump ran, because that recovery's OWN confirming PING had already
 * reset it). */
static void companion_print_link_log(const struct shell *sh, uint32_t fail_streak)
{
	if (companion_cc3501e == NULL) return;

	const uint8_t n = cc3501e_link_log_count(companion_cc3501e);

	if (sh != NULL) {
		shell_print(sh, COMPANION_LINK_LOG_HDR_FMT, n, CC3501E_LINK_LOG_LEN, fail_streak);
	} else {
		printk(COMPANION_LINK_LOG_HDR_FMT "\n", n, CC3501E_LINK_LOG_LEN, fail_streak);
	}

	for (uint8_t i = 0; i < n; i++) {
		cc3501e_link_log_entry_t e;

		if (cc3501e_link_log_get(companion_cc3501e, i, &e) != ALP_OK) break;
		if (sh != NULL) {
			shell_print(sh,
			            COMPANION_LINK_LOG_LINE_FMT,
			            e.ts_ms,
			            e.cmd,
			            e.phase,
			            (int)e.status,
			            e.flags,
			            e.hdr_bytes[0],
			            e.hdr_bytes[1],
			            e.hdr_bytes[2],
			            e.hdr_bytes[3],
			            e.reply_hdr[0],
			            e.reply_hdr[1],
			            e.reply_hdr[2],
			            e.reply_hdr[3],
			            e.recover_attempt_count);
		} else {
			printk(COMPANION_LINK_LOG_LINE_FMT "\n",
			       e.ts_ms,
			       e.cmd,
			       e.phase,
			       (int)e.status,
			       e.flags,
			       e.hdr_bytes[0],
			       e.hdr_bytes[1],
			       e.hdr_bytes[2],
			       e.hdr_bytes[3],
			       e.reply_hdr[0],
			       e.reply_hdr[1],
			       e.reply_hdr[2],
			       e.reply_hdr[3],
			       e.recover_attempt_count);
		}
	}
}

/* Registered via cc3501e_set_recover_callback() below (issue #2126): announce
 * an AUTOMATIC recovery -- cc3501e_link_check_and_recover(), wired into the
 * driver's own failure exits (cc3501e_core.c / cc3501e_wifi.c) -- the moment
 * it happens. printk, same as the async event callback below: goes to the
 * active console backend, safe off whatever thread the failing op was
 * running on. Runs AFTER the recovery is already committed (ctx->
 * recover_count already bumped), so @p recover_count here is exactly what
 * `alp companion recover` would print too. A registered, per-ctx callback,
 * the same pattern this driver already uses for async events
 * (cc3501e_add_event_callback).
 *
 * ONE slot: this registration happens in alp_console_companion_set(), so an
 * application that wants its own recovery callback must register it AFTER
 * binding the console -- the later registration wins and this line stops
 * printing. */
static void companion_recover_notify(cc3501e_t *ctx, uint32_t recover_count, void *user)
{
	ARG_UNUSED(user);
	/* Issue #2136: dump the ring right before the recovery line, no extra
	 * bench step needed to capture the state around a reset -- see
	 * companion_print_link_log()'s doc comment. cc3501e_link_log_recover_streak(),
	 * not the live cc3501e_link_log_fail_streak(): this recovery's own
	 * confirming PING already reset the live one to 0 by the time this
	 * callback runs. */
	companion_print_link_log(NULL, cc3501e_link_log_recover_streak(ctx));
	/* #2136 review (MAJOR follow-up): the probe's own PING failures never
	 * land in the ring (link_log_suppress, cc3501e_core.c), so surface them
	 * here instead -- an operator still needs to know the probe ran and how
	 * many of its own attempts failed before the reset landed.
	 * CC3501E_LINK_PROBE_TRIES itself (currently 24) is a private
	 * cc3501e_core.c constant, not exported here -- print the count alone
	 * rather than duplicate that magic number into this TU. */
	printk("cc3501e: recovery probe: %u PING(s) failed before the reset\n",
	       cc3501e_link_log_probe_fail_count(ctx));
	printk("cc3501e: link recovered by warm reset (#%u)\n", recover_count);
}
#endif

void alp_console_companion_set(cc3501e_t *ctx)
{
	companion_cc3501e = ctx;
#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	/* No-op on a NULL ctx (unbind) -- cc3501e_set_recover_callback() itself
	 * refuses a NULL ctx, so there is nothing to register the callback on. */
	cc3501e_set_recover_callback(ctx, companion_recover_notify, NULL);
#endif
}

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

	uint16_t     ver = 0;
	alp_status_t s   = cc3501e_get_version(companion_cc3501e, &ver);

	if (s != ALP_OK) {
		shell_error(sh, "get_version failed (%d)", (int)s);
		return -EIO;
	}
	/* Report the RELEASE version first and the wire version second (ADR 0033).
	 *
	 * This line is where the raw wire number leaked into customer-facing
	 * conversations: it printed "CC3501E protocol v9", so release notes and
	 * support threads quoted the wire integer as if it were the thing to match,
	 * when the number a customer can actually act on is the firmware SemVer
	 * from firmware-version.txt.  The wire version is a contract between this
	 * host library and the firmware -- worth showing, not worth leading with.
	 *
	 * GET_DIAG_INFO carries the release version; if it is unavailable (older
	 * firmware, or a transport hiccup) fall back to printing the wire version
	 * alone rather than inventing a release number. */
	const unsigned int wire_major = (unsigned int)ALP_CC3501E_PROTOCOL_VERSION_MAJOR(ver);
	const unsigned int wire_minor = (unsigned int)ALP_CC3501E_PROTOCOL_VERSION_MINOR(ver);

	alp_cc3501e_diag_info_t diag = { 0 };
	if (cc3501e_diag_info(companion_cc3501e, &diag) == ALP_OK) {
		/* fw_version packs (MINOR << 8) | PATCH and does NOT carry the major at
		 * all -- the firmware's CMake calls it "pre-1.0 packing" and derives it
		 * from firmware-version.txt that way.  The leading "0." below is
		 * therefore hardcoded because the wire genuinely does not transmit it.
		 *
		 * THAT MAKES THIS LINE WRONG THE DAY firmware-version.txt REACHES 1.0.0:
		 * a 1.2.3 firmware would print "fw 0.2.3".  Fixing it needs the wire
		 * field widened (a MINOR-class change under ADR 0033, since a host that
		 * ignores the extra byte keeps working), not a change here -- so this
		 * comment is the warning for whoever bumps that major. */
		shell_print(sh,
		            "fw 0.%u.%u  (wire %u.%u)",
		            (unsigned int)((diag.fw_version >> 8) & 0xFFu),
		            (unsigned int)(diag.fw_version & 0xFFu),
		            wire_major,
		            wire_minor);
	} else {
		shell_print(sh, "wire %u.%u  (release version unavailable)", wire_major, wire_minor);
	}

	if (wire_major == 0u) {
		/* Cannot happen against a firmware this host will talk to -- cc3501e_reset
		 * refuses major 0 -- but `ver` is also used as a bare liveness probe on a
		 * context that never passed the gate, so say what it means rather than
		 * printing "wire 0.9" and leaving the reader to guess. */
		shell_warn(sh,
		           "this firmware predates the MAJOR.MINOR scheme (raw protocol %u)",
		           (unsigned int)ver);
	}

	uint32_t caps = 0u;
	if (cc3501e_get_capabilities(companion_cc3501e, &caps) == ALP_OK) {
		shell_print(sh, "caps 0x%08x", (unsigned int)caps);
	}
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
	alp_status_t s = cc3501e_ping(companion_cc3501e);
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
 * workqueue), AFTER cc3501e_poll_events()'s own cc3501e_request() call has
 * returned and released ctx's transport lock -- so this callback may itself
 * drive another request (e.g. reading a status register in response to an
 * event) without self-deadlocking.  Print-only -- printk goes to the active
 * console backend, so it is safe off the shell thread. */
static volatile uint32_t companion_evt_seen;
static volatile bool     companion_attn_backoff;

static void companion_event_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	companion_evt_seen++;
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

/* Drain + dispatch pending events.  Shared by the timer-poll thread and the
 * opt-in IRQ workqueue -- the ONLY two callers of companion_drain_events(),
 * both in this file, so this lock stays file-local (issue #1116: unlike
 * every OTHER companion_bus_lock use this split used to have, this one is
 * NOT redundant against cc3501e_request()'s new internal transport lock --
 * that lock covers only the single cc3501e_request() call inside
 * cc3501e_poll_events(); the evt_busy check + the evt_buf walk-and-dispatch
 * loop around it are a SEPARATE critical section evt_busy alone does not
 * make safe against a genuinely concurrent second caller, per
 * cc3501e_poll_events()'s own @warning in <alp/chips/cc3501e/events.h>).
 * No-op until a companion is registered (the cb is attached lazily on first
 * poll). */
static bool companion_event_cb_set;
K_MUTEX_DEFINE(companion_events_lock);

static void companion_drain_events(void)
{
	if (companion_cc3501e == NULL) {
		return;
	}
	k_mutex_lock(&companion_events_lock, K_FOREVER);
	if (!companion_event_cb_set) {
		/* ADD, never replace (issue #1723): this runs after the application's
		 * main() has registered its own callback, and the old single-slot
		 * registration overwrote it -- the console then consumed every event
		 * and the application silently received none. */
		(void)cc3501e_add_event_callback(companion_cc3501e, companion_event_cb, NULL);
		companion_event_cb_set = true;
	}
	(void)cc3501e_poll_events(companion_cc3501e);
	k_mutex_unlock(&companion_events_lock);
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

/* Strong override of the driver's weak hook: the bridge is idle exactly when its
 * request lock is free, and that is the only window in which a rising edge on
 * this wire can mean "event pending" rather than "a transaction just re-armed".
 * cc3501e_core.c calls this around every request. */
void cc3501e_attn_set_armed(bool armed)
{
	if (!device_is_ready(companion_attn.port)) {
		return;
	}
	/* While backing off, refuse to ARM.  The wire is shared with READY flow
	 * control, so a transaction end is indistinguishable from an attention
	 * pulse; re-arming on each one lets the drain's own trailing READY rise
	 * re-trigger this path.  Measured: 11888 ISRs for 86 real events. */
	if (armed && companion_attn_backoff) {
		return;
	}
	(void)gpio_pin_interrupt_configure_dt(&companion_attn,
	                                      armed ? GPIO_INT_EDGE_TO_ACTIVE : GPIO_INT_DISABLE);
}

/* Long enough to break the self-feeding chain, short enough that a real event
 * behind a spurious one is not held up meaningfully. */
#define ALP_COMPANION_ATTN_BACKOFF_MS 50

static void companion_attn_rearm_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	companion_attn_backoff = false;
	cc3501e_attn_rearm_if_desired(companion_cc3501e);
}
static K_WORK_DELAYABLE_DEFINE(companion_attn_rearm_work, companion_attn_rearm_fn);

/* An explicit application arm overrides the back-off: drop the latch and cancel
 * the pending delayed re-arm so the arm below actually reaches the controller. */
void cc3501e_attn_clear_backoff(void)
{
	companion_attn_backoff = false;
	(void)k_work_cancel_delayable(&companion_attn_rearm_work);
}

static void companion_event_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	const uint32_t seen_before = companion_evt_seen;

	companion_drain_events();

	/* RE-ARM HERE.  The requests inside the drain only MASK the line
	 * (cc3501e_lock_acquire); nothing re-armed it, so the FIRST edge masked it
	 * permanently -- bench-measured as the ISR frozen at 1 across a 40 s armed
	 * window (#130).
	 *
	 * An EMPTY drain means that edge was flow control, not attention: back off
	 * so the chain (edge -> drain -> READY rise -> edge) breaks.  A drain that
	 * DID deliver re-arms immediately, keeping real events fast. */
	if (companion_evt_seen == seen_before) {
		companion_attn_backoff = true;
		k_work_reschedule(&companion_attn_rearm_work, K_MSEC(ALP_COMPANION_ATTN_BACKOFF_MS));
		return;
	}
	companion_attn_backoff = false;
	cc3501e_attn_rearm_if_desired(companion_cc3501e);
}
static K_WORK_DEFINE(companion_event_work, companion_event_work_fn);

static void companion_attn_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* MASK THE LINE BEFORE SCHEDULING, re-armed by the work item once the drain
	 * is done.  Without this the path livelocks the device (bench 2026-08-26:
	 * the app boots, registers, enters its soak and goes silent forever,
	 * reproducibly).
	 *
	 * The wire is shared with READY flow control, which is raised on EVERY
	 * bridge re-arm -- so the edge does not mean "event pending", it means
	 * "a transaction just finished".  The drain scheduled here does its OWN
	 * bridge I/O, which raises READY again, which re-enters this ISR: the path
	 * feeds itself, and it contends with the application's traffic while doing
	 * it.  "An empty ring answers with an empty list" does not save it -- the
	 * cost is the transaction, not the answer.
	 *
	 * Masking bounds it to ONE drain per re-arm instead of an unbounded chain.
	 * It does NOT make the edge meaningful: a genuinely idle-time-only
	 * attention signal still needs the line qualified (a distinguishable pulse
	 * width, a second wire, or arming this only while the host has no request
	 * in flight). See #130. */
	(void)gpio_pin_interrupt_configure_dt(&companion_attn, GPIO_INT_DISABLE);
	k_work_submit(&companion_event_work);
	/* Re-arming is the request lock's job (cc3501e_attn_set_armed), not this
	 * ISR's: the drain about to run will mask and re-arm around each of its own
	 * requests, and leaving it masked until the bridge is genuinely idle is what
	 * stops an active link from re-triggering this path. */
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
	int64_t      t0    = k_uptime_get();
	for (unsigned long i = 0; i < n; i++) {
		if (cc3501e_get_version(companion_cc3501e, &ver) != ALP_OK) {
			fails++;
		}
	}
	int64_t dt = k_uptime_get() - t0;
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
	alp_status_t s = cc3501e_soft_reset(companion_cc3501e);

	if (s != ALP_OK) {
		shell_error(sh, "soft reset failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "soft reset requested -- firmware reboots; link will drop");
	return 0;
}

/* ---- CC3501E warm-reset recovery (Alif companion, issue #2126) ---------- */
static int cmd_companion_recover(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	/* No probe first, no cooldown -- unlike cc3501e_link_check_and_recover()
	 * (the automatic path wired into the driver's own failure exits), this
	 * is an operator asking for the warm reset directly: go straight to
	 * cc3501e_recover(). #2126 review: it is NOT otherwise unconditional
	 * any more -- cc3501e_recover() itself now refuses (ALP_ERR_BUSY)
	 * while an OTA/update session is active (ctx->ota_session_active) or
	 * another recovery is already running on this ctx, and this manual
	 * call shares BOTH the cooldown bookkeeping and recover_count with the
	 * automatic path (see cc3501e_recover()'s own doc comment) -- it is no
	 * longer a separate, auto-only counter. */
	alp_status_t s = cc3501e_recover(companion_cc3501e);

	if (s == ALP_ERR_BUSY && companion_cc3501e->ota_session_active) {
		shell_error(sh, "recover refused: an OTA/update session is active");
		return -EBUSY;
	}
	if (s != ALP_OK) {
		shell_error(sh, "recover failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "recover OK (%u recovery(s) so far)", companion_cc3501e->recover_count);
	return 0;
}

/* ---- CC3501E link-failure ring (Alif companion, issue #2136) ------------ */
static int cmd_companion_linklog(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	companion_print_link_log(sh, cc3501e_link_log_fail_streak(companion_cc3501e));
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
SHELL_SUBCMD_ADD((alp, companion),
                 recover,
                 NULL,
                 "warm-reset the bridge link unconditionally (issue #2126)",
                 cmd_companion_recover,
                 1,
                 0);
SHELL_SUBCMD_ADD((alp, companion),
                 linklog,
                 NULL,
                 "dump the link-failure ring, oldest first (issue #2136)",
                 cmd_companion_linklog,
                 1,
                 0);
#endif

SHELL_SUBCMD_ADD((alp),
                 companion,
                 &alp_companion_subcmds,
                 "Companion chip bridge (GD32 / CC3501E)",
                 NULL,
                 1,
                 0);

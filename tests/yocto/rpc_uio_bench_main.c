/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-sdk #683 Path B, stage 5 -- RZ/V2N A55 <-> M33 OpenAMP LIVE-WIRE
 * bench binary.
 *
 * The M33 (examples/multicore/rpmsg-v2n/m33_sm) is silicon-proven alive
 * on the bench: heartbeating, resource table published at A55
 * 0x4F700000 (ver=1, one VIRTIO_ID_RPMSG vdev, vring0 da=0x4F800000,
 * vring1 da=0x4F850000), blocked in `rproc_virtio_wait_remote_ready()`
 * waiting for the A55 to attach. This binary IS that A55 attach --
 * cross-compiled STATIC aarch64, no `.so` deps, so a serial-only bench
 * can transfer + run it directly against `/dev/uio*`.
 *
 * @par Why this drives the REAL public <alp/rpc.h> surface, not the
 *      backend's internals directly
 * tests/yocto/rpc_uio_self_close.c (the existing off-hardware
 * regression test) `#include`s src/backends/rpc/yocto_uio_drv.c
 * directly and calls its static `y_open()`/`y_call()`/`y_shutdown()`/
 * `y_destroy()` functions by hand, bypassing src/rpc_dispatch.c
 * entirely -- that is fine there because its custom
 * `alp_rpc_close_finalize()` test double never has a genuinely
 * in-flight `y_call()` racing `y_destroy()`'s `free()` the way the real
 * dispatcher protects against.  Reading src/rpc_dispatch.c's
 * `_rpc_drain()` (and this backend's own `y_call()`, which holds
 * `ch->tx_mutex` for its ENTIRE blocking duration, including while
 * parked in `pthread_cond_wait()`) shows why that protection matters
 * for real: a self-close's `ops->shutdown()` broadcasts the sticky
 * `closing` flag and returns, but a thread blocked in `y_call()` still
 * needs a few more instructions to wake, observe `closing`, and
 * unwind back out through its two `pthread_mutex_unlock()` calls
 * before `ch` may be freed.  `alp_rpc_close_finalize()` ->
 * `_rpc_finalize()` -> `_rpc_drain()` is precisely what waits for that
 * unwind (via the dispatcher's per-op `chan_word` refcount) before
 * calling `ops->destroy()` -- skip the dispatcher and that wait is
 * gone, reopening the exact use-after-free window GHSA-xhm8-7f87-93q5
 * closed.  So this binary links the REAL src/rpc_dispatch.c +
 * src/backend.c and drives `alp_rpc_open()/subscribe()/send()/call()/
 * close()` -- the production API a real customer app would call --
 * with the backend's `y_open()` (this file's own diagnostic UIO
 * pre-check aside, see below) reaching the actual UIO devices and the
 * actual M33 peer.  "Not the test double" means: no
 * `fake_send_offchannel_raw()`, no `fake_irq_worker()` -- every byte
 * crosses the real MHU doorbell + real shared-memory vrings.
 *
 * @par Wire protocol note (load-bearing for the ECHO + CLOSE-RACE
 *      sections below)
 * examples/multicore/rpmsg-v2n/m33_sm/src/main.c's header comment
 * states it plainly: that firmware "deliberately bypasses <alp/rpc.h>"
 * and instead runs the vendor `rpmsg_sample_client` echo protocol --
 * it creates endpoint "rpmsg-service-0" at its OWN local address 1024
 * (`APP_EPT_ADDR`), remote ANY, and echoes back VERBATIM whatever
 * bytes it receives, with no method-name routing of its own.  Since
 * `<alp/rpc.h>`'s wire framing is just `<method>\0<payload>` (see
 * include/alp/rpc.h's "Framing" section), an echoed frame carries the
 * SAME method string we sent -- so `alp_rpc_call()`'s
 * method-name-matches-the-pending-call check is satisfied by the
 * M33's echo without the M33 knowing anything about `<alp/rpc.h>` at
 * all.  This is what makes the ECHO section below work using the
 * ordinary public API against this specific (Phase 1, raw-echo)
 * firmware image; a future framed-RPC-aware M33 image would not
 * change this file.
 *
 * @par The self-close scenario's real-hardware timing constraint
 * tests/yocto/rpc_uio_self_close.c's mock delivers an UNSOLICITED
 * "close_me" frame straight into `uio_ept_cb()` from its fake IRQ
 * thread -- simulating "the peer sent us a message we never asked
 * for".  The real M33 image is echo-only and never sends anything
 * unsolicited, so the ONLY way to get a "close_me" frame delivered to
 * OUR real `uio_ept_cb()` is to send it ourselves and wait for the
 * echo.  Combined with `y_call()` holding `ch->tx_mutex` for its WHOLE
 * blocking duration, this fixes the ordering that avoids a deadlock:
 * send the "close_me" trigger (a fast, non-blocking `alp_rpc_send()`)
 * BEFORE spawning the thread that starts the UINT32_MAX-timeout
 * `alp_rpc_call()` -- reversing that order would have the blocked
 * call hold `tx_mutex` forever, wedging our own trigger send behind
 * it.  Because the M33 processes/echoes frames in the order its
 * single worker thread received them (FIFO over one shared vring),
 * sending the trigger first also gives it the best real chance of
 * winning the round-trip race against the second call's own echo --
 * not a hard guarantee (real hardware scheduling can still surprise
 * us), so `run_self_close_scenario()` below retries a bounded number
 * of times and reports EVERY outcome plainly rather than silently
 * downgrading "the race didn't manifest this round" to a false PASS.
 * The safety property under test either way -- no hang, no
 * use-after-free, clean reopen -- is checked on every iteration
 * regardless of whether the harder race was actually hit.
 *
 * @par Doorbell wiring (bench-proven, #697)
 * The A55<->M33 MHU doorbell is ASYMMETRIC and was resolved on silicon:
 * forward A55->M33 is R_MHU_NS5.MSG_INT_SET -> the M33's own NVIC IRQ 293;
 * reverse M33->A55 is a CA55-routed MHU-B SWINT unit (12 -> GIC_SPI 404),
 * because the NS-channel RSP interrupt reaches no A55 GIC line.  See
 * src/backends/rpc/yocto_uio_drv.c's MHU register comment for the full map.
 * The forward kick slot stays env-overridable (ALP_UIO_MHU_KICK_SLOT, default
 * 0xA0 = R_MHU_NS5) for future SoM/channel variants; if ATTACH or ECHO
 * regresses, the verbose diagnostics below show how far the attach got.
 *
 * Build (STATIC aarch64 cross -- see the recipe in
 * tests/yocto/build_rpc_uio_bench_aarch64.sh, which documents the
 * exact toolchain + libopen_amp.a/libmetal.a/libsysfs.a inputs this
 * file needs): this is a standalone recipe outside the normal
 * find_package(open-amp)-based src/yocto/CMakeLists.txt flow, because
 * the aarch64 open-amp/libmetal static libs here were cross-built by
 * hand against a manually-assembled aarch64-linux-gnu sysroot (no
 * cross pkg-config/.pc files exist for it) -- see that script for the
 * full explanation.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <metal/device.h>
#include <metal/io.h>
#include <metal/sys.h>
#include <openamp/remoteproc.h>

#include <alp/peripheral.h>
#include <alp/rpc.h>

/* ------------------------------------------------------------------ */
/* Last-error plumbing (src/common/alp_z_last_error.h's contract)       */
/* ------------------------------------------------------------------ */
/*
 * src/rpc_dispatch.c calls alp_z_set_last_error()/alp_z_clear_last_error()
 * (declared in src/common/alp_z_last_error.h); the public alp_last_error()
 * reads whatever they stamped.  src/yocto/dispatch_last_error_shim.c
 * forwards these onto src/common/stub_backend.c's single canonical slot --
 * pulling that in here would drag the entire stub backend surface (every
 * OTHER class's NOSUPPORT stub) into a bench binary that only needs
 * <alp/rpc.h>, so this file supplies the same thread-local-storage pattern
 * src/zephyr/last_error.c uses directly instead, self-contained.
 */
static __thread alp_status_t g_last_err;

void alp_z_set_last_error(alp_status_t s)
{
	g_last_err = s;
}

void alp_z_clear_last_error(void)
{
	g_last_err = ALP_OK;
}

alp_status_t alp_last_error(void)
{
	return g_last_err;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int g_fail_count;

static void report(const char *scenario, bool ok, const char *detail)
{
	printf(
	    "[%s] %s%s%s\n", ok ? "PASS" : "FAIL", scenario, detail ? ": " : "", detail ? detail : "");
	fflush(stdout);
	if (!ok) {
		g_fail_count++;
	}
}

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static bool wait_until(atomic_bool *flag, long timeout_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
	while (!atomic_load(flag)) {
		if (now_ms() >= deadline) {
			return false;
		}
		sleep_ms(1);
	}
	return true;
}

/* Retries alp_rpc_open() for up to budget_ms -- the single-physical-link
 * backend (yocto_uio_drv.c's g_chan_claimed) only releases its claim
 * inside y_destroy(), which for a self-close runs asynchronously on the
 * backend's own rx thread (see this file's header comment) -- so a
 * reopen attempted the instant we observe "the close fired" can
 * legitimately race a still-unwinding teardown and see ALP_ERR_BUSY.
 * That is expected, not a bug; retry instead of treating it as fatal. */
static alp_rpc_channel_t *open_with_retry(const alp_rpc_config_t *cfg, long budget_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)budget_ms;
	for (;;) {
		alp_rpc_channel_t *ch = alp_rpc_open(cfg);
		if (ch != NULL) {
			return ch;
		}
		if (now_ms() >= deadline) {
			return NULL;
		}
		sleep_ms(5);
	}
}

/* ------------------------------------------------------------------ */
/* Section A -- ATTACH diagnostics (raw UIO pre-check, before the real  */
/* alp_rpc_open() attach below touches the same devices)                */
/* ------------------------------------------------------------------ */
/*
 * Mirrors src/backends/rpc/yocto_uio_drv.c's g_uio_regions table
 * (device names + ALP_UIO_* env overrides) for READ-ONLY diagnostic
 * purposes only -- kept in sync by hand since that table is
 * file-local (`static`) to the backend TU.  Every device opened here
 * is fully closed + metal_finish()'d before alp_rpc_open() runs
 * (metal_init() is refcounted -- see yocto_uio_drv.c's own header
 * comment -- so the two independent init/finish pairs are safe).
 */
struct diag_uio_region {
	const char *dt_name;
	const char *env_var;
};

static const struct diag_uio_region g_diag_regions[] = {
	{ "4f700000.rsctbl", "ALP_UIO_RSCTBL" },
	{ "4f701000.mhu-shm", "ALP_UIO_MHU_SHM" },
	{ "4f800000.vring-ctl0", "ALP_UIO_VRING_CTL0" },
	{ "4f850000.vring-ctl1", "ALP_UIO_VRING_CTL1" },
	{ "4f900000.vring-shm0", "ALP_UIO_VRING_SHM0" },
	{ "4fc00000.vring-shm1", "ALP_UIO_VRING_SHM1" },
	{ "10480000.mhu-uio", "ALP_UIO_MHU" },
};
#define DIAG_UIO_COUNT (sizeof(g_diag_regions) / sizeof(g_diag_regions[0]))

static const char *diag_uio_name(size_t i)
{
	const char *env = getenv(g_diag_regions[i].env_var);
	return (env != NULL && env[0] != '\0') ? env : g_diag_regions[i].dt_name;
}

/* Liveness beacon offsets -- see examples/multicore/rpmsg-v2n/m33_sm/
 * src/main.c's RSCTBL_BEACON_* macros (published at the TOP of the
 * rsctbl region, above the resource table itself). */
#define DIAG_BEACON_MAGIC_OFFSET     0xFF0u
#define DIAG_BEACON_VERSION_OFFSET   0xFF4u
#define DIAG_BEACON_HEARTBEAT_OFFSET 0xFF8u
#define DIAG_BEACON_MAGIC_EXPECT     0xA10D0683u

static uint32_t diag_read_u32(const void *base, uint32_t off)
{
	const volatile uint32_t *p = (const volatile uint32_t *)((const uint8_t *)base + off);
	return *p;
}

static void diag_dump_rsctbl(const void *base)
{
	const struct resource_table *rsc = (const struct resource_table *)base;
	printf("    rsctbl: ver=%u num=%u\n", rsc->ver, rsc->num);
	if (rsc->num >= 1) {
		const struct fw_rsc_vdev *vdev =
		    (const struct fw_rsc_vdev *)((const uint8_t *)base + rsc->offset[0]);
		printf("    vdev: type=%u id=%u notifyid=%u dfeatures=0x%x status=0x%x "
		       "num_of_vrings=%u\n",
		       vdev->type,
		       vdev->id,
		       vdev->notifyid,
		       vdev->dfeatures,
		       vdev->status,
		       vdev->num_of_vrings);
		for (uint8_t i = 0; i < vdev->num_of_vrings && i < 2u; ++i) {
			const struct fw_rsc_vdev_vring *vr = &vdev->vring[i];
			printf("    vring%u: da=0x%x align=%u num=%u notifyid=%u\n",
			       i,
			       vr->da,
			       vr->align,
			       vr->num,
			       vr->notifyid);
		}
	}

	uint32_t magic = diag_read_u32(base, DIAG_BEACON_MAGIC_OFFSET);
	uint32_t ver   = diag_read_u32(base, DIAG_BEACON_VERSION_OFFSET);
	uint32_t hb0   = diag_read_u32(base, DIAG_BEACON_HEARTBEAT_OFFSET);
	printf("    beacon: magic=0x%08x (expect 0x%08x) version=%u heartbeat=%u\n",
	       magic,
	       DIAG_BEACON_MAGIC_EXPECT,
	       ver,
	       hb0);
	if (magic == DIAG_BEACON_MAGIC_EXPECT) {
		/* ~1.2s heartbeat-delta liveness proxy: yocto_uio_drv.c doesn't
		 * expose a real MHU IRQ counter (the notification worker runs
		 * on libmetal's shared IRQ thread, opaque from here), so this
		 * substitutes the M33's own 1 Hz heartbeat word as visible,
		 * honest evidence the peer is alive right now -- not a
		 * replacement for a real per-IRQ count, just the closest
		 * thing this binary can read without touching backend
		 * internals. */
		sleep_ms(1200);
		uint32_t hb1 = diag_read_u32(base, DIAG_BEACON_HEARTBEAT_OFFSET);
		printf("    beacon: heartbeat after 1.2s = %u (delta=%d) -- %s\n",
		       hb1,
		       (int)(hb1 - hb0),
		       (hb1 != hb0) ? "M33 alive/ticking" : "NO CHANGE -- M33 may be stalled/faulted");
	}
}

/* @return true if every named UIO device opened. Never fatal on its own --
 *         ATTACH below is the real test; this is diagnostics only. */
static bool diag_uio_precheck(void)
{
	printf("[diag] raw UIO pre-check (read-only, before the real attach)\n");
	struct metal_init_params mp = METAL_INIT_DEFAULTS;
	if (metal_init(&mp) != 0) {
		printf("    metal_init() failed\n");
		return false;
	}

	bool                 all_ok     = true;
	struct metal_device *rsctbl_dev = NULL;
	struct metal_device *opened[DIAG_UIO_COUNT];
	memset(opened, 0, sizeof opened);

	for (size_t i = 0; i < DIAG_UIO_COUNT; ++i) {
		const char          *name = diag_uio_name(i);
		struct metal_device *dev  = NULL;
		int                  rc   = metal_device_open("platform", name, &dev);
		if (rc != 0) {
			printf("    [uio %zu/%zu] %-24s OPEN FAIL (rc=%d)\n", i + 1, DIAG_UIO_COUNT, name, rc);
			all_ok = false;
			continue;
		}
		printf("    [uio %zu/%zu] %-24s opened OK (irq_num=%d)\n",
		       i + 1,
		       DIAG_UIO_COUNT,
		       name,
		       dev->irq_num);
		opened[i] = dev;
		if (i == 0) {
			rsctbl_dev = dev; /* keep open a moment longer, see below */
		}
	}

	if (rsctbl_dev != NULL) {
		struct metal_io_region *io = metal_device_io_region(rsctbl_dev, 0);
		if (io != NULL && io->virt != NULL) {
			diag_dump_rsctbl(io->virt);
		} else {
			printf("    rsctbl: no mapped io region\n");
			all_ok = false;
		}
	}

	for (size_t i = 0; i < DIAG_UIO_COUNT; ++i) {
		if (opened[i] != NULL) {
			metal_device_close(opened[i]);
		}
	}
	metal_finish();
	return all_ok;
}

/* ------------------------------------------------------------------ */
/* Section B -- ATTACH + ECHO                                          */
/* ------------------------------------------------------------------ */
/*
 * `.dst_ept = 1024` matches examples/multicore/rpmsg-v2n/m33_sm/src/
 * main.c's APP_EPT_ADDR exactly -- the M33's sc_ept is created at LOCAL
 * address 1024, remote RPMSG_ADDR_ANY, so our side's `dst_ept` (the
 * peer address y_open() sends to) must equal that fixed value; `src_ept
 * = 0` lets the backend derive our own local address from the channel
 * name (fine, since the M33 accepts ANY remote). */
static const alp_rpc_config_t g_cfg = {
	.name    = "rpmsg-service-0",
	.src_ept = 0,
	.dst_ept = 1024,
};

static bool run_attach(alp_rpc_channel_t **out_ch)
{
	printf("[attach] alp_rpc_open(name=%s, dst_ept=%u)\n", g_cfg.name, g_cfg.dst_ept);
	alp_rpc_channel_t *ch = alp_rpc_open(&g_cfg);
	if (ch == NULL) {
		report("attach", false, alp_status_name(alp_last_error()));
		return false;
	}
	report("attach", true, "vdev/vring/ept up, MHU IRQ registered");
	*out_ch = ch;
	return true;
}

/* Round-trips a handful of growing payloads through the M33's echo
 * (see this file's header comment on the wire protocol) and confirms
 * byte-for-byte equality + zero errors. */
static bool run_echo(alp_rpc_channel_t *ch)
{
	static const size_t sizes[] = { 1, 4, 16, 64 };
	int                 errors  = 0;

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		size_t  n = sizes[i];
		uint8_t req[64];
		uint8_t resp[64];
		for (size_t b = 0; b < n; ++b) {
			req[b] = (uint8_t)(0xA0u + i * 16u + b);
		}
		size_t       resp_len = sizeof resp;
		alp_status_t rc       = alp_rpc_call(ch, "echo_test", req, n, resp, &resp_len, 3000u);
		if (rc != ALP_OK) {
			printf("    echo[%zu bytes]: alp_rpc_call failed: %s\n", n, alp_status_name(rc));
			errors++;
			continue;
		}
		if (resp_len != n || memcmp(req, resp, n) != 0) {
			printf("    echo[%zu bytes]: MISMATCH (resp_len=%zu)\n", n, resp_len);
			errors++;
			continue;
		}
		printf("    echo[%zu bytes]: round-trip OK\n", n);
	}

	char detail[64];
	snprintf(detail,
	         sizeof detail,
	         "%d/%zu payload sizes mismatched",
	         errors,
	         sizeof(sizes) / sizeof(sizes[0]));
	report("echo", errors == 0, detail);
	return errors == 0;
}

/* ------------------------------------------------------------------ */
/* Section C -- CLOSE-RACE (#677) over the LIVE transport               */
/* ------------------------------------------------------------------ */

struct call_arg {
	alp_rpc_channel_t *ch;
	const char        *method;
	alp_status_t       result;
	atomic_bool        done;
};

static void *call_thread(void *arg)
{
	struct call_arg *a = (struct call_arg *)arg;
	uint8_t          resp[8];
	size_t           resp_len = sizeof resp;
	a->result = alp_rpc_call(a->ch, a->method, NULL, 0, resp, &resp_len, UINT32_MAX);
	atomic_store(&a->done, true);
	return NULL;
}

/* 1. self-close-from-callback racing a concurrently-blocked call.       */

static atomic_bool g_selfclose_fired;

static void on_close_me(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	alp_rpc_channel_t *ch = (alp_rpc_channel_t *)user;
	atomic_store(&g_selfclose_fired, true);
	/* THE self-close under test: called from inside our own subscribed
	 * callback, on the real libmetal IRQ thread -- see this file's
	 * header comment for why the dispatcher's DEFERRED path (not the
	 * backend's internals directly) is what must run here. */
	alp_rpc_close(ch);
}

#define SELF_CLOSE_MAX_ATTEMPTS 8

static bool run_self_close_scenario(void)
{
	printf("[close-race] scenario 1: self-close-from-callback vs. concurrent "
	       "alp_rpc_call(UINT32_MAX)\n");
	for (int attempt = 1; attempt <= SELF_CLOSE_MAX_ATTEMPTS; ++attempt) {
		alp_rpc_channel_t *ch = open_with_retry(&g_cfg, 5000);
		if (ch == NULL) {
			report("close-race/self-close", false, "reopen failed before attempt");
			return false;
		}
		if (alp_rpc_subscribe(ch, "close_me", on_close_me, ch) != ALP_OK) {
			report("close-race/self-close", false, "subscribe failed");
			alp_rpc_close(ch);
			return false;
		}

		atomic_store(&g_selfclose_fired, false);
		/* "slow_" prefix: the M33 echo firmware deliberately delays echoing
		 * methods starting with "slow" (see m33_sm/src/main.c), so this blocked
		 * UINT32_MAX call stays in-flight while the fast "close_me" trigger below
		 * fires the self-close -- opening the GHSA-xhm8 self-close race that a
		 * plain fast-echo method closes before the window opens (#697). */
		struct call_arg carg = { .ch = ch, .method = "slow_selfclose", .done = false };

		/* Ordering is load-bearing -- see this file's header comment:
		 * send the trigger (fast, non-blocking) BEFORE spawning the
		 * thread that starts the UINT32_MAX-timeout call (which holds
		 * ch->tx_mutex for its whole duration). */
		alp_status_t send_rc = alp_rpc_send(ch, "close_me", NULL, 0);
		if (send_rc != ALP_OK) {
			printf("    attempt %d: trigger send failed (%s), retrying\n",
			       attempt,
			       alp_status_name(send_rc));
			alp_rpc_close(ch);
			continue;
		}

		pthread_t th;
		if (pthread_create(&th, NULL, call_thread, &carg) != 0) {
			report("close-race/self-close", false, "pthread_create failed");
			alp_rpc_close(ch);
			return false;
		}

		bool fired = wait_until(&g_selfclose_fired, 3000);
		/* Hard bound (task requirement): once the self-close fires, the
		 * concurrently-blocked call must unblock within 1s. */
		bool call_done = wait_until(&carg.done, fired ? 1000 : 3000);
		pthread_join(th, NULL);

		if (!call_done) {
			report("close-race/self-close", false, "blocked call did not return -- HANG");
			return false;
		}

		if (!fired) {
			printf("    attempt %d: self-close trigger's echo did not race the blocked "
			       "call (call finished as %s first) -- retrying\n",
			       attempt,
			       alp_status_name(carg.result));
			/* No hang, but the intended race wasn't exercised this
			 * round; the channel may already be closed by the
			 * fired-but-just-missed callback, or still open -- either
			 * way, force a clean close before retrying. */
			alp_rpc_close(ch);
			continue;
		}

		bool ok = (carg.result == ALP_ERR_NOT_READY);
		printf("    attempt %d: self-close fired, blocked call returned %s\n",
		       attempt,
		       alp_status_name(carg.result));

		alp_rpc_channel_t *re = open_with_retry(&g_cfg, 3000);
		ok                    = ok && (re != NULL);
		if (re != NULL) {
			alp_rpc_close(re);
		} else {
			printf("    attempt %d: reopen after self-close FAILED\n", attempt);
		}

		report("close-race/self-close", ok, ok ? "NOT_READY + clean reopen" : "see log above");
		return ok;
	}

	{
		char detail[96];
		snprintf(detail,
		         sizeof detail,
		         "race never manifested in %d attempts (no hang observed)",
		         SELF_CLOSE_MAX_ATTEMPTS);
		report("close-race/self-close", false, detail);
	}
	return false;
}

/* 2. external-close-vs-blocked-call: fully deterministic (see header    */
/*    comment -- y_shutdown()'s sticky-cancel unblocks the call purely   */
/*    via a local broadcast, no M33 round trip required).                */

struct closer_arg {
	alp_rpc_channel_t *ch;
	atomic_bool        done;
};

static void *closer_thread(void *arg)
{
	struct closer_arg *a = (struct closer_arg *)arg;
	alp_rpc_close(a->ch);
	atomic_store(&a->done, true);
	return NULL;
}

static bool run_external_close_scenario(void)
{
	printf("[close-race] scenario 2: external alp_rpc_close() vs. concurrent "
	       "alp_rpc_call(UINT32_MAX)\n");
	alp_rpc_channel_t *ch = open_with_retry(&g_cfg, 5000);
	if (ch == NULL) {
		report("close-race/external-close", false, "open failed");
		return false;
	}

	struct call_arg carg = { .ch = ch, .method = "extclose_method", .done = false };
	pthread_t       call_th;
	if (pthread_create(&call_th, NULL, call_thread, &carg) != 0) {
		report("close-race/external-close", false, "pthread_create (call) failed");
		alp_rpc_close(ch);
		return false;
	}
	sleep_ms(50); /* let the call genuinely reach its blocked wait */

	struct closer_arg closer = { .ch = ch, .done = false };
	pthread_t         closer_th;
	uint64_t          t0 = now_ms();
	if (pthread_create(&closer_th, NULL, closer_thread, &closer) != 0) {
		report("close-race/external-close", false, "pthread_create (closer) failed");
		return false;
	}

	bool     closer_done = wait_until(&closer.done, 2000);
	bool     call_done   = wait_until(&carg.done, 1000);
	uint64_t elapsed     = now_ms() - t0;
	pthread_join(closer_th, NULL);
	pthread_join(call_th, NULL);

	if (!closer_done || !call_done) {
		report(
		    "close-race/external-close", false, "close() or blocked call did not return -- HANG");
		return false;
	}

	bool ok = (carg.result == ALP_ERR_NOT_READY) && (elapsed < 1000u);
	printf("    external close took %llums, blocked call returned %s\n",
	       (unsigned long long)elapsed,
	       alp_status_name(carg.result));

	alp_rpc_channel_t *re = open_with_retry(&g_cfg, 3000);
	ok                    = ok && (re != NULL);
	if (re != NULL) {
		alp_rpc_close(re);
	}

	report("close-race/external-close",
	       ok,
	       ok ? "NOT_READY within bound + clean reopen" : "see log above");
	return ok;
}

/* ------------------------------------------------------------------ */

int main(void)
{
	/* Line-buffer stdout: on the bench, output is transferred over a
	 * serial console (no tty on the far end either), and this binary's
	 * whole job is to show its diagnostics even when it dies partway
	 * through an attach attempt -- a crash must not lose an unflushed
	 * block buffer's worth of output. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("=== alp-sdk #683 Path B: RZ/V2N A55<->M33 rpmsg bench ===\n");
	printf("ALP_UIO_MHU_KICK_SLOT=%s\n",
	       getenv("ALP_UIO_MHU_KICK_SLOT") ? getenv("ALP_UIO_MHU_KICK_SLOT")
	                                       : "(default 0xA0 = R_MHU_NS5)");

	diag_uio_precheck();

	alp_rpc_channel_t *ch = NULL;
	if (!run_attach(&ch)) {
		printf("=== ATTACH failed -- stopping here so the diagnostics above show exactly how "
		       "far this got ===\n");
		return 1;
	}

	run_echo(ch);
	alp_rpc_close(ch);

	run_self_close_scenario();
	run_external_close_scenario();

	printf("=== overall: %s (%d scenario failure%s) ===\n",
	       g_fail_count == 0 ? "PASS" : "FAIL",
	       g_fail_count,
	       g_fail_count == 1 ? "" : "s");
	return g_fail_count == 0 ? 0 : 1;
}

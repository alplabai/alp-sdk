/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for #747: RPMsg failed-open paths did not
 * destroy initialized pthread primitives.  src/backends/rpc/yocto_drv.c's
 * y_open() stages four pthread objects (tx_mutex, sub_mutex, call_mutex,
 * call_cond) plus a control fd, an endpoint (via RPMSG_CREATE_EPT_IOCTL),
 * an endpoint fd, a wake pipe, and an rx thread.  Before the fix, every
 * failure path past the pthread inits closed fds and freed the channel
 * directly, without ever calling pthread_mutex_destroy()/
 * pthread_cond_destroy() on the primitives a strictly earlier stage of
 * the SAME call had already initialized -- a leak on every failed open,
 * and (destroying a NEVER-initialized object would be, symmetrically)
 * undefined behaviour had the unwind gone the other way.
 *
 * This file #includes src/backends/rpc/yocto_drv.c directly (same
 * technique as tests/yocto/rpc_yocto_self_close.c -- see that file's own
 * header comment for the full rationale) to reach the file-local
 * `y_open()`, `rpc_be_open_fail()`, `struct rpc_be`, and the test-only
 * fault-injection hooks declared right above `y_open()` in that file:
 *   - `g_y_open_test_fail_at(stage)`: force exactly one staged
 *     init/open/create call in y_open() to behave as though it failed,
 *     without ever touching the real syscall/init for that stage.
 *   - `g_y_open_test_force_ept_create_ok`: RPMSG_CREATE_EPT_IOCTL has no
 *     real-device substitute on a host without actual OpenAMP/remoteproc
 *     hardware (unlike the two fd opens around it, which accept
 *     /dev/null via the existing ALP_RPMSG_CTRL_DEV / ALP_RPMSG_EPT_DEV
 *     overrides for a real, deterministic success) -- this flag lets a
 *     scenario driving a LATER stage's failure treat the ioctl as having
 *     succeeded.  Only THIS file defines ALP_RPC_YOCTO_OPEN_TEST_HOOKS
 *     (below, before the #include), which is what makes that flag exist
 *     in the compiled TU at all -- see its own doc comment in
 *     yocto_drv.c for why production and every OTHER test TU that
 *     includes that file never compile it in.
 *
 * Each scenario below drives exactly one stage of y_open()'s sequence to
 * fail (every EARLIER stage runs for real, or via ALP_RPMSG_CTRL_DEV=
 * /dev/null + the ioctl force-ok flag when a real /dev/rpmsg_ctrl0 isn't
 * available), and asserts:
 *   1. y_open() returns a real error (never ALP_OK) and never publishes
 *      `st->be_data` -- rpc_be_data_load() stays NULL.
 *   2. The process's open fd count (via /proc/self/fd) is unchanged
 *      after the failed open as before it -- proving every fd opened
 *      during the attempt got closed, not leaked.  Repeated
 *      (OPEN_FAIL_ITERATIONS times) per stage so a leak that is only
 *      one fd per call still shows up deterministically rather than
 *      needing a single lucky sample.
 *   3. pthread_mutex_init/_destroy and pthread_cond_init/_destroy calls
 *      made BY y_open()/rpc_be_open_fail() stay balanced (init count ==
 *      destroy count) after every single iteration -- see the counting
 *      interposition below.  This is the part of the contract fd-count
 *      and the return-code checks above do NOT cover.
 *
 * On sanitizers NOT proving this: an earlier version of this file's
 * comment claimed ASan/TSan (this file is wired into the
 * alp_test_rpc_asan_ubsan / alp_test_rpc_tsan targets -- see this
 * directory's CMakeLists.txt) "is what actually proves no pthread-
 * object leak".  That claim was checked empirically and is FALSE: a
 * standalone probe (3 mutexes + 1 cond initialized in a calloc'd
 * struct, then the struct is freed WITHOUT ever calling
 * pthread_mutex_destroy()/pthread_cond_destroy(), 50 iterations) built
 * with `-fsanitize=address,undefined` and separately with
 * `-fsanitize=thread` exits 0 silently under both -- glibc's pthread
 * primitives embedded in a struct carry no sanitizer-visible identity
 * once the backing memory is freed, so neither sanitizer flags an
 * undestroyed-but-initialized mutex/cond this way.  Do not rely on
 * ASan/TSan/UBSan for this contract; they were not observed to catch
 * it.  What DOES catch it in this file is #3 above: pthread_mutex_init/
 * pthread_cond_init/pthread_mutex_destroy/pthread_cond_destroy are
 * redirected (via the object-like macros right before the
 * yocto_drv.c #include below) to the counting wrappers defined in this
 * TU, so the shipped call sites in y_open()/rpc_be_open_fail()/
 * rpc_be_teardown() are the exact ones counted -- an init with no
 * matching destroy (or vice versa) is a direct, deterministic assertion
 * failure, not an inference from an absence of sanitizer output.
 */

#include <pthread.h>

/* Counting interposition for the #747 contract (see the header comment
 * above): these wrappers call the REAL pthread_*_init/_destroy (their
 * own bodies are compiled BEFORE the #define below takes effect, so the
 * calls inside them are NOT self-redirected) and just tally successes.
 * The #define block further down then redirects yocto_drv.c's own call
 * sites -- the ones under test -- onto these wrappers for the rest of
 * this translation unit. */
static int g_mutex_init_count    = 0;
static int g_mutex_destroy_count = 0;
static int g_cond_init_count     = 0;
static int g_cond_destroy_count  = 0;

static int counting_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
	int rc = pthread_mutex_init(m, attr);
	if (rc == 0) {
		++g_mutex_init_count;
	}
	return rc;
}

static int counting_pthread_mutex_destroy(pthread_mutex_t *m)
{
	int rc = pthread_mutex_destroy(m);
	if (rc == 0) {
		++g_mutex_destroy_count;
	}
	return rc;
}

static int counting_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
	int rc = pthread_cond_init(c, attr);
	if (rc == 0) {
		++g_cond_init_count;
	}
	return rc;
}

static int counting_pthread_cond_destroy(pthread_cond_t *c)
{
	int rc = pthread_cond_destroy(c);
	if (rc == 0) {
		++g_cond_destroy_count;
	}
	return rc;
}

/* From here down, every pthread_mutex_init/_destroy and
 * pthread_cond_init/_destroy call TEXT in the #include below (the
 * shipped yocto_drv.c source, not a copy) resolves to the counting
 * wrappers above instead of the real glibc symbols -- glibc's own
 * <pthread.h> is already fully parsed by this point (the #include
 * above), so its declarations are not affected, only the CALL SITES
 * inside yocto_drv.c's y_open()/rpc_be_open_fail()/rpc_be_teardown(). */
#define pthread_mutex_init    counting_pthread_mutex_init
#define pthread_mutex_destroy counting_pthread_mutex_destroy
#define pthread_cond_init     counting_pthread_cond_init
#define pthread_cond_destroy  counting_pthread_cond_destroy

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#define ALP_RPC_YOCTO_OPEN_TEST_HOOKS 1
#include "../../src/backends/rpc/yocto_drv.c"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

#define OPEN_FAIL_ITERATIONS 50

/* alp_rpc_close_finalize() is normally defined by src/rpc_dispatch.c;
 * this binary does not link alp::sdk (same rationale as
 * rpc_yocto_self_close.c's identical stub -- see that file's own
 * header comment), but rpc_rx_main()'s self-close epilogue still
 * references the symbol, so the link needs a definition even though no
 * scenario in this file drives the rx thread far enough to call it. */
void alp_rpc_close_finalize(void *owner)
{
	alp_rpc_backend_state_t *st = (alp_rpc_backend_state_t *)owner;
	if (st == NULL) return;
	y_destroy(st);
}

/* ------------------------------------------------------------------ */
/* Shared harness                                                      */
/* ------------------------------------------------------------------ */

/* Count this process's currently-open fds via /proc/self/fd -- a cheap,
 * dependency-free leak detector that needs no sanitizer.  '.' and '..'
 * plus the directory fd opendir() itself opens are excluded by closing
 * the DIR* before returning, so the count reflects only fds that were
 * open both before opendir() and after closedir(). */
static int count_open_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	ALP_ASSERT_TRUE(d != NULL);
	if (d == NULL) {
		return -1;
	}
	int            n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue; /* "." / ".." */
		}
		/* Exclude the fd backing this very opendir() call. */
		if (atoi(e->d_name) == dirfd(d)) {
			continue;
		}
		++n;
	}
	closedir(d);
	return n;
}

static alp_rpc_config_t make_cfg(const char *name)
{
	alp_rpc_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.name = name;
	return cfg;
}

/* Asserts g_mutex_init_count == g_mutex_destroy_count and
 * g_cond_init_count == g_cond_destroy_count -- the actual #747 contract:
 * every pthread primitive y_open() successfully initializes on a given
 * call is destroyed exactly once, by the time this is checked (either by
 * rpc_be_open_fail()'s unwind on a failed call, or by a real
 * y_destroy()/rpc_be_teardown() on a successful one).  Called after
 * EVERY iteration below, not just once at the end, so a leak on any
 * single call -- not just a cumulative one -- is caught precisely where
 * it happened. */
static void assert_pthread_primitives_balanced(const char *scenario_name, int iteration)
{
	if (g_mutex_init_count != g_mutex_destroy_count) {
		ALP_TEST_FAIL("%s: iteration %d: mutex init/destroy count mismatch: init=%d destroy=%d",
		              scenario_name,
		              iteration,
		              g_mutex_init_count,
		              g_mutex_destroy_count);
	} else {
		ALP_TEST_PASS();
	}
	if (g_cond_init_count != g_cond_destroy_count) {
		ALP_TEST_FAIL("%s: iteration %d: cond init/destroy count mismatch: init=%d destroy=%d",
		              scenario_name,
		              iteration,
		              g_cond_init_count,
		              g_cond_destroy_count);
	} else {
		ALP_TEST_PASS();
	}
}

/* Every scenario below points this at a function selecting exactly one
 * stage, drives y_open() OPEN_FAIL_ITERATIONS times, and asserts (a) the
 * expected error code every time, (b) `st.be_data` never gets published,
 * (c) the fd count is unchanged from before the run to after, and (d)
 * every pthread mutex/cond y_open() initialized on that call was
 * destroyed by the time it returns -- see this file's header comment. */
static void run_stage_fail_scenario(const char *scenario_name,
                                    bool (*fail_hook)(enum rpc_be_open_stage),
                                    bool         force_ept_create_ok,
                                    alp_status_t expected_err)
{
	g_y_open_test_fail_at             = fail_hook;
	g_y_open_test_force_ept_create_ok = force_ept_create_ok;

	int fds_before = count_open_fds();

	for (int i = 0; i < OPEN_FAIL_ITERATIONS; ++i) {
		alp_rpc_backend_state_t st;
		memset(&st, 0, sizeof st);
		alp_rpc_config_t   cfg = make_cfg("open_fail_probe");
		alp_capabilities_t caps;
		alp_status_t       rc = y_open(&cfg, &st, &caps);
		if (rc != expected_err) {
			ALP_TEST_FAIL(
			    "%s: iteration %d: expected rc=%d, got rc=%d", scenario_name, i, expected_err, rc);
		} else {
			ALP_TEST_PASS();
		}
		ALP_ASSERT_NULL(rpc_be_data_load(&st));
		assert_pthread_primitives_balanced(scenario_name, i);
	}

	int fds_after = count_open_fds();
	ALP_ASSERT_EQ_INT(fds_after, fds_before);

	g_y_open_test_fail_at             = NULL;
	g_y_open_test_force_ept_create_ok = false;
}

/* ------------------------------------------------------------------ */
/* Per-stage fault-injection predicates                                */
/* ------------------------------------------------------------------ */

static bool fail_tx_mutex(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_TX_MUTEX;
}
static bool fail_sub_mutex(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_SUB_MUTEX;
}
static bool fail_call_mutex(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_CALL_MUTEX;
}
static bool fail_call_cond(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_CALL_COND;
}
static bool fail_ctrl_open(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_CTRL_OPEN;
}
static bool fail_ept_create(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_EPT_CREATE;
}
static bool fail_ept_open(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_EPT_OPEN;
}
static bool fail_pipe(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_PIPE;
}
static bool fail_rx_thread(enum rpc_be_open_stage s)
{
	return s == RPC_BE_OPEN_STAGE_RX_THREAD;
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

/* Stage 1: tx_mutex init itself fails -- nothing was ever initialized,
 * so rpc_be_open_fail() must destroy NOTHING and just free `ch`. */
static void test_fail_tx_mutex(void)
{
	run_stage_fail_scenario("tx_mutex", fail_tx_mutex, false, ALP_ERR_NOMEM);
}

/* Stage 2: sub_mutex fails after tx_mutex succeeded -- exactly
 * tx_mutex must be destroyed. */
static void test_fail_sub_mutex(void)
{
	run_stage_fail_scenario("sub_mutex", fail_sub_mutex, false, ALP_ERR_NOMEM);
}

/* Stage 3: call_mutex fails after tx_mutex + sub_mutex succeeded --
 * both must be destroyed, in reverse order. */
static void test_fail_call_mutex(void)
{
	run_stage_fail_scenario("call_mutex", fail_call_mutex, false, ALP_ERR_NOMEM);
}

/* Stage 4: call_cond fails after all three mutexes succeeded -- all
 * three must be destroyed (call_cond itself was never initialized, so
 * must NOT be destroyed). */
static void test_fail_call_cond(void)
{
	run_stage_fail_scenario("call_cond", fail_call_cond, false, ALP_ERR_NOMEM);
}

/* Stage 5: the control-device open fails -- all four pthread
 * primitives (the full set) must be destroyed; no fd was ever opened
 * so rpc_be_open_fail()'s fd-close checks are all no-ops. */
static void test_fail_ctrl_open(void)
{
	run_stage_fail_scenario("ctrl_open", fail_ctrl_open, false, ALP_ERR_NOT_READY);
}

/* Stage 6: RPMSG_CREATE_EPT_IOCTL fails after a real ctrl_fd open
 * (ALP_RPMSG_CTRL_DEV=/dev/null, set by main() below, gives y_open() a
 * real, valid fd to open here without needing actual rpmsg hardware)
 * -- all four pthread primitives AND ctrl_fd must be unwound. */
static void test_fail_ept_create(void)
{
	run_stage_fail_scenario("ept_create", fail_ept_create, false, ALP_ERR_NOT_READY);
}

/* Stage 7: the endpoint-device open fails after ctrl_fd opened for
 * real and the ioctl was forced to succeed (no real rpmsg endpoint
 * exists on this host) -- all four pthread primitives AND ctrl_fd must
 * be unwound; ept_fd was never opened so must not be double-closed. */
static void test_fail_ept_open(void)
{
	run_stage_fail_scenario(
	    "ept_open", fail_ept_open, /*force_ept_create_ok=*/true, ALP_ERR_NOT_READY);
}

/* Stage 8: pipe() fails after ctrl_fd AND ept_fd opened for real
 * (ALP_RPMSG_EPT_DEV=/dev/null, set by main() below) and the ioctl was
 * forced to succeed -- all four pthread primitives AND both fds must
 * be unwound. */
static void test_fail_pipe(void)
{
	run_stage_fail_scenario("pipe", fail_pipe, /*force_ept_create_ok=*/true, ALP_ERR_IO);
}

/* Stage 9: pthread_create() fails after every other resource in the
 * sequence has been genuinely acquired -- the full teardown (all four
 * pthread primitives, ctrl_fd, ept_fd, AND both wake-pipe ends) must
 * run. */
static void test_fail_rx_thread(void)
{
	run_stage_fail_scenario("rx_thread", fail_rx_thread, /*force_ept_create_ok=*/true, ALP_ERR_IO);
}

/* ------------------------------------------------------------------ */
/* Happy-path sanity: proves the fault-injection scaffolding itself
 * doesn't mask a real success once the FAIL hook is cleared, and that
 * open()/subscribe show the channel is fully usable afterwards -- so
 * the scenarios above are proven to exercise a real failure branch,
 * not a permanently-broken open path.
 *
 * g_y_open_test_force_ept_create_ok stays true here (name says so,
 * unlike the earlier version of this test): this host has no real
 * /dev/rpmsg_ctrl0 / OpenAMP remoteproc endpoint for
 * RPMSG_CREATE_EPT_IOCTL to create for real, so that one stage is still
 * synthetic even on this "success" path -- see g_y_open_test_fail_at's
 * own doc comment in yocto_drv.c.  Every OTHER stage (the four pthread
 * inits, both fd opens via ALP_RPMSG_CTRL_DEV/ALP_RPMSG_EPT_DEV=
 * /dev/null, the pipe, and the rx thread) runs for real. */
/* ------------------------------------------------------------------ */

static void test_open_succeeds_with_fail_hook_cleared(void)
{
	g_y_open_test_fail_at             = NULL;
	g_y_open_test_force_ept_create_ok = true; /* still no real hardware on this host */

	alp_rpc_backend_state_t st;
	memset(&st, 0, sizeof st);
	st.owner               = &st;
	alp_rpc_config_t   cfg = make_cfg("open_fail_probe_ok");
	alp_capabilities_t caps;
	alp_status_t       rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(rpc_be_data_load(&st) != NULL);

	/* Tear back down through the normal shutdown/destroy path so this
	 * doesn't leak the one real success this test deliberately drives,
	 * and confirm the REAL teardown path (rpc_be_teardown(), not
	 * rpc_be_open_fail()) also leaves every primitive balanced. */
	ALP_ASSERT_TRUE(y_shutdown(&st) == ALP_RPC_SHUTDOWN_DONE);
	y_destroy(&st);
	ALP_ASSERT_NULL(rpc_be_data_load(&st));
	assert_pthread_primitives_balanced("open_succeeds", 0);

	g_y_open_test_force_ept_create_ok = false;
}

/* ------------------------------------------------------------------ */

int main(void)
{
	/* /dev/null always exists, opens O_RDWR cleanly, and needs no real
	 * OpenAMP/remoteproc hardware -- gives y_open()'s ctrl_fd/ept_fd
	 * opens a real fd to work with on any Linux host so scenarios 6-9
	 * can reach their own target stage without ever touching real
	 * rpmsg hardware. */
	setenv("ALP_RPMSG_CTRL_DEV", "/dev/null", 1);
	setenv("ALP_RPMSG_EPT_DEV", "/dev/null", 1);

	test_fail_tx_mutex();
	test_fail_sub_mutex();
	test_fail_call_mutex();
	test_fail_call_cond();
	test_fail_ctrl_open();
	test_fail_ept_create();
	test_fail_ept_open();
	test_fail_pipe();
	test_fail_rx_thread();
	test_open_succeeds_with_fail_hook_cleared();

	ALP_TEST_SUMMARY();
}

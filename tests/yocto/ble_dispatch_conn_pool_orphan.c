/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #1697: alp_ble_close() (src/ble_dispatch.c)
 * used to free the radio without walking _conn_pool, so a connection entry
 * could outlive the radio handle it back-points to.
 *
 * alp_ble_connect() stores a RAW back-pointer -- `c->state.radio = h` -- into
 * the conn slot.  Before the fix, close() called only _free_radio(h), leaving
 * any still-claimed conn slot holding state.radio into a radio slot that
 * _alloc_radio() is now free to recycle.  Reachable on the shipping Zephyr
 * backend with a plain open -> connect -> close -> reopen; no partial vendor
 * port needed.
 *
 * This file #includes src/ble_dispatch.c directly (same approach, and same
 * reason, as ble_dispatch_self_close.c in this directory) so it can drive the
 * real alp_ble_close() against a minimal FAKE backend and then inspect the
 * static _conn_pool the fix is supposed to have swept.  A black-box test is
 * not possible here: native_sim has no BLE controller, so alp_ble_open()
 * returns NULL and no connection can be established through the public API.
 *
 * Scenarios:
 *   1. test_close_releases_this_radios_conn_slots -- the issue reproduction.
 *      Without the fix the slot stays claimed and the assertion fires.
 *   2. test_close_leaves_another_radios_conns_alone -- the fix must sweep
 *      only ITS OWN radio's connections.  A conn belonging to a different
 *      radio must survive, or closing one radio would tear down another's
 *      peer links.
 *   3. test_close_disconnects_via_the_backend -- the sweep must go through
 *      the backend's disconnect op (so the peer link is actually dropped),
 *      not merely release the slot bookkeeping.
 *   4. test_self_close_releases_conn_slots -- issue #1644: the DEFERRED
 *      self-close path (alp_ble_scan_start(), re-entered from inside its own
 *      scan callback -- see ble_dispatch_self_close.c in this directory for
 *      the full mechanism) used to call _free_radio(h) with NO _conn_pool
 *      walk at all, because #1697 above only added the walk to
 *      alp_ble_close(). Same reproduction shape as scenario 1, but through
 *      the self-close entry point instead of a direct alp_ble_close() call.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "test_assert.h"

/* Two radio slots: scenario 2 needs a second, independent radio. */
#define CONFIG_ALP_SDK_MAX_BLE_HANDLES 2

#include "../../src/ble_dispatch.c"

static atomic_int g_disconnects;

static alp_status_t fake_disconnect(alp_ble_conn_state_t *state)
{
	(void)state;
	atomic_fetch_add(&g_disconnects, 1);
	return ALP_OK;
}

static void fake_close_noop(alp_ble_radio_state_t *state)
{
	(void)state;
}

/* One ops table serves both sides: alp_ble_conn_state_t and
 * alp_ble_radio_state_t each hold a `const alp_ble_ops_t *`, and
 * ->disconnect lives in that same table (src/backends/ble/ble_ops.h). */
static const alp_ble_ops_t fake_ops = {
	.close      = fake_close_noop,
	.disconnect = fake_disconnect,
};

/* Build an open radio by hand, exactly as ble_dispatch_self_close.c does:
 * refcount 1 is what a real alp_ble_open() publishes, and without it
 * alp_ble_close()'s #1118 decrement-and-check-last no-ops instead of tearing
 * down. */
static struct alp_ble *make_open_radio(void)
{
	struct alp_ble *h = _alloc_radio();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops;
	h->refcount  = 1u;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

static struct alp_ble_conn *make_conn_on(struct alp_ble *h)
{
	struct alp_ble_conn *c = _alloc_conn();
	ALP_ASSERT_TRUE(c != NULL);
	if (c == NULL) {
		/* Pre-fix, the leaked slots exhaust the pool by the third
		 * scenario.  Report and unwind instead of dereferencing NULL:
		 * a regression must fail the test, not crash it. */
		return NULL;
	}
	c->state.ops   = &fake_ops;
	c->state.radio = h; /* the raw back-pointer this issue is about */
	alp_lifecycle_set(&c->lifecycle, ALP_HANDLE_LC_OPEN);
	return c;
}

static void test_close_releases_this_radios_conn_slots(void)
{
	atomic_store(&g_disconnects, 0);
	struct alp_ble      *h = make_open_radio();
	struct alp_ble_conn *c = make_conn_on(h);
	if (c == NULL) {
		alp_ble_close(h);
		return;
	}

	ALP_ASSERT_TRUE(alp_slot_is_claimed(&c->in_use));
	alp_ble_close(h); /* close WITHOUT disconnecting first */

	/* Pre-fix this slot is still claimed and still points at a radio slot
	 * that _alloc_radio() may now hand to someone else. */
	ALP_ASSERT_TRUE(!alp_slot_is_claimed(&c->in_use));
	printf("ok 1 - close() released its radio's conn slot\n");
}

static void test_close_leaves_another_radios_conns_alone(void)
{
	atomic_store(&g_disconnects, 0);
	struct alp_ble *h1 = make_open_radio();
	struct alp_ble *h2 = make_open_radio();
	ALP_ASSERT_TRUE(h1 != h2);
	struct alp_ble_conn *c1 = make_conn_on(h1);
	struct alp_ble_conn *c2 = make_conn_on(h2);
	if (c1 == NULL || c2 == NULL) {
		alp_ble_close(h1);
		alp_ble_close(h2);
		return;
	}

	alp_ble_close(h1);

	ALP_ASSERT_TRUE(!alp_slot_is_claimed(&c1->in_use)); /* swept */
	ALP_ASSERT_TRUE(alp_slot_is_claimed(&c2->in_use));  /* untouched */
	ALP_ASSERT_TRUE(atomic_load(&g_disconnects) == 1);

	alp_ble_close(h2); /* tidy up so later scenarios start from a free pool */
	ALP_ASSERT_TRUE(!alp_slot_is_claimed(&c2->in_use));
	printf("ok 2 - close() sweeps only its own radio's connections\n");
}

static void test_close_disconnects_via_the_backend(void)
{
	atomic_store(&g_disconnects, 0);
	struct alp_ble *h = make_open_radio();
	if (make_conn_on(h) == NULL || make_conn_on(h) == NULL) {
		alp_ble_close(h);
		return;
	}

	alp_ble_close(h);

	/* Releasing the slot is not enough -- the peer link has to be dropped,
	 * which means going through the backend op for EVERY connection. */
	ALP_ASSERT_TRUE(atomic_load(&g_disconnects) == 2);
	printf("ok 3 - close() drops each peer link through the backend\n");
}

/* ------------------------------------------------------------------ */
/* 4. Issue #1644: the DEFERRED self-close path (alp_ble_scan_start(), */
/*    re-entered from inside its own scan callback) must sweep         */
/*    _conn_pool exactly like alp_ble_close() above.                   */
/* ------------------------------------------------------------------ */

static alp_ble_t *g_self_close_conn_handle;

static void self_close_conn_scan_cb(const alp_ble_scan_result_t *r, void *user)
{
	(void)r;
	(void)user;
	alp_ble_close(g_self_close_conn_handle); /* THE self-close under test */
}

static alp_status_t fake_scan_start_self_close_conn(alp_ble_radio_state_t *state,
                                                    bool                   active,
                                                    alp_ble_scan_cb_t      cb,
                                                    void                  *user)
{
	(void)state;
	(void)active;
	alp_ble_scan_result_t r;
	memset(&r, 0, sizeof(r));
	/* mirrors cc35_scan_start()'s synchronous fan-out, and
	 * ble_dispatch_self_close.c's identical fake_scan_start_self_close(). */
	cb(&r, user);
	return ALP_OK;
}

static const alp_ble_ops_t fake_ops_self_close_conn = {
	.scan_start = fake_scan_start_self_close_conn,
	.close      = fake_close_noop,
	.disconnect = fake_disconnect,
};

static void test_self_close_releases_conn_slots(void)
{
	atomic_store(&g_disconnects, 0);
	struct alp_ble *h = _alloc_radio();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_self_close_conn;
	h->refcount  = 1u; /* issue #1118: see make_open_radio() above. */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_self_close_conn_handle = h;

	struct alp_ble_conn *c = make_conn_on(h);
	if (c == NULL) {
		alp_ble_close(h);
		return;
	}
	ALP_ASSERT_TRUE(alp_slot_is_claimed(&c->in_use));

	alp_status_t rc = alp_ble_scan_start(h, true, self_close_conn_scan_cb, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	/* Pre-#1644 fix: the self-close branch at the end of
	 * alp_ble_scan_start() called _free_radio(h) with no _conn_pool walk
	 * at all, so this slot stayed claimed -- pointing at a radio slot
	 * _alloc_radio() may now recycle underneath it. */
	ALP_ASSERT_TRUE(!alp_slot_is_claimed(&c->in_use));
	ALP_ASSERT_TRUE(atomic_load(&g_disconnects) == 1);
	ALP_ASSERT_EQ_INT(alp_lifecycle_get(&h->lifecycle), ALP_HANDLE_LC_UNOPENED);
	printf("ok 4 - self-close() releases its radio's conn slot\n");
}

int main(void)
{
	test_close_releases_this_radios_conn_slots();
	test_close_leaves_another_radios_conns_alone();
	test_close_disconnects_via_the_backend();
	test_self_close_releases_conn_slots();
	/* Non-zero exit on any failure -- a pre-fix build must FAIL the
	 * suite, not print failures and still return 0. */
	ALP_TEST_SUMMARY();
}

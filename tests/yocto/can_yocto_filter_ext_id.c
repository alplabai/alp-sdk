/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for the Yocto CAN backend's ext_id filter rule
 * (include/alp/can.h alp_can_filter_t::ext_id): a filter with
 * ext_id=false must match standard 11-bit frames only, a filter with
 * ext_id=true 29-bit frames only.  Before the fix the kernel mask for
 * ext_id=false lacked CAN_EFF_FLAG, so a 29-bit frame whose low 11 bits
 * equalled the filter id was delivered to the 11-bit filter.
 *
 * Drives the real y_add_filter() (kernel filter build) and
 * _dispatch_rx() (software match) by #including yocto_drv.c; no socket
 * is needed (apply-filters hook stubbed, rx_running pre-set so no
 * reader thread is spawned).
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_yocto_filter_ext_id
 *   ctest --test-dir build -R alp_test_can_yocto_filter_ext_id
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* the included yocto_drv.c uses pipe2() */
#endif

#include <string.h>

#include <linux/can.h>

#include "test_assert.h"

#include "../../src/backends/can/yocto_drv.c"

/* The _rx_loop() epilogue in yocto_drv.c calls this dispatch-layer hook
 * (src/can_dispatch.c).  This test exercises only filter matching and
 * never closes a handle, so a no-op keeps it linkable without pulling in
 * the whole dispatch layer. */
void alp_can_close_finalize(void *owner)
{
	(void)owner;
}

static int g_hits;

static int fake_apply_filters(y_can_data_t *d, const struct can_filter *set, size_t n)
{
	(void)d;
	(void)set;
	(void)n;
	return 0;
}

static void count_cb(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
	++g_hits;
}

/* Deliver one classic frame with raw kernel can_id to a handle carrying
 * a single filter; return how many times the filter cb fired. */
static int deliver(bool filter_ext, canid_t raw_can_id)
{
	alp_can_backend_state_t st;
	memset(&st, 0, sizeof(st));
	y_can_data_t *d = (y_can_data_t *)calloc(1, sizeof(*d));
	ALP_ASSERT_TRUE(d != NULL);
	pthread_mutex_init(&d->lock, NULL);
	d->rx_running = true; /* skip the lazy reader-thread spawn */
	st.be_data    = d;

	g_can_test_apply_filters_hook = fake_apply_filters;

	alp_can_filter_t flt = { .id = 0x123u, .mask = 0x7FFu, .ext_id = filter_ext };
	int32_t          id;
	ALP_ASSERT_EQ_INT(y_add_filter(&st, &flt, count_cb, NULL, &id), ALP_OK);

	struct can_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id  = raw_can_id;
	cf.can_dlc = 1;

	g_hits = 0;
	_dispatch_rx(d, &cf, (ssize_t)sizeof(cf));
	int hits = g_hits;

	g_can_test_apply_filters_hook = NULL;
	pthread_mutex_destroy(&d->lock);
	free(d);
	return hits;
}

static void test_std_filter_matches_std_frame(void)
{
	ALP_ASSERT_EQ_INT(deliver(false, 0x123u), 1);
}

static void test_std_filter_rejects_ext_frame_with_same_low_bits(void)
{
	/* 0x1FFFF123 & 0x7FF == 0x123, but it is a 29-bit frame. */
	ALP_ASSERT_EQ_INT(deliver(false, 0x1FFFF123u | CAN_EFF_FLAG), 0);
}

static void test_ext_filter_matches_ext_frame_only(void)
{
	ALP_ASSERT_EQ_INT(deliver(true, 0x123u | CAN_EFF_FLAG), 1);
	ALP_ASSERT_EQ_INT(deliver(true, 0x123u), 0);
}

int main(void)
{
	test_std_filter_matches_std_frame();
	test_std_filter_rejects_ext_frame_with_same_low_bits();
	test_ext_filter_matches_ext_frame_only();
	ALP_TEST_SUMMARY();
}

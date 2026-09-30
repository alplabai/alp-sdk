/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #2352 -- the E1M_X_CAN0/1 <-> can0/1
 * netdev swap in src/backends/can/yocto_drv.c's _can_netdev_index().
 * Before the fix, y_open() built the SocketCAN interface name straight
 * from the portable bus_id, so alp_can_open(E1M_X_CAN0) silently drove
 * the CAN1 pins on the E1M-X SoM family (rcar_canfd's channel PROBE
 * order swaps the visible can0/can1 relative to the portable
 * E1M_X_CAN0/1 numbering -- see _can_netdev_index()'s own comment).
 *
 * Drives the real _can_netdev_index() directly by #including
 * yocto_drv.c; same technique as can_yocto_filter_ext_id.c. Run
 * against the pre-fix file (bus_id passed straight to snprintf, no
 * swap) and this fails: index(0) comes back 0, not 1.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_yocto_netdev_swap
 *   ctest --test-dir build -R alp_test_can_yocto_netdev_swap
 */

#include "test_assert.h"

#include "../../src/backends/can/yocto_drv.c"

/* Same no-op stub as can_yocto_filter_ext_id.c: this test never closes
 * a handle, so the dispatch-layer hook _rx_loop() would call is never
 * reached, but it must still resolve to link. */
void alp_can_close_finalize(void *owner)
{
	(void)owner;
}

int main(void)
{
	/* The swapped pair: E1M_X_CAN0 (bus_id 0) opens can1, E1M_X_CAN1
	 * (bus_id 1) opens can0. */
	ALP_ASSERT_EQ_INT((int)_can_netdev_index(0u), 1);
	ALP_ASSERT_EQ_INT((int)_can_netdev_index(1u), 0);

	/* Any other bus_id (raw netdev access, not through the portable
	 * E1M enum) passes through unchanged. */
	ALP_ASSERT_EQ_INT((int)_can_netdev_index(2u), 2);
	ALP_ASSERT_EQ_INT((int)_can_netdev_index(7u), 7);

	ALP_TEST_SUMMARY();
}

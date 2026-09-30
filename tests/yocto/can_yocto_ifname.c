/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Coverage for the Yocto CAN backend's netdev name choice
 * (can_pick_ifname() in yocto_drv.c): can_e1m<N> wins when present;
 * the plain can<N> fallback must refuse an rcar_canfd netdev whose
 * dev_port says it is the OTHER E1M-X port (#2352), instead of silently
 * opening the wrong bus.  The helper is pure, so no CAN interface or
 * sysfs is needed.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_yocto_ifname
 *   ctest --test-dir build -R alp_test_can_yocto_ifname
 */

#include <string.h>

#include <linux/can.h>

#include "test_assert.h"

#include "../../src/backends/can/yocto_drv.c"

void alp_can_close_finalize(void *owner)
{
	(void)owner;
}

static void pick(uint32_t bus, bool e1m, int dev_port, alp_status_t want, const char *want_name)
{
	char name[IFNAMSIZ] = "";
	ALP_ASSERT_EQ_INT(can_pick_ifname(bus, e1m, dev_port, name, sizeof(name)), want);
	if (want == ALP_OK) ALP_ASSERT_TRUE(strcmp(name, want_name) == 0);
}

int main(void)
{
	pick(0, true, -1, ALP_OK, "can_e1m0");
	pick(1, true, 2, ALP_OK, "can_e1m1");
	/* No udev rename, dev_port unknown (vcan, USB adapter, host, or an
	 * rcar_canfd kernel without patch 0012): plain name. */
	pick(0, false, -1, ALP_OK, "can0");
	pick(1, false, 0, ALP_OK, "can1");
	/* rcar_canfd without the rename: probe-order swapped -> must refuse. */
	pick(0, false, 2, ALP_ERR_NOT_READY, NULL); /* can0 = ch 2 = E1M_X_CAN1 */
	pick(1, false, 3, ALP_ERR_NOT_READY, NULL);
	/* Same netdev that does match the requested bus is fine. */
	pick(0, false, 3, ALP_OK, "can0");
	pick(1, false, 2, ALP_OK, "can1");
	/* Buses with no E1M-X mapping are never refused. */
	pick(2, false, 5, ALP_OK, "can2");

	ALP_TEST_SUMMARY();
}

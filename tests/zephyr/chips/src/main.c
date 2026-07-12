/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite root for the chip driver smoke tests (lsm6dso, ssd1306, and the
 * ~90 chips/blocks that followed).  Test cases live in the sibling
 * test_*.c translation units, grouped by device class -- sensors,
 * displays, cameras, accelerators, power, connectivity, security, the
 * GD32 bridge, misc platform ICs, cc3501e, audio, industrial, and
 * blocks.  Every ZTEST(alp_chips, ...) case across those TUs links into
 * the single suite defined here via ztest's linker-section
 * registration; do not define a second ZTEST_SUITE(alp_chips, ...)
 * anywhere else in this test.
 */

#include <zephyr/ztest.h>

ZTEST_SUITE(alp_chips, NULL, NULL, NULL, NULL, NULL);

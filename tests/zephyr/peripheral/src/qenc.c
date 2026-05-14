/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- quadrature-encoder wrapper NULL-cfg test.
 * Extracted from main.c in §C.16.  Bigger surface (encoder_id range,
 * count read-back, reset) lands as HW-gated tests in a follow-up
 * once a Zephyr emulator backend for the qdec class is available.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_qenc_null_cfg) {
    zassert_is_null(alp_qenc_open(NULL));
    /* qenc backend doesn't yet stamp last_error on NULL cfg —
     * this is a TODO retrofit; the test still passes on NULL
     * return. */
}

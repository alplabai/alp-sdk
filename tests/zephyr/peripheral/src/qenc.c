/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- quadrature-encoder wrapper tests.
 * §C.16 split + §C.22 thin-spot fills: every public function on
 * the qenc surface gets a NULL-handle / NULL-arg guard test.
 */

#include <zephyr/ztest.h>

#include "alp/counter.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_qenc_null_cfg) {
    zassert_is_null(alp_qenc_open(NULL));
    /* qenc backend doesn't yet stamp last_error on NULL cfg --
     * this is a TODO retrofit; the test still passes on NULL
     * return. */
}

ZTEST(alp_peripheral, test_qenc_get_position_null_handle_not_ready)
{
    int32_t pos = 99;
    zassert_equal(alp_qenc_get_position(NULL, &pos), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_qenc_get_position_null_out_invalid)
{
    zassert_equal(alp_qenc_get_position(NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_qenc_reset_position_null_handle_not_ready)
{
    zassert_equal(alp_qenc_reset_position(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_qenc_close_null_is_noop)
{
    alp_qenc_close(NULL);
}
